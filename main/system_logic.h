#ifndef SYSTEM_LOGIC_H
#define SYSTEM_LOGIC_H

#include <stdint.h>
#include <stddef.h>
#include "freertos/timers.h"

#define EX_UART_NUM UART_NUM_0
#define BUF_SIZE    1024

typedef enum {
    STATE_IDLE,
    STATE_RECORD_GET_PLANET,
    STATE_RECORD_WAIT_NFC,
    STATE_READING_NFC,
    STATE_MINIGAME,
    STATE_KEYBOARD
} system_state_t;

void serial_monitor_task(void *pvParameters);
void process_pc_command(const char* command, char* response_buffer, size_t max_resp_len);
void handle_nfc_detection(uint8_t *uid);
void iniciar_timer_leitura(void);
void leitura_timeout_callback(TimerHandle_t xTimer);
void system_logic_init(void);
void executar_loop_minigame(void);
void executar_loop_teclado(void);
system_state_t system_logic_get_state(void);
const char *system_logic_state_name(system_state_t state);

#endif
