#ifndef D_INTERFACE_CART_H
#define D_INTERFACE_CART_H

// -----------------------------------------------------------------------------------------------------------------------------------------
// The CART: a companion board on a switched rail, powered and supervised by this node
// -----------------------------------------------------------------------------------------------------------------------------------------
//
// A cart is something a node tows: a board with its own operating system and its own job, that the
// node switches on now and again and switches off when the job is done. The worked case is a remote
// relay running off solar with a Pi Zero W and an LTE modem beside it -- the Pi is unpowered almost
// all the time, and every twelve hours or so the relay gives it power long enough to phone home.
// Nothing in here knows that; a cart is any board on a switched 5V line that can wiggle a pin.
//
// -----------------------------------------------------------------------------------------------------------------------------------------
// THE SWITCH
// -----------------------------------------------------------------------------------------------------------------------------------------
//
//     5V ─────┬─────────────────────────┬──────────────────────────────────────► this node (always on)
//             │                         │
//         [R1 100k]                 ┌───┴───┐
//             │                     │   S   │ Q1  P-MOSFET, LOGIC LEVEL, >=2A. AO3401 / DMG3415 (SOT-23),
//             ├─────────────────────┤ G     │
//             │                     │   D   │  NDP6020P (TO-220) if you would rather solder something
//         [C1 1u]                   └───┬───┘  you can hold. Oversized here and none the worse for it.
//             │                         │
//             ├──[R4 10k]──┐            ├──────────┬───────────────────► SW_5V ──► cart + whatever it powers
//             │            │            │          │
//             │        ┌───┴───┐    [C2 100u]  [C3 100n]
//             │        │   C   │        │          │
//             │        │       │ Q2     └────┬─────┘
//             │        │   E   │ NPN 2N2222A │
//             │        └─┬───┬─┘            GND
//             │          │   │
//            5V         GND  └──[R2 10k]──┬── CART_PWR  (out, this node)
//                                         │
//                                     [R3 100k]
//                                         │
//                                        GND
//
//     cart cartbeat pin ──[R5 1k]────┬──────────────────────────────────────────► CART_LIVE (in, this node)
//                                   │
//                               [R6 100k]
//                                   │
//                                  GND
//
//     cart GND ─────────────────────────────────────────────────────────────────── node GND   (common, required)
//
// WHY A P-FET ON THE HIGH SIDE and not a cheaper N-FET in the ground leg: the cart's ground has to
// stay tied to ours, because the cartbeat is a signal referenced to it. Switch the low side and the
// cart's ground floats up when it is off, its cartbeat pin sits at an undefined potential relative
// to our input, and current finds its way home through the signal wire instead.
//
// WHY BOTH R1 AND R3, which look redundant and are not. R1 holds the gate AT the source, which is
// what OFF means for a P-FET; R3 holds Q2's base down. Between this node powering up and cart_init()
// running, CART_PWR is an input and drives nothing, so without R3 the base floats and Q2 is at the
// mercy of leakage -- and R1 alone cannot help, because it is Q2 that decides whether the gate is
// pulled down. Each one covers a different half of "off unless we say otherwise", and that state has
// to hold through reset, flashing, brownout and the moments before any code runs.
//
// LOGIC LEVEL IS THE ONE SPECIFICATION THAT MATTERS on Q1. The gate swings to about -4.8V here
// (source at 5V, gate pulled down to Q2's saturation voltage), so a part characterised only at
// Vgs = -10V sits PARTLY enhanced -- warm, and worse, variably on. Look for an Rds(on) figure quoted
// at Vgs = -4.5V; everything else on the datasheet is margin, since 5V and 2A are nowhere near what
// any of these parts are rated for. No gate zener: Vgs(max) is ±20V against a 5V swing.
//
// WHY C1 AND R4 -- the soft start, and the reason this is not just a transistor. A board and a modem
// coming up together pull an inrush that the shared supply feels; unchecked it can brown out the
// node doing the switching, which drops CART_PWR, which cuts the cart mid-boot, which looks exactly
// like a cart that failed to boot. R4 limits how fast Q2 can discharge the gate and C1 sets the
// ramp: ~10ms here, which is slow next to an inrush and instant next to a boot. Turn-OFF runs
// through R1 alone (~100ms) and that slowness is harmless -- nothing is waiting on it.
//
// A BIGGER FET MAKES THE SOFT START MORE IMPORTANT, not less, which is the opposite of the instinct:
// a part rated at tens of amps will pass a far larger surge before anything in it limits the
// current. The timings do not change when you substitute one, though -- C1 at 1uF swamps any of
// these parts' gate capacitance (~1-2nF), so the ramp stays where it is drawn.
//
// WHY C2 AT THE LOAD: the modem's transmit peaks are much faster than anything upstream can answer,
// and they are what resets a board over long thin wiring. Put the bulk where the current is drawn.
//
// WHY R5 AND R6 ON THE INPUT. R6 is load-bearing: the cart's pin is an unpowered high-impedance node
// for almost all of this module's life, and a floating input beside a switching supply will read as
// a cartbeat -- which is to say, as a cart that is alive. hw_gpio_cfg_enable_input() enables no
// internal pull-down and an internal one would not survive deep sleep. R5 only limits what flows if
// the two ends ever drive against each other.
//
// Values are a starting point, not a design: any logic-level P-FET rated well past the load will do,
// and the resistors are ordinary. A packaged load switch with an enable pin and adjustable soft-start
// replaces Q1/Q2/R1/R4/C1 entirely and is worth it if the board is being laid out rather than wired.
//
// -----------------------------------------------------------------------------------------------------------------------------------------
// WHY A CARTBEAT AND NOT A LEVEL
// -----------------------------------------------------------------------------------------------------------------------------------------
//
// The cart beats a pin -- a CARTBEAT, which is a heartbeat from something you tow.
//
// The obvious design is the other one: a line the cart holds high while it is working. It does not
// work, and the reason is the reason every watchdog wants a kick rather than a flag: A LEVEL SAYS THAT SOMETHING
// SET THE PIN. AN EDGE RATE SAYS THAT SOFTWARE IS STILL RUNNING. A cart that wedges with its pin
// high is indistinguishable from one that is busy, and it is precisely the wedged one you are
// switching the power for.
//
// So the cart oscillates the line, and this module counts changes rather than reading a level.
// Nothing here is timing critical: a beat every one to five seconds against a tolerance of ten or
// fifteen is the intended scale, sampled by an ordinary tick. Call cart_tick() at least twice per
// beat period; a hundred-millisecond loop oversamples a one-second beat by ten and is ample.
//
// AND ONE CHANGE IS NOT A HEARTBEAT EITHER, which is the same mistake one level deeper. A cart that
// boots, drives the pin, and wedges produces exactly one edge -- and a single edge taken as proof
// of life would let that cart be recorded as a NORMAL window: alive, then quiet, then cut, with the
// failure counter cleared and the back-off never engaging. It would be switched on again on
// schedule for ever. Liveness therefore needs CART_BEATS_MIN changes, because two is the smallest
// number that can only come from something still running.
//
// THE HEARTBEAT IS ALSO THE HOLD-OPEN, which is the part worth noticing. There is no separate
// "stay up" signal and no second meaning encoded in the rate, because a cart that wants to stay up
// simply keeps beating. That puts the policy where the knowledge is: the cart is the only end that
// knows whether somebody is logged in, whether a transfer is half finished, whether the tunnel it
// opened is still carrying anything. This node keeps only the SAFETY limits -- a boot deadline and
// a backstop -- and the backstop is not the window length. It is what stops a wedged cart draining
// a battery, and should be set hours out, not minutes.
//
// -----------------------------------------------------------------------------------------------------------------------------------------
// WHY TWO LINES, AND WHAT THAT COSTS
// -----------------------------------------------------------------------------------------------------------------------------------------
//
// One out, one in, and the losses are deliberate rather than accidental:
//
//   - THERE IS NO WAY TO ASK. A third line could carry "please finish early" and let the cart shut
//     down cleanly before its backstop; without one, every close this node initiates is a hard cut.
//     That is survivable when the cart's root filesystem is read-only, and is not otherwise -- so a
//     cart that can be cut at any instant SHOULD have a read-only root. It will be, eventually, on
//     a supply that is only as reliable as the weather.
//
//   - FINISHED AND DEAD LOOK THE SAME. Both are "the beat stopped", and this module cannot tell
//     them apart. The ACTION is the same either way -- cut the power -- so nothing unsafe follows
//     from the ambiguity; what is lost is knowing, afterwards, which one a site has been doing.
//     The counters below narrow it a little: a cart that never beat at all is separated from one
//     that beat and then stopped, which distinguishes "it did not boot" from everything else.
//
// A third line buys both back and the pins are usually there. This is the two-line version because
// two is enough to be safe, and safe was the requirement.
//
// -----------------------------------------------------------------------------------------------------------------------------------------
// WIRING REQUIREMENTS -- these are load-bearing, not suggestions
// -----------------------------------------------------------------------------------------------------------------------------------------
//
// EN PULLED DOWN AT THE LOAD SWITCH. The cart must be OFF whenever this node is not deliberately
// holding it on: through reset, through flashing, through a brownout, through the window between
// power-up and cart_init(). A pull-down at the switch's enable pin is the only thing that covers
// all of those, because a GPIO is an input in every one of them.
//
// A PULL-DOWN ON CART_LIVE, ON THIS SIDE. The cart's pin floats while the cart is unpowered, and a
// floating input next to a switching supply will read as a cartbeat. hw_gpio_cfg_enable_input()
// does not turn on an internal pull-down and an internal one would not survive deep sleep anyway,
// so it has to be a resistor. The series resistor limits what flows if the cart ever drives the
// line while this node holds it.
//
// SOFT-START ON THE LOAD SWITCH. The inrush of a board plus a modem coming up on a shared solar
// supply can brown out the node doing the switching, which would drop EN, which would cut the cart
// mid-boot. An adjustable soft-start is worth more than the part costs.
//
// NOTHING DRIVES A LINE HIGH INTO AN UNPOWERED BOARD. With only these two signals that is free --
// the one output goes to a switch, not to the cart. Anything else added later (a USB data link, a
// serial console) has to be checked for it; phantom-powering a cart through its protection diodes
// wastes the battery this whole mechanism exists to save.
//
// -----------------------------------------------------------------------------------------------------------------------------------------
// WHAT THE CART HAS TO DO
// -----------------------------------------------------------------------------------------------------------------------------------------
//
//   1. Beat the pin, from early in boot until the work is finished. Early matters: the boot
//      deadline is generous but it is not infinite, and a cart that only starts beating once its
//      application is up has spent its whole boot against that budget.
//   2. Stop beating when it is done, then shut down. The settle below is the time between those two
//      -- it must be longer than the cart takes to get from "beat stopped" to "safe to cut".
//   3. Close itself before the backstop. The backstop is this node's protection against a wedged
//      cart, not a schedule; a cart that regularly hits it is a cart that is not managing itself.
//
// A CART THAT CAN SIGNAL ITS OWN HALT should do it on the same pin. If the last thing the cart does
// on the way down is pulse that line -- linux's `gpio-poweroff` overlay does exactly this, active
// 100ms / inactive 100ms / active -- then those pulses read as beats, the beat effectively runs
// until the true halt, and the settle starts from there instead of from wherever the beating
// process happened to be killed. Nothing here needs configuring for it: a beat is a beat.
//
// -----------------------------------------------------------------------------------------------------------------------------------------

#include <stdbool.h>
#include <stdint.h>

static const char *__tag_cart = "cart";

// -----------------------------------------------------------------------------------------------------------------------------------------

/* Every interval is milliseconds. The defaults suit a linux single-board computer on solar; a cart
   that boots in two seconds or runs for a day wants its own numbers. */
typedef struct {
    gpio_num_t pin_power; /* out -> load switch EN. High = the cart has power.                     */
    gpio_num_t pin_live;  /* in  <- the CARTBEAT. Needs an external pull-down (above).              */

    uint32_t interval_ms; /* between windows, measured from the last CUT, not the last open        */
    uint32_t boot_ms;     /* the first beat must arrive within this of power-on                    */
    uint32_t silent_ms;   /* no change for this long = finished, or dead                           */
    uint32_t settle_ms;   /* after the beat stops, wait this before cutting                        */
    uint32_t limit_ms;    /* BACKSTOP: cut even while beating. Hours, not minutes.                 */

    /* A cart that is not coming back should not be offered power on schedule for ever. Each
       consecutive failure adds this to the wait, up to the cap -- so a dead cart costs a little
       battery on a lengthening interval rather than the same battery for ever. */
    uint32_t retry_ms;
    uint8_t retry_max;
} cart_config_t;

#ifndef CART_INTERVAL_MS_DEFAULT
#define CART_INTERVAL_MS_DEFAULT (12u * 60u * 60u * 1000u) /* twice a day */
#endif
/*
 * THE LEADING ALLOWANCE. A kernel, a filesystem check and an init system all happen before anything
 * can wiggle a pin, so the first beat is minutes away, not seconds. Two of them here against the
 * ~15-30s a small board actually takes, because the cost is asymmetric: too long wastes a few mAh
 * on a cart that was never coming back, too short cuts a working cart mid-boot AND files it as a
 * failure, which engages the back-off and loses the next window too. Lengthen freely.
 */
#ifndef CART_BOOT_MS_DEFAULT
#define CART_BOOT_MS_DEFAULT (2u * 60u * 1000u)
#endif

/*
 * THE TRAILING ALLOWANCE IS TWO DIFFERENT THINGS, which is worth separating because they answer
 * different questions and want different numbers:
 *
 *   silent_ms  HOW LONG BEFORE WE BELIEVE IT STOPPED. Tolerance for a missed beat, a busy moment,
 *              a slow scheduler. Too short and ordinary jitter reads as a finished cart.
 *   settle_ms  HAVING BELIEVED IT, HOW LONG THE SHUTDOWN GETS. Nothing else stands between the last
 *              beat and the rail going down, so this is the one that protects the filesystem.
 *
 * They stack: a cart that stops beating has silent_ms + settle_ms before the cut. And if it pulses
 * the pin at its true halt, that lands inside the silence or the settle, is taken as a beat, and
 * the whole trailing allowance restarts from the halt rather than from whenever its beating process
 * happened to be killed -- which is the best case and costs nothing to allow for.
 */
#ifndef CART_SILENT_MS_DEFAULT
#define CART_SILENT_MS_DEFAULT (15u * 1000u) /* ~3 missed beats at the slow end of the intended rate */
#endif
#ifndef CART_SETTLE_MS_DEFAULT
#define CART_SETTLE_MS_DEFAULT (30u * 1000u) /* longer than a small board's shutdown, with room */
#endif
#ifndef CART_LIMIT_MS_DEFAULT
#define CART_LIMIT_MS_DEFAULT (2u * 60u * 60u * 1000u) /* the wedged-cart backstop, deliberately far out */
#endif
#ifndef CART_RETRY_MS_DEFAULT
#define CART_RETRY_MS_DEFAULT (12u * 60u * 60u * 1000u)
#endif
#ifndef CART_RETRY_MAX_DEFAULT
#define CART_RETRY_MAX_DEFAULT 4u /* so a dead cart settles at roughly one attempt every 2.5 days */
#endif

/* Changes before the cart counts as alive. Two: one edge is what a board produces by driving the
   pin once and then wedging, and calling that alive would file the window as a success. Raising it
   costs one beat period of the boot budget per extra beat and buys very little. */
#ifndef CART_BEATS_MIN
#define CART_BEATS_MIN 2u
#endif

#define CART_CONFIG_DEFAULTS(power, live) \
    (cart_config_t) { \
        .pin_power = (power), .pin_live = (live), .interval_ms = CART_INTERVAL_MS_DEFAULT, .boot_ms = CART_BOOT_MS_DEFAULT, .silent_ms = CART_SILENT_MS_DEFAULT, .settle_ms = CART_SETTLE_MS_DEFAULT, .limit_ms = CART_LIMIT_MS_DEFAULT, \
        .retry_ms = CART_RETRY_MS_DEFAULT, .retry_max = CART_RETRY_MAX_DEFAULT \
    }

// -----------------------------------------------------------------------------------------------------------------------------------------

typedef enum {
    CART_OFF = 0,  /* no power, waiting for the next window                       */
    CART_BOOTING,  /* powered, nothing heard yet, boot deadline running           */
    CART_RUNNING,  /* beating; it stays up as long as it keeps doing so           */
    CART_SETTLING, /* gone quiet, giving it the settle before the power goes      */
} cart_state_t;

/* Returned by cart_tick(), one per transition. Everything except NONE is worth counting and most
   are worth reporting: a site whose cart stopped booting is a site to visit. */
typedef enum {
    CART_EVENT_NONE = 0,
    CART_EVENT_OPENED,  /* power applied, the window has begun                     */
    CART_EVENT_LIVE,    /* the first beat: it booted                               */
    CART_EVENT_CLOSED,  /* it went quiet, settled, and the power is off -- normal  */
    CART_EVENT_NO_BOOT, /* the boot deadline passed in silence. Cut.               */
    CART_EVENT_OVERRAN, /* still beating at the backstop. Cut anyway.              */
} cart_event_t;

typedef struct {
    cart_config_t cfg;
    cart_state_t state;

    bool live_level;       /* the last sample, for change detection                */
    uint32_t live_edge_ms; /* when it last CHANGED -- the cartbeat, in one number   */
    uint8_t live_beats;    /* changes this window, saturating at CART_BEATS_MIN     */
    bool live_seen;        /* has it beaten ENOUGH to count as alive                */

    uint32_t opened_ms; /* when the power went on                                */
    uint32_t closed_ms; /* when it last went off; the interval runs from here    */
    uint32_t quiet_ms;  /* when the beat was last judged to have stopped         */

    uint8_t fails; /* consecutive windows that produced nothing useful             */

    uint32_t st_opened, st_closed, st_no_boot, st_overran;
} cart_t;

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline const char *cart_state_str(const cart_state_t s) {
    switch (s) {
    case CART_OFF:
        return "off";
    case CART_BOOTING:
        return "booting";
    case CART_RUNNING:
        return "running";
    case CART_SETTLING:
        return "settling";
    default:
        return "?";
    }
}

static inline const char *cart_event_str(const cart_event_t e) {
    switch (e) {
    case CART_EVENT_NONE:
        return "none";
    case CART_EVENT_OPENED:
        return "opened";
    case CART_EVENT_LIVE:
        return "live";
    case CART_EVENT_CLOSED:
        return "closed";
    case CART_EVENT_NO_BOOT:
        return "no-boot";
    case CART_EVENT_OVERRAN:
        return "overran";
    default:
        return "?";
    }
}

// -----------------------------------------------------------------------------------------------------------------------------------------

static inline void _cart_power(cart_t *const c, const bool on) {
    hw_gpio_set(c->cfg.pin_power, on);
}

/* OFF FIRST, BEFORE ANYTHING ELSE. Between this node powering up and this call, the cart's fate
   rests entirely on the pull-down at the load switch; the first thing to do with the pin is to
   agree with it. Configuring the output before driving it would leave a moment at whatever level
   the pad happens to hold. */
static inline void cart_init(cart_t *const c, const cart_config_t *const cfg, const uint32_t now_ms) {
    *c = (cart_t){ 0 };
    c->cfg = *cfg;
    hw_gpio_cfg_enable_output(c->cfg.pin_power);
    _cart_power(c, false);
    hw_gpio_cfg_enable_input(c->cfg.pin_live, false); /* the pull-down is external -- see the header */
    c->state = CART_OFF;
    c->closed_ms = now_ms; /* the first window is one interval away, not immediate */
    c->live_level = hw_gpio_get(c->cfg.pin_live);
    ESP_LOGI(__tag_cart, "init: power=%d live=%d, every %us, boot %us, silent %us, settle %us, limit %us", (int)c->cfg.pin_power, (int)c->cfg.pin_live, (unsigned)(c->cfg.interval_ms / 1000u), (unsigned)(c->cfg.boot_ms / 1000u),
             (unsigned)(c->cfg.silent_ms / 1000u), (unsigned)(c->cfg.settle_ms / 1000u), (unsigned)(c->cfg.limit_ms / 1000u));
}

static inline bool cart_is_open(const cart_t *const c) {
    return c->state != CART_OFF;
}

/* How long until the next window would open by itself. Back-off is applied here rather than to
   closed_ms, so that a cart which starts working again returns to the plain interval at once. */
static inline uint32_t cart_wait_ms(const cart_t *const c) {
    const uint8_t n = (c->fails < c->cfg.retry_max) ? c->fails : c->cfg.retry_max;
    return c->cfg.interval_ms + (uint32_t)n * c->cfg.retry_ms;
}

/* Open one now: a console command, a mesh request, a button, or the first window after a restart
   because somebody is probably standing at the site. */
static inline bool cart_open(cart_t *const c, const uint32_t now_ms) {
    if (cart_is_open(c))
        return false;
    c->state = CART_BOOTING;
    c->opened_ms = now_ms;
    c->live_seen = false;
    c->live_beats = 0;
    c->live_level = hw_gpio_get(c->cfg.pin_live);
    c->live_edge_ms = now_ms;
    _cart_power(c, true);
    c->st_opened++;
    ESP_LOGI(__tag_cart, "window: open (boot deadline %us)", (unsigned)(c->cfg.boot_ms / 1000u));
    return true;
}

/* Cut now. `why` is for the log only; the caller decides what it meant. */
static inline void cart_close(cart_t *const c, const uint32_t now_ms, const char *const why) {
    if (!cart_is_open(c))
        return;
    _cart_power(c, false);
    c->state = CART_OFF;
    c->closed_ms = now_ms;
    ESP_LOGI(__tag_cart, "window: closed after %us (%s)", (unsigned)((now_ms - c->opened_ms) / 1000u), (why != NULL) ? why : "asked");
}

// -----------------------------------------------------------------------------------------------------------------------------------------

/*
 * Sample, then decide. One event per call at most.
 *
 * The sample is a CHANGE, not a level, and the whole cartbeat reduces to one number: when the pin
 * last differed from what it was. Everything else is a comparison against that -- which is why the
 * beat rate does not have to be configured, only an outside bound on how long silence may last.
 */
static inline cart_event_t cart_tick(cart_t *const c, const uint32_t now_ms) {

    if (c->state != CART_OFF) {
        const bool now_level = hw_gpio_get(c->cfg.pin_live);
        if (now_level != c->live_level) {
            c->live_level = now_level;
            c->live_edge_ms = now_ms;
            if (!c->live_seen) {
                /* NOT YET ALIVE ON THE FIRST CHANGE. A board that drives the pin once on the way to
                   wedging gets exactly one, and it must not buy a clean window with it. */
                if (++c->live_beats < (uint8_t)CART_BEATS_MIN)
                    return CART_EVENT_NONE;
                c->live_seen = true;
                c->state = CART_RUNNING;
                ESP_LOGI(__tag_cart, "window: live after %us", (unsigned)((now_ms - c->opened_ms) / 1000u));
                return CART_EVENT_LIVE;
            }
            if (c->state == CART_SETTLING) {
                /* it spoke again inside the settle -- it was pausing, not finished. The halt pulse
                   of a shutting-down board arrives exactly like this, which is the point. */
                c->state = CART_RUNNING;
                ESP_LOGI(__tag_cart, "window: beating again, settle abandoned");
            }
        }
    }

    switch (c->state) {

    case CART_OFF:
        if ((now_ms - c->closed_ms) >= cart_wait_ms(c))
            return cart_open(c, now_ms) ? CART_EVENT_OPENED : CART_EVENT_NONE;
        return CART_EVENT_NONE;

    case CART_BOOTING:
        /* NEVER SPOKE. Distinct from every other ending, and the one worth acting on: a cart that
           cannot boot is a visit, not a retry. */
        if ((now_ms - c->opened_ms) >= c->cfg.boot_ms) {
            c->fails++;
            c->st_no_boot++;
            cart_close(c, now_ms, "no boot");
            ESP_LOGW(__tag_cart, "window: nothing heard in %us -- cut. %u consecutive, next in %us", (unsigned)(c->cfg.boot_ms / 1000u), (unsigned)c->fails, (unsigned)(cart_wait_ms(c) / 1000u));
            return CART_EVENT_NO_BOOT;
        }
        return CART_EVENT_NONE;

    case CART_RUNNING:
        /* THE BACKSTOP, and it fires against a cart that is still beating -- which means it is
           alive and not finishing, so the cut is hard by definition. Counted separately because a
           site that keeps doing this is misconfigured rather than broken. */
        if ((now_ms - c->opened_ms) >= c->cfg.limit_ms) {
            c->fails++;
            c->st_overran++;
            cart_close(c, now_ms, "backstop");
            ESP_LOGW(__tag_cart, "window: still beating at the %umin backstop -- cut", (unsigned)(c->cfg.limit_ms / 60000u));
            return CART_EVENT_OVERRAN;
        }
        if ((now_ms - c->live_edge_ms) >= c->cfg.silent_ms) {
            c->state = CART_SETTLING;
            c->quiet_ms = now_ms;
            ESP_LOGI(__tag_cart, "window: quiet for %us -- settling %us before the cut", (unsigned)(c->cfg.silent_ms / 1000u), (unsigned)(c->cfg.settle_ms / 1000u));
        }
        return CART_EVENT_NONE;

    case CART_SETTLING:
        /* The backstop still applies: a cart that goes quiet and never comes back must not hold the
           power through a settle that outlives it. */
        if ((now_ms - c->quiet_ms) >= c->cfg.settle_ms || (now_ms - c->opened_ms) >= c->cfg.limit_ms) {
            c->fails = 0; /* it booted, it worked, it stopped: that is a good window */
            c->st_closed++;
            cart_close(c, now_ms, "finished");
            return CART_EVENT_CLOSED;
        }
        return CART_EVENT_NONE;

    default:
        return CART_EVENT_NONE;
    }
}

#endif /* D_INTERFACE_CART_H */
