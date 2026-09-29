
// ------------------------------------------------------------------------------------------------------------------------
// NMEA parsing (standard NMEA 0183)
// ------------------------------------------------------------------------------------------------------------------------

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wredundant-decls"
#include <time.h>
#pragma GCC diagnostic pop

#define _NMEA_SENTENCE_LENGTH_MIN (1 + 2 + 3 + 1 + 2) // $GPTXT*HH + \r\n
#define _NMEA_SENTENCE_LENGTH_MAX 128

// Match sentence type at fixed offset [3..5] (works for all talker IDs: GP, GN, BD, GL, GA)
#define NMEA_SENTENCE(l, c)       ((l)[3] == (c[0]) && (l)[4] == (c[1]) && (l)[5] == (c[2]))

// ------------------------------------------------------------------------------------------------------------------------

static inline uint8_t _nmea_hex_nibble(const char c) {
    if (c >= '0' && c <= '9')
        return (uint8_t)(c - '0');
    else if (c >= 'A' && c <= 'F')
        return (uint8_t)(c - 'A' + 10);
    else if (c >= 'a' && c <= 'f')
        return (uint8_t)(c - 'a' + 10);
    else
        return (uint8_t)0xFF; // invalid
}

bool nmea_verify_checksum(const char *const sentence) {
    if (sentence[0] != '$')
        return false;
    uint8_t calc = 0;
    const char *p = sentence + 1;
    while (*p && *p != '*')
        calc ^= (uint8_t)*p++;
    if (*p != '*')
        return false;
    if (p[1] == '\0' || p[2] == '\0')
        return false;
    const uint8_t hi = _nmea_hex_nibble(p[1]), lo = _nmea_hex_nibble(p[2]);
    if ((hi | lo) == 0xFF)
        return false;
    return calc == ((hi << 4) | lo);
}

// ------------------------------------------------------------------------------------------------------------------------

const int _nmea_timegm_days_in_month[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
#define _NMEA_TIMEGM_IS_LEAP(y)       (((y) + 1900) % 4 == 0) && (((y) + 1900) % 100 != 0 || ((y) + 1900) % 400 == 0)
#define _NMEA_TIMEGM_MONTH_DAYS(m, y) (((m) == 1 && _NMEA_TIMEGM_IS_LEAP(y)) ? 29 : _nmea_timegm_days_in_month[m])
void _nmea_timegm_normalize_unit(int *const unit, int *const next, const int base) {
    if (*unit >= base) {
        *next += *unit / base;
        *unit = *unit % base;
    } else if (*unit < 0) {
        *next += *unit / base;
        if ((*unit % base) < 0) {
            *unit = base + (*unit % base);
            *next -= 1;
        } else
            *unit = 0;
    }
}
void _nmea_timegm_normalize_days(int *const days, int *const month, int *const year) {
    while (*days <= 0) {
        if (*month > 0)
            *month -= 1;
        else {
            *year -= 1;
            *month = 11;
        }
        *days += _NMEA_TIMEGM_MONTH_DAYS(*month, *year);
    }
    int md;
    while (*days > (md = _NMEA_TIMEGM_MONTH_DAYS(*month, *year))) {
        if (*month == 11) {
            *month = 0;
            *year += 1;
        } else
            *month += 1;
        *days -= md;
    }
}
int64_t nmea_timegm(const struct tm *const tm) {
    int sec = tm->tm_sec, min = tm->tm_min, hour = tm->tm_hour, mday = tm->tm_mday, mon = tm->tm_mon, year = tm->tm_year;
    _nmea_timegm_normalize_unit(&sec, &min, 60);
    _nmea_timegm_normalize_unit(&min, &hour, 60);
    _nmea_timegm_normalize_unit(&hour, &mday, 24);
    _nmea_timegm_normalize_unit(&mon, &year, 12);
    _nmea_timegm_normalize_days(&mday, &mon, &year);
    int64_t ret = 0;
    if (year > 70)
        for (int x = 70; x < year; ++x)
            ret += (365 * 24 * 60 * 60) + (_NMEA_TIMEGM_IS_LEAP(x) ? (24 * 60 * 60) : 0);
    else if (year < 70)
        for (int x = 70 - 1; x >= year; --x)
            ret -= (365 * 60 * 60 * 60) + (_NMEA_TIMEGM_IS_LEAP(x) ? (24 * 60 * 60) : 0);
    for (int x = 0; x < mon; ++x)
        ret += (_nmea_timegm_days_in_month[x] + (x == 1 && _NMEA_TIMEGM_IS_LEAP(year))) * (24 * 60 * 60);
    return ret + ((24 * 60 * 60) * (mday - 1)) + ((60 * 60) * hour) + (60 * min) + sec;
}

int64_t _nmea_parse_datetime(const char *const time_str, const char *const date_str) {
    if (!time_str[0])
        return -1;
    struct tm tm = { 0 };
    // Time: HHMMSS.ss
    tm.tm_hour = (time_str[0] - '0') * 10 + (time_str[1] - '0');
    tm.tm_min = (time_str[2] - '0') * 10 + (time_str[3] - '0');
    tm.tm_sec = (time_str[4] - '0') * 10 + (time_str[5] - '0');
    // Date: DDMMYY (from RMC)
    if (date_str && date_str[0]) {
        tm.tm_mday = (date_str[0] - '0') * 10 + (date_str[1] - '0');
        tm.tm_mon = (date_str[2] - '0') * 10 + (date_str[3] - '0') - 1;
        tm.tm_year = (date_str[4] - '0') * 10 + (date_str[5] - '0') + 100; // 2000+
    }
    return nmea_timegm(&tm);
}

// ------------------------------------------------------------------------------------------------------------------------

#define _NMEA_SENTENCE_FIELDS_MAX 16
typedef struct {
    const char *start[_NMEA_SENTENCE_FIELDS_MAX]; // pointer to start of each field
    uint8_t len[_NMEA_SENTENCE_FIELDS_MAX];       // length of each field
    int count;                                    // total fields found
} nmea_fields_t;

int _nmea_field_indexes(const char *const sentence, nmea_fields_t *const f) {
    f->count = 0;
    const char *p = sentence;
    if (*p == '$')
        p++;
    while (*p && f->count < _NMEA_SENTENCE_FIELDS_MAX) {
        f->start[f->count] = p;
        uint8_t len = 0;
        while (*p && *p != ',' && *p != '*' && *p != '\r' && *p != '\n')
            p++, len++;
        f->len[f->count] = len;
        f->count++;
        if (*p != ',')
            break;
        p++;
    }
    return f->count;
}

#define _NMEA_FIELD_LENGTH(f, idx) ((f)->len[idx])
#define _NMEA_FIELD_START(f, idx)  ((f)->start[idx])

// int nmea_field_get(const nmea_fields_t *f, int idx, char *out, int out_size) {
//     if (idx < 0 || idx >= f->count) {
//         out[0] = '\0';
//         return 0;
//     }
//     const int len = f->len[idx] < out_size ? f->len[idx] : out_size - 1;
//     memcpy(out, f->start[idx], (size_t)len);
//     out[len] = '\0';
//     return len;
// }

char _nmea_field_get_char(const nmea_fields_t *const f, const int idx) {
    if (idx < 0 || idx >= f->count || f->len[idx] == 0)
        return '\0';
    return f->start[idx][0];
}

int _nmea_field_get_int(const nmea_fields_t *const f, const int idx) {
    if (idx < 0 || idx >= f->count || f->len[idx] == 0)
        return 0;
    int val = 0;
    const char *p = f->start[idx];
    for (int i = 0; i < f->len[idx] && *p >= '0' && *p <= '9'; i++, p++)
        val = val * 10 + (*p - '0');
    return val;
}

float _nmea_field_get_float(const nmea_fields_t *const f, const int idx) {
    if (idx < 0 || idx >= f->count || f->len[idx] == 0)
        return 0.0f;
    const char *p = f->start[idx], *end = p + f->len[idx];
    uint32_t integer = 0;
    while (p < end && *p >= '0' && *p <= '9')
        integer = integer * 10 + (uint32_t)(*p++ - '0');
    if (p < end && *p == '.') {
        p++;
        uint32_t frac = 0, scale = 1;
        while (p < end && *p >= '0' && *p <= '9') {
            frac = frac * 10 + (uint32_t)(*p++ - '0');
            scale *= 10;
        }
        return (float)integer + (float)frac / (float)scale;
    }
    return (float)integer;
}

float _nmea_field_get_coord(const nmea_fields_t *const f, const int val_idx, const int dir_idx) {
    if (val_idx >= f->count || f->len[val_idx] == 0)
        return NAN;
    const char *s = f->start[val_idx];
    const int len = f->len[val_idx];
    // find the dot — minutes are the 2 digits before it
    const char *dot = (const char *)memchr(s, '.', (size_t)len);
    if (!dot || dot - s < 2)
        return NAN;
    // degrees: all digits before MM.MMMM
    int deg = 0;
    for (const char *p = s; p < dot - 2; p++)
        deg = deg * 10 + (*p - '0');
    // minutes: integer part (2 digits)
    const uint32_t min_int = (uint32_t)(dot[-2] - '0') * 10 + (uint32_t)(dot[-1] - '0');
    // fractional minutes: parse as scaled integer
    uint32_t frac = 0, scale = 1;
    for (const char *p = dot + 1; p < s + len && *p >= '0' && *p <= '9'; p++) {
        if (*p < '0' || *p > '9')
            return NAN;
        frac = frac * 10 + (uint32_t)(*p - '0');
        scale *= 10;
    }
    const float result = (float)deg + ((float)min_int + (float)frac / (float)scale) / 60.0f;
    const char dir = _nmea_field_get_char(f, dir_idx);
    return (dir == 'S' || dir == 'W') ? -result : result;
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t nmea_parse_gga(const char *const sentence, gnss_reading_t *const out) {

    nmea_fields_t f;

    // GGA has 14+ fields
    ESP_RETURN_ON_FALSE(_nmea_field_indexes(sentence, &f) >= 10, DEV_ERR_PARSE, __func__, "< 10 fields");

    // Field 6: fix quality — check first to short-circuit
    if ((out->fix_quality = (uint8_t)_nmea_field_get_int(&f, 6)) == 0)
        return DEV_ERR_NO_FIX;
    // Field 7: satellites
    out->satellites = (uint8_t)_nmea_field_get_int(&f, 7);
    // Field 8: HDOP
    out->hdop = _nmea_field_get_float(&f, 8);
    // Fields 2,3: latitude
    out->latitude = _nmea_field_get_coord(&f, 2, 3);
    // Fields 4,5: longitude
    out->longitude = _nmea_field_get_coord(&f, 4, 5);
    // Field 9: altitude (m)
    out->altitude = _nmea_field_get_float(&f, 9);

    return ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------

esp_err_t nmea_parse_rmc(const char *const sentence, gnss_reading_t *const out) {

    nmea_fields_t f;

    // RMC has 12+ fields
    ESP_RETURN_ON_FALSE(_nmea_field_indexes(sentence, &f) >= 10, DEV_ERR_PARSE, __func__, "< 10 fields");

    // Field 2: status — 'A' = active, 'V' = void
    if (_nmea_field_get_char(&f, 2) != 'A')
        return DEV_ERR_NO_FIX;
    // Field 1: time (HHMMSS.ss)
    const char *time_ptr = _NMEA_FIELD_LENGTH(&f, 1) >= 6 ? _NMEA_FIELD_START(&f, 1) : NULL;
    // Field 9: date (DDMMYY)
    const char *date_ptr = _NMEA_FIELD_LENGTH(&f, 9) >= 6 ? _NMEA_FIELD_START(&f, 9) : NULL;
    out->utc_time = _nmea_parse_datetime(time_ptr, date_ptr);

    return ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------

#define _NMEA_TXT_VALUE_MAX 32

typedef struct {
    char manufacturer[_NMEA_TXT_VALUE_MAX]; // MA= field
    char ic[_NMEA_TXT_VALUE_MAX];           // IC= field (chip identifier)
    char software[_NMEA_TXT_VALUE_MAX];     // SW= field (firmware version)
    char timebuild[_NMEA_TXT_VALUE_MAX];    // TB= field (build timestamp)
    char model[_NMEA_TXT_VALUE_MAX];        // MO= field (build timestamp)
} nmea_txt_product_t;

// Extract the text payload from a TXT sentence (field index 4, everything after
// the 4th comma up to '*'), then match a 2-char key prefix and copy the value
// after '=' into dst. The text payload may contain commas (e.g. SW=URANUS5,V5.3.0.0)
// so we skip commas manually rather than using nmea_field_indexes.
bool _nmea_parse_txt_field(const char *const sentence, const char key[2], char *const dst, const int dst_size) {
    // Skip past "$GPTXT," and 3 more comma-separated fields to reach the text payload
    const char *p = sentence;
    if (*p == '$')
        p++;
    int commas = 0;
    while (*p && commas < 4)
        if (*p++ == ',')
            commas++;
    if (commas < 4 || !*p)
        return false;
    if (*p++ != key[0] || *p++ != key[1] || *p++ != '=')
        return false;
    int i = 0;
    while (*p && *p != '*' && *p != '\r' && *p != '\n' && i < dst_size - 1)
        dst[i++] = *p++;
    dst[i] = '\0';
    return true;
}

bool nmea_parse_txt(const char *const sentence, nmea_txt_product_t *const product) {
    return _nmea_parse_txt_field(sentence, "MA", product->manufacturer, _NMEA_TXT_VALUE_MAX) || _nmea_parse_txt_field(sentence, "IC", product->ic, _NMEA_TXT_VALUE_MAX) ||
           _nmea_parse_txt_field(sentence, "SW", product->software, _NMEA_TXT_VALUE_MAX) || _nmea_parse_txt_field(sentence, "TB", product->timebuild, _NMEA_TXT_VALUE_MAX) ||
           _nmea_parse_txt_field(sentence, "MO", product->model, _NMEA_TXT_VALUE_MAX);
}

const char *nmea_format_txt_product(const nmea_txt_product_t *const product, char *const buf, const int buf_size) {
    int pos = 0;
    const struct {
        const char *key, *val;
    } fields[] = {
        { "ma", product->manufacturer }, //
        { "ic", product->ic },           //
        { "sw", product->software },     //
        { "mo", product->model },        //
        { "tb", product->timebuild },
    };
    for (int i = 0; i < (int)(sizeof(fields) / sizeof(fields[0])); i++)
        if (fields[i].val[0])
            pos += snprintf(buf + pos, (size_t)(buf_size - pos), "%s%s=%s", pos > 0 ? ", " : "", fields[i].key, fields[i].val);
    if (pos == 0 && buf_size > 0)
        buf[0] = '\0';
    return buf;
}

// ------------------------------------------------------------------------------------------------------------------------

#define _GNSS_RX_TIME_MIN 2000

struct {
    uint8_t data[_NMEA_SENTENCE_LENGTH_MAX * 2];
    int head, tail;
} _gnss_rx;

void _gnss_rx_init(void) {
    _gnss_rx.head = _gnss_rx.tail = 0;
}

bool _gnss_rx_next(char *const ch, const uint32_t timeout_ms) {
    if (_gnss_rx.head == _gnss_rx.tail) {
        _gnss_rx.head = _gnss_rx.tail = 0;
        const int n = hw_uart_read(GNSS_UART_PORT, _gnss_rx.data, sizeof(_gnss_rx.data), (int)timeout_ms);
        if (n <= 0)
            return false;
        _gnss_rx.tail += n;
    }
    *ch = (char)_gnss_rx.data[_gnss_rx.head++];
    return true;
}

/* NOTE THE CONTRACT: `start_ms` is an ABSOLUTE origin, not "now" -- the deadline is
   start_ms + timeout_ms. Passing a caller's loop start therefore gives every call in that loop the
   SAME deadline, so once it passes they all return 0 immediately. A caller that wants a fresh
   window per sentence must pass hw_ticks_ms(); one that wants a total budget across several
   sentences passes its own start, which is what _gnss_read_product() does. */
int _gnss_read_sentence(char *const line, const int max_len, const uint32_t timeout_ms, const int64_t start_ms) {
    int pos = 0;
    bool in_sentence = false;
    int32_t remaining_ms;
    while ((remaining_ms = ((int32_t)timeout_ms - (int32_t)(hw_ticks_ms() - start_ms))) > 0 && pos < max_len - 1) {
        char ch;
        if (!_gnss_rx_next(&ch, (uint32_t)MIN_INT(remaining_ms, _GNSS_RX_TIME_MIN)))
            hw_delay_ms_yieldable(5); // prevent any failure based tight busy spins
        else {
            if (ch == '$') {
                pos = 0;
                line[pos++] = ch;
                in_sentence = true;
            } else if (in_sentence) {
                line[pos++] = ch;
                if (ch == '\n')
                    break;
            }
        }
    }
    line[pos] = '\0';
    if (pos < _NMEA_SENTENCE_LENGTH_MIN || !in_sentence)
        return 0;
    while (pos > 0 && (line[pos - 1] == '\r' || line[pos - 1] == '\n'))
        line[--pos] = '\0';
    return pos;
}

// ------------------------------------------------------------------------------------------------------------------------
