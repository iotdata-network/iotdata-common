
// ------------------------------------------------------------------------------------------------------------------------
// ADC (oneshot, single-channel)
// ------------------------------------------------------------------------------------------------------------------------

#ifndef EMU_LINUX
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#pragma GCC diagnostic pop
#endif

// ------------------------------------------------------------------------------------------------------------------------

#define HW_ADC_VREF_MV 3300

typedef struct {
    adc_oneshot_unit_handle_t unit;
    adc_cali_handle_t cali;
    adc_channel_t channel;
} _hw_adc_state_t;

static _hw_adc_state_t s_hw_adc;

// ------------------------------------------------------------------------------------------------------------------------

static esp_err_t hw_adc_oneshot_start(const adc_unit_t unit, const adc_channel_t channel, const adc_atten_t atten, const adc_bitwidth_t bitwidth) {
    assert(!s_hw_adc.unit);

    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&(const adc_oneshot_unit_init_cfg_t){ .unit_id = unit }), &s_hw_adc.unit), __func__, "adc_oneshot_new_unit");

    esp_err_t err;
    if ((err = adc_oneshot_config_channel(s_hw_adc.unit, channel, &(const adc_oneshot_chan_cfg_t){ .atten = atten, .bitwidth = bitwidth }))) != ESP_OK) {
        (void)adc_oneshot_del_unit(s_hw_adc.unit);
        s_hw_adc.unit = NULL;
        ESP_LOGE("hw_adc", "adc_oneshot_config_channel: %s", esp_err_to_name(err));
        return err;
    }

    if (adc_cali_create_scheme_curve_fitting(&(const adc_cali_curve_fitting_config_t){
        .unit_id = unit,
        .chan = channel,
        .atten = atten,
        .bitwidth = bitwidth,
    }, &s_hw_adc.cali) != ESP_OK)
        s_hw_adc.cali = NULL;

    s_hw_adc.channel = channel;
    ESP_LOGD("hw_adc", "started: unit=%d, chan=%d, cali=%s", (int)unit, (int)channel, s_hw_adc.cali ? "curve-fitting" : "none");
    return ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------

static esp_err_t hw_adc_oneshot_stop(void) {
    if (s_hw_adc.cali) {
        adc_cali_delete_scheme_curve_fitting(s_hw_adc.cali);
        s_hw_adc.cali = NULL;
    }
    if (s_hw_adc.unit) {
        adc_oneshot_del_unit(s_hw_adc.unit);
        s_hw_adc.unit = NULL;
    }
    ESP_LOGD("hw_adc", "stopped");
    return ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------

static esp_err_t hw_adc_oneshot_read_mv(int *const mv) {
    int raw = 0;
    ESP_RETURN_ON_ERROR(adc_oneshot_read(s_hw_adc.unit, s_hw_adc.channel, &raw), __func__, "adc_oneshot_read");
    if (s_hw_adc.cali)
        return adc_cali_raw_to_voltage(s_hw_adc.cali, raw, mv);
    *mv = (raw * HW_ADC_VREF_MV) / 4095;
    return ESP_OK;
}

// ------------------------------------------------------------------------------------------------------------------------
