#ifndef BUTTON_MATRIX_H
#define BUTTON_MATRIX_H

#include <stdint.h>
#include <stddef.h>

#define BUTTON_ADC_CHANNEL  ADC_CHANNEL_4
#define BUTTON_ADC_UNIT     ADC_UNIT_1

#define KEYBOARD_NAME_MAX   30

typedef enum {
    BTN_NONE = 0,
    BTN_1,
    BTN_2,
    BTN_3,
    BTN_4,
    BTN_5
} button_id_t;

typedef enum {
    KB_EVT_NONE = 0,
    KB_EVT_CURSOR_CHANGED,
    KB_EVT_LETTER_ADDED,
    KB_EVT_BACKSPACE,
    KB_EVT_ENTER
} keyboard_event_t;

typedef struct {
    char buffer[KEYBOARD_NAME_MAX + 1];
    uint8_t length;
    char cursor_letter;
} keyboard_state_t;

void button_matrix_init(void);
button_id_t button_matrix_read(void);
button_id_t button_matrix_read_on_press(void);
int button_matrix_read_raw(void);
const char *button_matrix_label(button_id_t btn);

void button_keyboard_init(keyboard_state_t *state);
void button_keyboard_reset(keyboard_state_t *state);
keyboard_event_t button_keyboard_handle(keyboard_state_t *state, button_id_t btn);

#endif
