#ifndef SYSTEM_LOGIC_H
#define SYSTEM_LOGIC_H

#include <stdint.h>
#include <stddef.h>

// Definições globais compartilhadas
#define EX_UART_NUM UART_NUM_0
#define BUF_SIZE    1024

typedef enum {
    STATE_IDLE,              // Sistema ocioso, aguardando chamadas
    STATE_RECORD_GET_PLANET, // Etapa 1 da Gravação (Serial)
    STATE_RECORD_WAIT_NFC,   // Etapa 2 da Gravação (Bloqueia TCP)
    STATE_READING_NFC,       // Modo de Leitura TCP (Bloqueia Serial, 30s limite)
    STATE_MINIGAME           // Modo de baixa latência
} system_state_t;

// Declaração das funções de controle
void serial_monitor_task(void *pvParameters);
void process_pc_command(const char* command, char* response_buffer, size_t max_resp_len);
void handle_nfc_detection(uint8_t *uid);
void iniciar_timer_leitura(void);
void init_timers(void);
void leitura_timeout_callback(TimerHandle_t xTimer);
void system_logic_init(void);
void executar_loop_minigame(void);

#endif // SYSTEM_LOGIC_H