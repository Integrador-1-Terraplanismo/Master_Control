#ifndef BUTTON_MATRIX_H
#define BUTTON_MATRIX_H

#include <stdint.h>

#define BUTTON_ADC_CHANNEL  ADC_CHANNEL_4
#define BUTTON_ADC_UNIT     ADC_UNIT_1

// Enumeração para identificar qual botão foi pressionado
typedef enum {
    BTN_NONE = 0,
    BTN_1,
    BTN_2,
    BTN_3,
    BTN_4,
    BTN_5
} button_id_t;

// Funções públicas
void button_matrix_init(void);
button_id_t button_matrix_read(void);

#endif // BUTTON_MATRIX_H