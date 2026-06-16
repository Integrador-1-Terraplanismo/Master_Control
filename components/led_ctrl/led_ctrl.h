#ifndef LED_CTRL_H
#define LED_CTRL_H

#include <stdbool.h>
#include <stdint.h>

// Pino único conectado à entrada de sinal da controladora
#define LED_GPIO 2

// Códigos Hexadecimais Comuns (Protocolo NEC - 24 teclas)
// NOTA: Se a sua controladora usar um controle diferente, esses códigos podem variar.
#define IR_CMD_ON    0x00FFB04F
#define IR_CMD_OFF   0x00FFF807
#define IR_CMD_RED   0x00FF1AE5
#define IR_CMD_GREEN 0x00FF9A65
#define IR_CMD_BLUE  0x00FFA25D

// Funções públicas do módulo
void led_ctrl_init(void);
void led_ctrl_set_state(bool state);
void led_ctrl_send_command(uint32_t nec_code);

#endif // LED_CTRL_H