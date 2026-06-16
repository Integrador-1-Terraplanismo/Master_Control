#include "button_matrix.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

static const char *TAG = "BUTTON_MATRIX";
static adc_oneshot_unit_handle_t adc_handle = NULL;

void button_matrix_init(void) {
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = BUTTON_ADC_UNIT,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle));

    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT, 
        .atten = ADC_ATTEN_DB_12,         
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, BUTTON_ADC_CHANNEL, &config));
    ESP_LOGI(TAG, "Teclado analógico inicializado com sucesso no GPIO 32.");
}

button_id_t button_matrix_read(void) {
    int adc_raw = 4095;
    if (adc_handle == NULL) return BTN_NONE;

    if (adc_oneshot_read(adc_handle, BUTTON_ADC_CHANNEL, &adc_raw) != ESP_OK) {
        return BTN_NONE;
    }

    // 🎯 JANELAS DE TOLERÂNCIA AJUSTADAS SOB MEDIDA PARA A SUA PLACA:
    
    // Sem pressionar nada (Valor real: 4095)
    if (adc_raw > 3800) return BTN_NONE;
    
    // Botão 1 (Valor real: 0)
    if (adc_raw >= 0 && adc_raw < 150) return BTN_1;
    
    // Botão 2 (Valor real: 375)
    if (adc_raw >= 200 && adc_raw < 600) return BTN_2;
    
    // Botão 3 (Valor real: 1003)
    if (adc_raw >= 800 && adc_raw < 1300) return BTN_3;
    
    // Botão 4 (Valor real: 1771)
    if (adc_raw >= 1500 && adc_raw < 2300) return BTN_4;
    
    // Botão 5 (Valor real: ~2900)
    if (adc_raw >= 2500 && adc_raw < 3500) return BTN_5;

    return BTN_NONE;
}