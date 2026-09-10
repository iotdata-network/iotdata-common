
// ------------------------------------------------------------------------------------------------------------------------
// readings.c - Multi-reading strategy: discard, gross check, outlier, average
// ------------------------------------------------------------------------------------------------------------------------

#define READINGS_MAX 20

typedef struct {
    int num_total;     // total readings to attempt
    int num_discard;   // discard first N (sensor settling)
    int min_accepted;  // minimum accepted for valid result
    float range_min;   // gross sanity floor
    float range_max;   // gross sanity ceiling
    float outlier_pct; // reject if > this % from median (0 = disabled)
    float outlier_abs; // reject if > this absolute distance from median (0 = disabled)
} reading_strategy_t;

typedef struct {
    float value;          // filtered mean of accepted readings
    float spread;         // max - min of accepted readings
    int accepted;         // how many readings were used
    int rejected_range;   // failed gross range check
    int rejected_outlier; // failed outlier check
    bool valid;           // accepted >= min_accepted
} reading_result_t;

// ------------------------------------------------------------------------------------------------------------------------

#define STRATEGY_STR_MAX 64
const char *reading_strategy_to_str(const reading_strategy_t *const s, char *const buf, const int buf_size) {
    snprintf(buf, (size_t)buf_size, "(%d-%d>%d@%.0f-%.0f#%.1f+%.1f)", s->num_total, s->num_discard, s->min_accepted, (double)s->range_min, (double)s->range_max, (double)s->outlier_pct, (double)s->outlier_abs);
    return buf;
}

// ------------------------------------------------------------------------------------------------------------------------

// NOTE: insertion sort is fine for current array sizes (max 20). The full pipeline (copy →
// sort → copy → outlier scan) could be replaced by an in-place sort + inward walk from both
// ends of the sorted array, but the current code is clear and the arrays are small.
void _sort_floats(float *const arr, const int n) {
    for (int i = 1; i < n; i++) {
        const float key = arr[i];
        int j = i - 1;
        while (j >= 0 && arr[j] > key) {
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = key;
    }
}

// ------------------------------------------------------------------------------------------------------------------------
// readings_process
//
// Takes an array of raw readings (already collected by the caller or device
// read function), applies the strategy, and produces a filtered result.
//
// Steps:
//   1. Discard first num_discard readings (settling)
//   2. Gross range check on remaining
//   3. Sort survivors, compute median
//   4. Outlier rejection relative to median (if outlier_pct > 0)
//   5. Mean and spread of final accepted set
// ------------------------------------------------------------------------------------------------------------------------

esp_err_t readings_process(const float *const raw, const int count, const reading_strategy_t *const strategy, reading_result_t *const result) {

    if (count <= 0 || !raw || !strategy)
        return ESP_ERR_INVALID_ARG;

    result->rejected_range = result->rejected_outlier = 0;

    const int start = strategy->num_discard;
    if (start >= count) {
        ESP_LOGW(__func__, "all readings (%d) discarded (num_discard=%d)", count, start);
        result->valid = false;
        return DEV_ERR_BAD_READING;
    }

    // Gross range filter into single working array
    float buf[READINGS_MAX];
    int n = 0;
    for (int i = start; i < count && n < READINGS_MAX; i++)
        if (raw[i] >= strategy->range_min && raw[i] <= strategy->range_max)
            buf[n++] = raw[i];
        else {
            result->rejected_range++;
            ESP_LOGD(__func__, "rejected [%d]=%.3f (range %.1f..%.1f)", i, (double)raw[i], (double)strategy->range_min, (double)strategy->range_max);
        }
    if (n == 0) {
        ESP_LOGW(__func__, "all readings (%d) rejected by range check", count);
        result->valid = false;
        return DEV_ERR_BAD_READING;
    }

    _sort_floats(buf, n);
    const float median = (n % 2 == 0) ? ((buf[n / 2 - 1] + buf[n / 2]) / 2.0f) : buf[n / 2];

    // Outlier rejection in place — sorted, so walk inward from ends
    int lo = 0, hi = n - 1;
    if (strategy->outlier_pct > 0.0f || strategy->outlier_abs > 0.0f) {
        float threshold = 0.0f;
        if (strategy->outlier_pct > 0.0f && fabsf(median) > 1e-9f)
            threshold = fabsf(median) * (strategy->outlier_pct / 100.0f);
        if (strategy->outlier_abs > 0.0f && strategy->outlier_abs > threshold)
            threshold = strategy->outlier_abs;
        while (lo <= hi && fabsf(buf[lo] - median) > threshold) {
            result->rejected_outlier++;
            lo++;
        }
        while (hi >= lo && fabsf(buf[hi] - median) > threshold) {
            result->rejected_outlier++;
            hi--;
        }
    }

    const int n_accepted = hi - lo + 1;
    if (n_accepted <= 0) {
        ESP_LOGW(__func__, "all readings (%d) rejected after outlier check", count);
        result->valid = false;
        return DEV_ERR_BAD_READING;
    }

    // Mean and spread — already sorted, so spread is trivial
    float vsum = 0.0f;
    for (int i = lo; i <= hi; i++)
        vsum += buf[i];
    result->value = vsum / (float)n_accepted;
    result->spread = buf[hi] - buf[lo];
    result->accepted = n_accepted;
    result->valid = (n_accepted >= strategy->min_accepted);

    return result->valid ? ESP_OK : DEV_ERR_BAD_QUALITY;
}

// ------------------------------------------------------------------------------------------------------------------------
