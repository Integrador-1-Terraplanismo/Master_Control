#include "button_matrix.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include <ctype.h>
#include <stdio.h>

static const char *TAG = "BUTTON_MATRIX";
static adc_oneshot_unit_handle_t adc_handle = NULL;

void button_matrix_init(void) {
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = BUTTON_ADC_UNIT,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle));

    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_11,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, BUTTON_ADC_CHANNEL, &config));
    ESP_LOGI(TAG, "Teclado analogico inicializado no GPIO 32.");
}

int button_matrix_read_raw(void) {
    int adc_raw = 4095;
    if (adc_handle == NULL) {
        return -1;
    }
    if (adc_oneshot_read(adc_handle, BUTTON_ADC_CHANNEL, &adc_raw) != ESP_OK) {
        return -1;
    }
    return adc_raw;
}

button_id_t button_matrix_read(void) {
    int adc_raw = button_matrix_read_raw();
    if (adc_raw < 0) {
        return BTN_NONE;
    }

    if (adc_raw > 3800) return BTN_NONE;
    if (adc_raw >= 0 && adc_raw < 150) return BTN_1;
    if (adc_raw >= 200 && adc_raw < 600) return BTN_2;
    if (adc_raw >= 800 && adc_raw < 1300) return BTN_3;
    if (adc_raw >= 1500 && adc_raw < 2300) return BTN_4;
    if (adc_raw >= 2500 && adc_raw < 3500) return BTN_5;

    return BTN_NONE;
}

button_id_t button_matrix_read_on_press(void) {
    static button_id_t last = BTN_NONE;
    button_id_t current = button_matrix_read();
    button_id_t pressed = BTN_NONE;

    if (current != BTN_NONE && last == BTN_NONE) {
        pressed = current;
    }

    last = current;
    return pressed;
}

const char *button_matrix_label(button_id_t btn) {
    switch (btn) {
        case BTN_1: return "ANTERIOR";
        case BTN_2: return "PROXIMA";
        case BTN_3: return "CONFIRMAR";
        case BTN_4: return "APAGAR";
        case BTN_5: return "ENTER";
        default: return "NENHUM";
    }
}

void button_keyboard_init(keyboard_state_t *state) {
    if (state == NULL) {
        return;
    }
    button_keyboard_reset(state);
}

void button_keyboard_reset(keyboard_state_t *state) {
    if (state == NULL) {
        return;
    }
    state->buffer[0] = '\0';
    state->length = 0;
    state->cursor_letter = 'A';
}

static char keyboard_prev_letter(char letter) {
    if (letter <= 'A') {
        return 'Z';
    }
    return (char)(letter - 1);
}

static char keyboard_next_letter(char letter) {
    if (letter >= 'Z') {
        return 'A';
    }
    return (char)(letter + 1);
}

keyboard_event_t button_keyboard_handle(keyboard_state_t *state, button_id_t btn) {
    if (state == NULL || btn == BTN_NONE) {
        return KB_EVT_NONE;
    }

    switch (btn) {
        case BTN_1:
            state->cursor_letter = keyboard_prev_letter(state->cursor_letter);
            return KB_EVT_CURSOR_CHANGED;

        case BTN_2:
            state->cursor_letter = keyboard_next_letter(state->cursor_letter);
            return KB_EVT_CURSOR_CHANGED;

        case BTN_3:
            if (state->length >= KEYBOARD_NAME_MAX) {
                return KB_EVT_NONE;
            }
            state->buffer[state->length++] = state->cursor_letter;
            state->buffer[state->length] = '\0';
            state->cursor_letter = 'A';
            return KB_EVT_LETTER_ADDED;

        case BTN_4:
            if (state->length == 0) {
                return KB_EVT_NONE;
            }
            state->buffer[--state->length] = '\0';
            return KB_EVT_BACKSPACE;

        case BTN_5:
            if (state->length == 0) {
                return KB_EVT_NONE;
            }
            return KB_EVT_ENTER;

        default:
            return KB_EVT_NONE;
    }
}
