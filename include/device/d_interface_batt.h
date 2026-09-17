// -----------------------------------------------------------------------------------------------------------------------------------------
// Battery voltage divider
// -----------------------------------------------------------------------------------------------------------------------------------------
//
// A switched resistive divider from the pack to an ADC pin, on the BATTERY side of the 3V3
// regulator. Not every carrier has one fitted, so the first boot after a restart probes for it and
// records the answer in the RTC-retained state; when it is absent the ADC is never brought up
// again and the packet simply carries no battery field. Nothing else in the app changes.
//
//              VBAT+  (raw cell 3.0-4.2V)
//                │
//                ├──────────────┬─────────────────┬──────────────► to 3V3 reg input
//                               │                 │
//                           [R6 100k]         ┌───┴───┐
//                               │             │   E   │ Q2 2N3906 (PNP)
//                               └───+─────────┤ B     │
//                                   │         │   C   │
//                                   │         └───┬───┘
//                                   │             │
//                                   │             │
//                               [R5 100k]      [R1 330k]
//                                   │             │
//                             ┌─────┴─────┐       ├───────────+──► GPIO_ADC
// GPIO_EN ──[R3 10k]────+─────┤ B   C     │       │           │
//                       │     │           │    [R2 330k]   [C1 10n]
//                  [R4 100k]  │     E     │       │           │
//                       │     └─────┬─────┘      GND         GND
//                      GND         GND
//                            Q1 2N2222A (NPN)
//
// WHY A PNP AND NOT A P-FET. The pack sits above the GPIO rail, so the high-side switch cannot be
// driven from a pin directly -- turning it OFF means pulling its control terminal up to VBAT+,
// which a 3V3 output cannot do. Hence Q1 as an inverting level shifter.
//
// WHY R4 SITS ON THE BASE, not at the enable pin: the divider must be off when the pin is an input
// during boot and when it floats in deep sleep, and a pull-down at the junction itself also holds
// Q1 off against base leakage at temperature. It is load-bearing -- hw_gpio_cfg_enable_output()
// does not enable an internal pull-down, and an internal one would not survive deep sleep anyway.
//
// WHY C1 IS 10nF. 330k alone cannot source the ADC's sample-and-hold charge, so C1 sits across R2
// and supplies it. Its value sets both settling paths, and the two are NOT the same:
//
//   turn-on   tau = (R1||R2) * C1 = 165k * C1  -- a stiff source charging the tap
//   turn-off  tau =  R2      * C1 = 330k * C1  -- Q2 is high-Z, so R2 alone drains it
//
// The SLOWER one is the off path, and it is the one BATTERY_SETTLE_MS has to satisfy, because
// battery_status() reads the pin after switching off and requires it back at ground. At 100nF that
// is 94ms to clear the threshold and the old constant waited 50, which reported a good divider as
// "switch stuck on"; the same 50ms was only 3 tau on the on path, so a full cell read ~3% low and
// reported 87%. 10nF puts both paths inside 25ms with margin, and the settle below is DERIVED from
// the value so changing the cap cannot silently break either one again.
//
// -----------------------------------------------------------------------------------------------------------------------------------------

static const char *__tag_batt = "battery";

// -----------------------------------------------------------------------------------------------------------------------------------------

typedef enum {
    BATTERY_TYPE_LIION = 0, /* 18650 and friends: 3.0-4.2V, the piecewise curve below */
    BATTERY_TYPE_LIPO,      /* same chemistry, same curve */
    BATTERY_TYPE_LIFEPO4,   /* 2.5-3.65V and much flatter: linear is as good as anything */
    BATTERY_TYPE_UNKNOWN,
} battery_type_t;

typedef enum {
    BATTERY_STATE_CRITICAL = 0,
    BATTERY_STATE_LOW,
    BATTERY_STATE_WARNING,
    BATTERY_STATE_GOOD,
    BATTERY_STATE_FULL,
    BATTERY_STATE_UNKNOWN,
} battery_state_t;

typedef enum {
    BATTERY_STATUS_OK = 0,
    BATTERY_STATUS_NO_DIVIDER,    /* the pin cannot follow the enable line at all: nothing fitted */
    BATTERY_STATUS_SWITCH_STUCK,  /* the pin will not return to ground: Q2 conducting when off */
    BATTERY_STATUS_NO_BATTERY,    /* the switch works, but there is nothing plausible behind it */
    BATTERY_STATUS_DIVIDER_FAULT, /* follows the enable, but to an implausible voltage */
    BATTERY_STATUS_ADC_FAULT,     /* the ADC would not answer, or would not repeat itself */
    BATTERY_STATUS_UNKNOWN,
} battery_status_t;

typedef struct {
    int voltage_mv;
    uint8_t percent;
    battery_state_t state;
    bool charging;
} battery_reading_t;

// -----------------------------------------------------------------------------------------------------------------------------------------

#ifndef BATTERY_CHARGING_HYSTERESIS_MV
#define BATTERY_CHARGING_HYSTERESIS_MV 20
#endif

#ifndef BATTERY_TYPE
#define BATTERY_TYPE BATTERY_TYPE_LIION
#endif

#ifndef BATTERY_DIVIDER_RATIO_X100
#define BATTERY_DIVIDER_RATIO_X100 200 /* (R1 + R2) / R2, x100 -- 330k / 330k = 2.00 */
#endif

#ifndef BATTERY_OFFSET_MV
#define BATTERY_OFFSET_MV 0
#endif
#ifndef BATTERY_DIVIDER_C1_NF
#define BATTERY_DIVIDER_C1_NF 10 /* the cap across R2*/
#endif

/* tau_off in microseconds = R2 * C1 = 330k * n nF = 330 * n us. Seven of them is 99.9% drained,
   and since tau_off is twice tau_on the same wait over-satisfies the turn-on path. */
#define BATTERY_TAU_OFF_US (330u * (unsigned)BATTERY_DIVIDER_C1_NF)
#define BATTERY_SETTLE_MS  (((BATTERY_TAU_OFF_US * 7u) / 1000u) + 2u)

#ifndef BATTERY_SAMPLE_COUNT
#define BATTERY_SAMPLE_COUNT 8
#endif

#define BATTERY_SAMPLE_INTERVAL_US 2000
#define BATTERY_ADC_UNIT           ADC_UNIT_1 /* ADC2 shares its hardware with the radio on some parts */
#define BATTERY_ADC_ATTEN          ADC_ATTEN_DB_12
#define BATTERY_ADC_BITWIDTH       ADC_BITWIDTH_12

/* Chemistry limits, and the two points where a Li-ion discharge curve bends. Overridable, because
   the useful values are properties of the INSTALLATION rather than of the cell:
     MAX  is what YOUR charger leaves the cell RESTING at, not the 4.20V it terminates charging at.
          A full cell settles at 4.15-4.18V once surface charge relaxes, so a 4200 ceiling means a
          full cell reads 96-98% forever. Measure it a few minutes after charge and use that.
     MIN  is where the NODE stops being useful, not the cell's 2.5V datasheet cutoff -- regulator
          dropout and the sag under a 22dBm transmit arrive long before that. 0% should mean "about
          to go quiet", which is what a gauge is for.
     KNEE and MID are the curve's shape and are chemistry, not installation. */
#ifndef BATTERY_LIION_MV_MIN
#define BATTERY_LIION_MV_MIN 3000
#endif
#ifndef BATTERY_LIION_MV_KNEE
#define BATTERY_LIION_MV_KNEE 3500
#endif
#ifndef BATTERY_LIION_MV_MID
#define BATTERY_LIION_MV_MID 3700
#endif
#ifndef BATTERY_LIION_MV_MAX
#define BATTERY_LIION_MV_MAX 4200
#endif
#ifndef BATTERY_LIFEPO4_MV_MIN
#define BATTERY_LIFEPO4_MV_MIN 2500
#endif
#ifndef BATTERY_LIFEPO4_MV_MAX
#define BATTERY_LIFEPO4_MV_MAX 3650
#endif

/* Adverse temperature widens the window rather than reporting a fault: a cold cell sags under load
   and a hot one can sit above its nominal ceiling, and neither is the pack being out of spec. */
#define BATTERY_COLD_MIN_DROP_MV   300
#define BATTERY_HOT_MAX_RISE_MV    150

#define BATTERY_PCT_CRITICAL       10
#define BATTERY_PCT_LOW            20 /* at or below this the gateway should hear about it */
#define BATTERY_PCT_WARNING        35
#define BATTERY_PCT_GOOD           80

/* Self-test limits, at the PIN (the seam hands back millivolts, not counts). Off should be within a
   few mV of ground after 7 tau; on is half a plausible pack, so 1500-2100mV. */
#define BATTERY_PROBE_OFF_MAX_MV   150
#define BATTERY_PROBE_DELTA_MIN_MV 300
#define BATTERY_PROBE_OFF_DIFF_MV  100  /* the two off readings disagreeing means an unstable ADC */
#define BATTERY_PROBE_MV_MIN       2000 /* at the PACK: below this there is no usable cell */
#define BATTERY_PROBE_MV_MAX       4500

#define BATTERY_TEST_JITTER_MAX_MV 200
#define BATTERY_TEST_CYCLE_MS      1000

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline const char *battery_type_str(const battery_type_t type) {
    switch (type) {
    case BATTERY_TYPE_LIPO:
        return "lipo";
    case BATTERY_TYPE_LIFEPO4:
        return "lifepo4";
    case BATTERY_TYPE_LIION:
        return "li-ion";
    case BATTERY_TYPE_UNKNOWN:
    default:
        return "unknown";
    }
}

static inline const char *battery_state_str(const battery_state_t state) {
    switch (state) {
    case BATTERY_STATE_CRITICAL:
        return "critical";
    case BATTERY_STATE_LOW:
        return "low";
    case BATTERY_STATE_WARNING:
        return "warning";
    case BATTERY_STATE_GOOD:
        return "good";
    case BATTERY_STATE_FULL:
        return "full";
    case BATTERY_STATE_UNKNOWN:
    default:
        return "unknown";
    }
}

static inline const char *battery_status_str(const battery_status_t status) {
    switch (status) {
    case BATTERY_STATUS_OK:
        return "ok";
    case BATTERY_STATUS_NO_DIVIDER:
        return "no-divider";
    case BATTERY_STATUS_SWITCH_STUCK:
        return "switch-stuck-on";
    case BATTERY_STATUS_NO_BATTERY:
        return "no-battery";
    case BATTERY_STATUS_DIVIDER_FAULT:
        return "divider-fault";
    case BATTERY_STATUS_ADC_FAULT:
        return "adc-fault";
    case BATTERY_STATUS_UNKNOWN:
    default:
        return "unknown";
    }
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void battery_range_mv(const battery_type_t type, const bool cold, const bool hot, int *const min_mv, int *const max_mv) {
    if (type == BATTERY_TYPE_LIFEPO4) {
        *min_mv = BATTERY_LIFEPO4_MV_MIN;
        *max_mv = BATTERY_LIFEPO4_MV_MAX;
    } else {
        *min_mv = BATTERY_LIION_MV_MIN;
        *max_mv = BATTERY_LIION_MV_MAX;
    }
    if (cold)
        *min_mv -= BATTERY_COLD_MIN_DROP_MV;
    if (hot)
        *max_mv += BATTERY_HOT_MAX_RISE_MV;
}

/* Piecewise, because a Li-ion cell does not discharge linearly: it falls quickly off the top, sits
   on a long plateau, then drops away below the knee. A straight line over the whole range reads
   ~50% for most of the life and then falls off a cliff. LiFePO4 is flat enough that the straight
   line is the honest answer. Integer throughout */
static inline uint8_t battery_percent_of(const int mv, const battery_type_t type, const bool cold, const bool hot) {

    int min_mv, max_mv;
    battery_range_mv(type, cold, hot, &min_mv, &max_mv);
    if (mv <= min_mv)
        return 0;
    if (mv >= max_mv)
        return 100;

    int pct;
    if (type == BATTERY_TYPE_LIFEPO4)
        pct = ((mv - min_mv) * 100) / (max_mv - min_mv);
    else if (mv > BATTERY_LIION_MV_MID)
        pct = 50 + ((mv - BATTERY_LIION_MV_MID) * 50) / (max_mv - BATTERY_LIION_MV_MID);
    else if (mv > BATTERY_LIION_MV_KNEE)
        pct = 20 + ((mv - BATTERY_LIION_MV_KNEE) * 30) / (BATTERY_LIION_MV_MID - BATTERY_LIION_MV_KNEE);
    else
        pct = ((mv - min_mv) * 20) / (BATTERY_LIION_MV_KNEE - min_mv);

    return (uint8_t)(pct < 0 ? 0 : (pct > 100 ? 100 : pct));
}

static inline battery_state_t battery_state_of(const uint8_t percent) {
    if (percent < BATTERY_PCT_CRITICAL)
        return BATTERY_STATE_CRITICAL;
    if (percent < BATTERY_PCT_LOW)
        return BATTERY_STATE_LOW;
    if (percent < BATTERY_PCT_WARNING)
        return BATTERY_STATE_WARNING;
    if (percent <= BATTERY_PCT_GOOD)
        return BATTERY_STATE_GOOD;
    return BATTERY_STATE_FULL;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline bool battery_begin(void) {

    hw_gpio_cfg_enable_output(PIN_BATTERY_EN);
    hw_gpio_set(PIN_BATTERY_EN, false);

    adc_unit_t unit;
    adc_channel_t channel;
    if (hw_adc_oneshot_channel(PIN_BATTERY_ADC, &unit, &channel) != ESP_OK)
        return false;
    if (unit != BATTERY_ADC_UNIT) {
        ESP_LOGE(__tag_batt, "begin: GPIO%d -> ADC%d, but expected ADC%d", PIN_BATTERY_ADC, (int)unit + 1, BATTERY_ADC_UNIT + 1);
        return false;
    }
    if (hw_adc_oneshot_start(unit, channel, BATTERY_ADC_ATTEN, BATTERY_ADC_BITWIDTH) != ESP_OK)
        return false;
    ESP_LOGD(__tag_batt, "begin: GPIO%d -> ADC%d channel %d, en=GPIO%d", PIN_BATTERY_ADC, (int)unit + 1, (int)channel, PIN_BATTERY_EN);
    if (!hw_adc_oneshot_calibrated())
        ESP_LOGW(__tag_batt, "begin: no eFuse calibration: voltages are approximate");

    return true;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void battery_end(void) {

    hw_gpio_set(PIN_BATTERY_EN, false);

    (void)hw_adc_oneshot_stop();
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline bool battery_sample_pin_mv(int *const out_mv) {
    int mv, sum = 0, count = 0;
    for (int i = 0; i < BATTERY_SAMPLE_COUNT; i++) {
        hw_delay_us_precise(BATTERY_SAMPLE_INTERVAL_US);
        if (hw_adc_oneshot_read_mv(&mv) == ESP_OK) {
            sum += mv;
            count++;
        }
    }
    if (count == 0)
        return false;
    *out_mv = sum / count;
    return true;
}

static inline int battery_measure_mv(void) {

    hw_gpio_set(PIN_BATTERY_EN, true);
    hw_delay_ms_yieldable(BATTERY_SETTLE_MS);

    int pin_mv;
    const bool ok = battery_sample_pin_mv(&pin_mv);

    hw_gpio_set(PIN_BATTERY_EN, false);

    return ok ? (((pin_mv * BATTERY_DIVIDER_RATIO_X100) / 100) + BATTERY_OFFSET_MV) : -1;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline bool battery_read(battery_reading_t *const out, int16_t *const previous_mv) {

    const int mv = battery_measure_mv();
    if (mv < 0)
        return false;

    out->voltage_mv = mv;
    out->percent = battery_percent_of(mv, BATTERY_TYPE, false, false);
    out->state = battery_state_of(out->percent);
    out->charging = (*previous_mv > 0) && (mv > (int)*previous_mv + BATTERY_CHARGING_HYSTERESIS_MV);
    *previous_mv = (int16_t)mv;

    char sb[CENTI_STR_MAX];
    ESP_LOGI(__tag_batt, "read: %sV (%u%%, %s)%s", centi_str(sb, sizeof(sb), mv / 10), (unsigned)out->percent, battery_state_str(out->state), out->charging ? " charging" : "");
    return true;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline battery_status_t battery_status(int *const out_pack_mv) {

    int off1 = 0, on = 0, off2 = 0;
    bool read = true;

    const uint32_t entered_ms = hw_time_ms();
    hw_gpio_set(PIN_BATTERY_EN, false);
    hw_delay_ms_yieldable(BATTERY_SETTLE_MS);
    read = read && (hw_adc_oneshot_read_mv(&off1) == ESP_OK);
    ESP_LOGD(__tag_batt, "status: en=0 -> %dmV (+%ums)", off1, (unsigned)(hw_time_ms() - entered_ms));
    hw_gpio_set(PIN_BATTERY_EN, true);
    hw_delay_ms_yieldable(BATTERY_SETTLE_MS);
    read = read && battery_sample_pin_mv(&on);
    ESP_LOGD(__tag_batt, "status: en=1 -> %dmV (+%ums)", on, (unsigned)(hw_time_ms() - entered_ms));
    hw_gpio_set(PIN_BATTERY_EN, false);
    hw_delay_ms_yieldable(BATTERY_SETTLE_MS);
    read = read && (hw_adc_oneshot_read_mv(&off2) == ESP_OK);
    ESP_LOGD(__tag_batt, "status: en=0 -> %dmV (+%ums)", off2, (unsigned)(hw_time_ms() - entered_ms));

    const int pack_mv = (on * BATTERY_DIVIDER_RATIO_X100) / 100;
    if (out_pack_mv != NULL)
        *out_pack_mv = read ? pack_mv : 0;
    if (!read)
        return BATTERY_STATUS_ADC_FAULT;

    ESP_LOGI(__tag_batt, "status: pin off=%dmV on=%dmV off=%dmV (%dmV at the pack)", off1, on, off2, pack_mv);

    const int off_diff = (off1 > off2) ? (off1 - off2) : (off2 - off1);
    const int off_hi = (off1 > off2) ? off1 : off2;
    const int delta = on - off1;

    if (delta < BATTERY_PROBE_DELTA_MIN_MV) {
        if (off_diff > BATTERY_PROBE_OFF_DIFF_MV)
            return BATTERY_STATUS_ADC_FAULT; /* wandering AND not following: nothing drives it */
        /* It ignores the enable. A stuck-ON switch would at least leave it at half a plausible
           pack; resting anywhere else means the pin is not looking at the divider at all. */
        if (off_hi > (BATTERY_PROBE_MV_MIN / 2) && off_hi < (BATTERY_PROBE_MV_MAX / 2))
            return BATTERY_STATUS_SWITCH_STUCK;
        return BATTERY_STATUS_NO_DIVIDER;
    }
    if (off_diff > BATTERY_PROBE_OFF_DIFF_MV)
        return BATTERY_STATUS_ADC_FAULT;
    if (off_hi > BATTERY_PROBE_OFF_MAX_MV)
        return BATTERY_STATUS_SWITCH_STUCK; /* it moves, but it never comes back to ground */
    if (pack_mv < BATTERY_PROBE_MV_MIN)
        return BATTERY_STATUS_NO_BATTERY;
    if (pack_mv > BATTERY_PROBE_MV_MAX)
        return BATTERY_STATUS_DIVIDER_FAULT;

    return BATTERY_STATUS_OK;
}

static inline bool battery_probe(void) {
    battery_status_t status = BATTERY_STATUS_UNKNOWN;
    if (battery_begin()) {
        int pack_mv = 0;
        switch (status = battery_status(&pack_mv)) {
        case BATTERY_STATUS_OK:
            ESP_LOGI(__tag_batt, "probe: divider present (%s, %dmV, adc=GPIO%d, en=GPIO%d)", battery_type_str(BATTERY_TYPE), pack_mv, PIN_BATTERY_ADC, PIN_BATTERY_EN);
            break;
        case BATTERY_STATUS_SWITCH_STUCK:
            ESP_LOGW(__tag_batt, "probe: the pin does not return to ground -- Q2 backwards (a P-FET's body diode, or an E/C swap), R2 missing, or C1 too large for BATTERY_SETTLE_MS");
            break;
        case BATTERY_STATUS_NO_DIVIDER:
            ESP_LOGW(__tag_batt, "probe: the pin ignores the enable and rests at an implausible level -- in order of likelihood: PIN_BATTERY_ADC is not the pin the tap is wired to (a floating input reads a few hundred mV and drifts), R2 "
                                 "missing so nothing pulls the tap down, EN not reaching Q1, or R5 landing on Q2's collector instead of its base");
            break;
        case BATTERY_STATUS_NO_BATTERY:
            ESP_LOGW(__tag_batt, "probe: the circuit works but the pack reads %dmV -- absent or dead cell (solar-only is fine, there is just nothing to report)", pack_mv);
            break;
        case BATTERY_STATUS_DIVIDER_FAULT:
            ESP_LOGW(__tag_batt, "probe: %dmV at the pack is not plausible -- wrong R1/R2 ratio for BATTERY_DIVIDER_RATIO_X100", pack_mv);
            break;
        case BATTERY_STATUS_ADC_FAULT:
            ESP_LOGW(__tag_batt, "probe: ADC fault");
        default:
            break;
        }
        battery_end();
    }
    return status == BATTERY_STATUS_OK;
}

// -----------------------------------------------------------------------------------------------------------------------------------------

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline bool battery_test(const uint32_t duration_ms) {

    bool ok = true;

    ESP_LOGI(__tag_batt, "test: %s, divider x%d.%02d, C1=%dnF -> settle=%ums (tau_off=%uus)", battery_type_str(BATTERY_TYPE), BATTERY_DIVIDER_RATIO_X100 / 100, BATTERY_DIVIDER_RATIO_X100 % 100, BATTERY_DIVIDER_C1_NF, BATTERY_SETTLE_MS,
             BATTERY_TAU_OFF_US);

    /* percentage curve: the ends clamp, the middle is monotonic */
    {
        int min_mv, max_mv;
        battery_range_mv(BATTERY_TYPE, false, false, &min_mv, &max_mv);
        const int probe[] = { min_mv - 500, min_mv, (min_mv + max_mv) / 2, max_mv, max_mv + 500 };
        const uint8_t lo[] = { 0, 0, 25, 100, 100 }, hi[] = { 0, 0, 75, 100, 100 };
        for (int i = 0; i < (int)(sizeof(probe) / sizeof(probe[0])); i++) {
            const uint8_t pct = battery_percent_of(probe[i], BATTERY_TYPE, false, false);
            if (pct < lo[i] || pct > hi[i]) {
                ESP_LOGE(__tag_batt, "test: curve FAIL %dmV -> %u%% (wanted %u-%u)", probe[i], (unsigned)pct, (unsigned)lo[i], (unsigned)hi[i]);
                ok = false;
                break;
            }
        }
        uint8_t last = 0;
        for (int mv = min_mv; mv <= max_mv; mv += 10) {
            const uint8_t pct = battery_percent_of(mv, BATTERY_TYPE, false, false);
            if (pct < last) {
                ESP_LOGE(__tag_batt, "test: curve FAIL not monotonic at %dmV (%u after %u)", mv, (unsigned)pct, (unsigned)last);
                ok = false;
                break;
            }
            last = pct;
        }
        /* a cold pack must widen the window, not report empty */
        if (battery_percent_of(min_mv - 100, BATTERY_TYPE, true, false) == 0) {
            ESP_LOGE(__tag_batt, "test: cold compensation FAIL (%dmV still reads 0%%)", min_mv - 100);
            ok = false;
        }
        ESP_LOGI(__tag_batt, "test: curve %s (%d..%dmV)", ok ? "ok" : "FAILED", min_mv, max_mv);
    }

    /* state thresholds */
    {
        const struct {
            uint8_t percent;
            battery_state_t want;
        } pts[] = {
            { 0, BATTERY_STATE_CRITICAL }, { 9, BATTERY_STATE_CRITICAL }, { 10, BATTERY_STATE_LOW },  { 19, BATTERY_STATE_LOW },  { 20, BATTERY_STATE_WARNING },
            { 34, BATTERY_STATE_WARNING }, { 35, BATTERY_STATE_GOOD },    { 80, BATTERY_STATE_GOOD }, { 81, BATTERY_STATE_FULL }, { 100, BATTERY_STATE_FULL },
        };
        for (int i = 0; i < (int)(sizeof(pts) / sizeof(pts[0])); i++)
            if (battery_state_of(pts[i].percent) != pts[i].want) {
                ESP_LOGE(__tag_batt, "test: state FAIL %u%% -> %s (wanted %s)", (unsigned)pts[i].percent, battery_state_str(battery_state_of(pts[i].percent)), battery_state_str(pts[i].want));
                ok = false;
            }
        if (ok)
            ESP_LOGI(__tag_batt, "test: state ok");
    }

    if (!battery_begin()) {
        ESP_LOGE(__tag_batt, "test: begin FAILED");
        return false;
    }
    ESP_LOGI(__tag_batt, "test: begin ok");

    /* hardware: the enable line must move the pin, and the self-test must agree */
    {
        int pack_mv = 0;
        const battery_status_t status = battery_status(&pack_mv);
        ESP_LOGI(__tag_batt, "test: status=%s (%dmV at the pack)", battery_status_str(status), pack_mv);
        if (status != BATTERY_STATUS_OK)
            ok = false;
        else if (BATTERY_OFFSET_MV == 0) /* only while this board is still uncalibrated */
            ESP_LOGW(__tag_batt, "test: calibration -- meter BAT+ now and set BATTERY_OFFSET_MV to (meter - %d)", pack_mv);
    }

    /* jitter: consecutive measurements of a DC voltage should agree */
    if (ok) {
        int lo = 100000, hi = 0;
        for (int i = 0; i < 5; i++) {
            const int mv = battery_measure_mv();
            if (mv < 0) {
                ok = false;
                break;
            }
            if (mv < lo)
                lo = mv;
            if (mv > hi)
                hi = mv;
        }
        if (ok) {
            ESP_LOGI(__tag_batt, "test: jitter %dmV (min=%dmV max=%dmV)", hi - lo, lo, hi);
            if ((hi - lo) > BATTERY_TEST_JITTER_MAX_MV) {
                ESP_LOGE(__tag_batt, "test: jitter FAIL (over %dmV -- C1 missing, or a noisy divider)", BATTERY_TEST_JITTER_MAX_MV);
                ok = false;
            }
        }
    }

    /* and then just read it, for as long as asked */
    if (ok && duration_ms > 0) {
        const uint32_t started_ms = hw_time_ms();
        int16_t previous_mv = 0;
        unsigned n = 0, errors = 0;
        while ((hw_time_ms() - started_ms) < duration_ms) {
            battery_reading_t reading = { 0 };
            if (!battery_read(&reading, &previous_mv) || reading.voltage_mv < BATTERY_PROBE_MV_MIN || reading.voltage_mv > BATTERY_PROBE_MV_MAX)
                errors++;
            n++;
            hw_delay_ms_yieldable(BATTERY_TEST_CYCLE_MS);
        }
        ESP_LOGI(__tag_batt, "test: %u reading(s), %u error(s)", n, errors);
        if (errors > 0)
            ok = false;
    }

    battery_end();
    ESP_LOGI(__tag_batt, "test: %s", ok ? "PASSED" : "FAILED");
    return ok;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
