#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h" // ADICIONADO: Biblioteca para a Fila
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/timers.h"

#include "system_logic.h"
#include "led_ctrl.h"
#include "servo_ctrl.h"
#include "wifi_tcp_mgr.h"
#include "storage_mgr.h"
#include "button_matrix.h"
#include "nfc_reader.h"

static const char *TAG = "SYSTEM_LOGIC";
static TimerHandle_t leitura_timer = NULL;

// ADICIONADO: Handle da Fila para passar os UIDs da task NFC para a task Lógica
static QueueHandle_t nfc_queue = NULL;

static system_state_t current_state = STATE_IDLE;
static char requested_planet[32] = "";
static char target_record_planet[32] = "";
static int failed_attempts = 0;
static uint8_t last_serialized_uid[5] = {0};
static bool keyboard_via_tcp = false;

static void string_to_uppercase(char *str);
static void finish_nfc_search(bool success);
static bool find_planet_by_uid(const char *uid_str, char *found_name, size_t max_len);

system_state_t system_logic_get_state(void) {
    return current_state;
}

const char *system_logic_state_name(system_state_t state) {
    switch (state) {
        case STATE_IDLE: return "IDLE";
        case STATE_RECORD_GET_PLANET: return "GRAVACAO_NOME";
        case STATE_RECORD_WAIT_NFC: return "GRAVACAO_NFC";
        case STATE_READING_NFC: return "LEITURA_NFC";
        case STATE_MINIGAME: return "MINIGAME";
        case STATE_KEYBOARD: return "TECLADO";
        default: return "DESCONHECIDO";
    }
}

static bool system_is_busy_for_pc(void) {
    return current_state == STATE_RECORD_GET_PLANET ||
           current_state == STATE_RECORD_WAIT_NFC ||
           current_state == STATE_READING_NFC ||
           current_state == STATE_MINIGAME ||
           current_state == STATE_KEYBOARD;
}

static bool system_is_idle_for_tests(void) {
    return current_state == STATE_IDLE;
}

// ADICIONADO: Tarefa que vai processar os motores e lógicas de forma assíncrona
static void logic_actuation_task(void *pvParameters) {
    uint8_t uid[5];

    while (1) {
        // Fica aguardando infinitamente até receber um UID na fila
        if (xQueueReceive(nfc_queue, &uid, portMAX_DELAY)) {
            char uid_str[20];
            snprintf(uid_str, sizeof(uid_str), "%02X%02X%02X%02X", uid[0], uid[1], uid[2], uid[3]);

            if (current_state == STATE_RECORD_WAIT_NFC) {
                if (memcmp(last_serialized_uid, uid, 4) == 0) {
                    continue; // Pula para a próxima iteração do loop
                }
                memcpy(last_serialized_uid, uid, 4);

                led_ctrl_set_state(true);
                storage_save_planet(target_record_planet, uid_str);
                printf("\n[GRAVADO] Tag %s -> planeta %s\n", uid_str, target_record_planet);
                printf("Aproxime outra tag ou envie 'END'/'PROXIMO'.\n");
                fflush(stdout);

                vTaskDelay(pdMS_TO_TICKS(500));
                led_ctrl_set_state(false);
                continue;
            }

            if (current_state != STATE_READING_NFC || requested_planet[0] == '\0') {
                continue;
            }

            char detected_planet_name[32] = "";
            // Nota: certifique-se de que find_planet_by_uid esteja implementada ou acessível acima
            // Vamos assumir que ela existe no seu código completo como static
            bool found = find_planet_by_uid(uid_str, detected_planet_name, sizeof(detected_planet_name));

            if (found) {
                // string_to_uppercase já deve estar definida acima
                string_to_uppercase(detected_planet_name);

                if (strcmp(detected_planet_name, requested_planet) == 0) {
                    printf("\n[NFC OK] Tag %s confirmada para %s\n", uid_str, detected_planet_name);
                    fflush(stdout);

                    tcp_send_reply(TCP_REPLY_RECD);
                    servo_set_angle(SERVO_1, 0);
                    servo_set_angle(SERVO_2, 0);
                    led_ctrl_set_state(true);
                    
                    // ESTE DELAY AGORA NÃO TRAVA MAIS O NFC!
                    vTaskDelay(pdMS_TO_TICKS(2000)); 
                    
                    servo_set_angle(SERVO_1, 90);
                    servo_set_angle(SERVO_2, 90);
                    led_ctrl_set_state(false);
                    finish_nfc_search(true);
                    continue;
                }

                tcp_send_reply(TCP_REPLY_NRECD);
                failed_attempts++;
            } else {
                tcp_send_reply(TCP_REPLY_NPLT);
                failed_attempts++;
            }

            if (failed_attempts >= 3) {
                printf("\n[BLOQUEIO] 3 tentativas falhas para %s\n", requested_planet);
                fflush(stdout);
                finish_nfc_search(false);
            }

            vTaskDelay(pdMS_TO_TICKS(1500));
        }
    }
}

// CORRIGIDO: Inicialização agora cria a Fila e a Task
void system_logic_init(void) {
    leitura_timer = xTimerCreate(
        "LeituraTimeoutTimer",
        pdMS_TO_TICKS(30000),
        pdFALSE,
        (void *)0,
        leitura_timeout_callback
    );

    if (leitura_timer == NULL) {
        ESP_LOGE(TAG, "Falha ao alocar timer de leitura NFC.");
    }

    // Cria a fila capaz de segurar até 5 UIDs lidos em sequência rápida
    nfc_queue = xQueueCreate(5, sizeof(uint8_t) * 5);
    if (nfc_queue == NULL) {
        ESP_LOGE(TAG, "Falha ao criar fila do NFC.");
    }

    // Inicia a tarefa que moverá os servos e controlará os delays
    xTaskCreate(logic_actuation_task, "logic_task", 4096, NULL, 5, NULL);
}

static void string_to_uppercase(char *str) {
    for (int i = 0; str[i]; i++) {
        str[i] = (char)toupper((unsigned char)str[i]);
    }
}

static void stop_leitura_timer(void) {
    if (leitura_timer != NULL) {
        xTimerStop(leitura_timer, 0);
    }
}

static void finish_nfc_search(bool success) {
    stop_leitura_timer();
    requested_planet[0] = '\0';
    failed_attempts = 0;
    current_state = STATE_IDLE;

    if (!success) {
        servo_set_angle(SERVO_1, 180);
        servo_set_angle(SERVO_2, 180);
        led_ctrl_set_state(false);
        vTaskDelay(pdMS_TO_TICKS(2000));
        servo_set_angle(SERVO_1, 90);
        servo_set_angle(SERVO_2, 90);
    }
}

static bool find_planet_by_uid(const char *uid_str, char *found_name, size_t max_len) {
    char file_content[1024];
    storage_get_file_content(file_content, sizeof(file_content));

    char *line = strtok(file_content, "\n");
    while (line != NULL) {
        char name[32];
        char uid[32];
        if (sscanf(line, "%[^,],%s", name, uid) == 2 && strcmp(uid, uid_str) == 0) {
            strncpy(found_name, name, max_len - 1);
            found_name[max_len - 1] = '\0';
            return true;
        }
        line = strtok(NULL, "\n");
    }
    return false;
}

static void run_servo_test_sequence(void) {
    servo_set_angle(SERVO_1, 90);
    servo_set_angle(SERVO_2, 90);
    vTaskDelay(pdMS_TO_TICKS(1500));

    servo_set_angle(SERVO_1, 180);
    servo_set_angle(SERVO_2, 180);
    vTaskDelay(pdMS_TO_TICKS(1500));

    servo_set_angle(SERVO_1, 0);
    servo_set_angle(SERVO_2, 0);
    vTaskDelay(pdMS_TO_TICKS(1500));

    servo_set_angle(SERVO_3, 90);
    servo_set_angle(SERVO_4, 90);
    vTaskDelay(pdMS_TO_TICKS(1500));

    servo_set_angle(SERVO_3, 180);
    servo_set_angle(SERVO_4, 180);
    vTaskDelay(pdMS_TO_TICKS(1500));

    servo_set_angle(SERVO_3, 0);
    servo_set_angle(SERVO_4, 0);
    vTaskDelay(pdMS_TO_TICKS(1500));
}

static void run_led_test_sequence(void) {
    led_ctrl_send_command(IR_CMD_ON);
    vTaskDelay(pdMS_TO_TICKS(1000));
    led_ctrl_send_command(IR_CMD_RED);
    vTaskDelay(pdMS_TO_TICKS(2000));
    led_ctrl_send_command(IR_CMD_GREEN);
    vTaskDelay(pdMS_TO_TICKS(2000));
    led_ctrl_send_command(IR_CMD_BLUE);
    vTaskDelay(pdMS_TO_TICKS(2000));
    led_ctrl_send_command(IR_CMD_OFF);
    vTaskDelay(pdMS_TO_TICKS(500));
}

static void print_keyboard_help(void) {
    printf("\n=== MODO TECLADO (NOME) ===\n");
    printf("Botao 1: Letra anterior\n");
    printf("Botao 2: Proxima letra\n");
    printf("Botao 3: Confirmar letra\n");
    printf("Botao 4: Apagar ultima letra\n");
    printf("Botao 5: ENTER (enviar nome)\n");
    printf("============================\n\n");
}

static void print_keyboard_status(const keyboard_state_t *kb) {
    printf("[TECLADO] Letra:%c | Nome:%s%s\n",
           kb->cursor_letter,
           kb->buffer,
           kb->length == 0 ? "(vazio)" : "");
    fflush(stdout);
}

static void send_keyboard_event(const char *event_line) {
    wifi_tcp_send_raw(event_line);
}

static void handle_keyboard_event(keyboard_state_t *kb, keyboard_event_t evt, bool via_tcp) {
    char line[64];

    switch (evt) {
        case KB_EVT_CURSOR_CHANGED:
            snprintf(line, sizeof(line), "TECLADO:CURSOR:%c\n", kb->cursor_letter);
            if (via_tcp) send_keyboard_event(line);
            else print_keyboard_status(kb);
            break;

        case KB_EVT_LETTER_ADDED:
            snprintf(line, sizeof(line), "TECLADO:NOME:%s\n", kb->buffer);
            if (via_tcp) send_keyboard_event(line);
            else print_keyboard_status(kb);
            break;

        case KB_EVT_BACKSPACE:
            snprintf(line, sizeof(line), "TECLADO:APAGOU:%s\n", kb->buffer);
            if (via_tcp) send_keyboard_event(line);
            else print_keyboard_status(kb);
            break;

        case KB_EVT_ENTER:
            snprintf(line, sizeof(line), "TECLADO:ENTER:%s\n", kb->buffer);
            if (via_tcp) send_keyboard_event(line);
            else {
                printf("\n[NOME CONFIRMADO] %s\n\n", kb->buffer);
                fflush(stdout);
            }
            current_state = STATE_IDLE;
            nfc_reader_start_reading();
            break;

        default:
            break;
    }
}

void executar_loop_teclado(void) {
    keyboard_state_t kb;
    button_keyboard_init(&kb);

    print_keyboard_help();
    print_keyboard_status(&kb);

    while (current_state == STATE_KEYBOARD) {
        button_id_t pressed = button_matrix_read_on_press();
        if (pressed != BTN_NONE) {
            keyboard_event_t evt = button_keyboard_handle(&kb, pressed);
            handle_keyboard_event(&kb, evt, keyboard_via_tcp);
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }

    keyboard_via_tcp = false;
}

void executar_loop_minigame(void) {
    button_id_t ultimo_botao = BTN_NONE;
    char msg_tcp[32];

    ESP_LOGI(TAG, "Loop do minigame iniciado.");

    while (current_state == STATE_MINIGAME) {
        button_id_t botao_atual = button_matrix_read();

        if (botao_atual != ultimo_botao) {
            ultimo_botao = botao_atual;

            if (botao_atual != BTN_NONE) {
                snprintf(msg_tcp, sizeof(msg_tcp), "BOTAO:%d:%s\n",
                         botao_atual, button_matrix_label(botao_atual));
                wifi_tcp_send_raw(msg_tcp);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void run_button_monitor_test(int duration_ms, bool via_tcp) {
    button_id_t ultimo_botao = BTN_NONE;
    int iterations = duration_ms / 50;

    for (int i = 0; i < iterations; i++) {
        button_id_t botao_atual = button_matrix_read();

        if (botao_atual != ultimo_botao) {
            ultimo_botao = botao_atual;

            if (botao_atual != BTN_NONE) {
                if (via_tcp) {
                    char line[48];
                    snprintf(line, sizeof(line), "TESTE_BOTOES:%d:%s\n",
                             botao_atual, button_matrix_label(botao_atual));
                    wifi_tcp_send_raw(line);
                } else {
                    printf("[BOTOES] Botao %d (%s) | ADC=%d\n",
                           botao_atual, button_matrix_label(botao_atual),
                           button_matrix_read_raw());
                    fflush(stdout);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static int run_nfc_scan_test(int duration_ms, bool via_tcp) {
    bool was_reading = nfc_reader_is_reading();
    if (was_reading) {
        nfc_reader_stop_reading();
    }

    int tags_found = 0;
    int iterations = duration_ms / 300;
    char uid[20];

    for (int i = 0; i < iterations; i++) {
        if (nfc_reader_scan_once(uid, sizeof(uid))) {
            tags_found++;
            if (via_tcp) {
                char line[48];
                snprintf(line, sizeof(line), "TESTE_NFC:TAG:%s\n", uid);
                wifi_tcp_send_raw(line);
            } else {
                printf("[NFC] Tag detectada: %s\n", uid);
                fflush(stdout);
            }
            vTaskDelay(pdMS_TO_TICKS(1200));
        }
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    if (was_reading) {
        nfc_reader_start_reading();
    }

    return tags_found;
}

void process_pc_command(const char *command, char *response_buffer, size_t max_resp_len) {
    char cmd_copy[64];
    strncpy(cmd_copy, command, sizeof(cmd_copy) - 1);
    cmd_copy[sizeof(cmd_copy) - 1] = '\0';
    cmd_copy[strcspn(cmd_copy, "\r\n")] = 0;

    if (system_is_busy_for_pc() &&
        strncmp(cmd_copy, "FIM_", 4) != 0 &&
        strcmp(cmd_copy, "STATUS") != 0) {
        snprintf(response_buffer, max_resp_len, "ERRO:ESP32 Ocupado\n");
        tcp_send_reply(TCP_REPLY_BUSY);
        return;
    }

    if (strncmp(cmd_copy, "BUSCA:", 6) == 0) {
        if (current_state != STATE_IDLE) {
            snprintf(response_buffer, max_resp_len, "ERRO:ESP32 Ocupado\n");
            return;
        }

        strncpy(requested_planet, cmd_copy + 6, sizeof(requested_planet) - 1);
        requested_planet[sizeof(requested_planet) - 1] = '\0';
        string_to_uppercase(requested_planet);
        failed_attempts = 0;
        current_state = STATE_READING_NFC;

        if (leitura_timer != NULL) {
            xTimerStart(leitura_timer, 0);
        }

        snprintf(response_buffer, max_resp_len, "BUSCANDO:%s\n", requested_planet);
        return;
    }

    if (strcmp(cmd_copy, "TESTE_SERVO") == 0) {
        if (!system_is_idle_for_tests()) {
            snprintf(response_buffer, max_resp_len, "busy\n");
            return;
        }
        run_servo_test_sequence();
        snprintf(response_buffer, max_resp_len, "TESTE_SERVO_CONCLUIDO\n");
        return;
    }

    if (strcmp(cmd_copy, "TESTE_LED") == 0) {
        if (!system_is_idle_for_tests()) {
            snprintf(response_buffer, max_resp_len, "busy\n");
            return;
        }
        run_led_test_sequence();
        snprintf(response_buffer, max_resp_len, "TESTE_LED_CONCLUIDO\n");
        return;
    }

    if (strcmp(cmd_copy, "TESTE_NFC") == 0) {
        if (!system_is_idle_for_tests()) {
            snprintf(response_buffer, max_resp_len, "busy\n");
            return;
        }
        char report[96];
        nfc_reader_self_test(report, sizeof(report));
        snprintf(response_buffer, max_resp_len, "%s\n", report);
        int found = run_nfc_scan_test(10000, true);
        char suffix[48];
        snprintf(suffix, sizeof(suffix), "TESTE_NFC:FIM:%d\n", found);
        wifi_tcp_send_raw(suffix);
        strncat(response_buffer, " | scan 10s iniciado\n", max_resp_len - strlen(response_buffer) - 1);
        return;
    }

    if (strcmp(cmd_copy, "TESTE_BOTOES") == 0) {
        if (!system_is_idle_for_tests()) {
            snprintf(response_buffer, max_resp_len, "busy\n");
            return;
        }
        snprintf(response_buffer, max_resp_len, "TESTE_BOTOES_INICIADO\n");
        run_button_monitor_test(10000, true);
        wifi_tcp_send_raw("TESTE_BOTOES:FIM\n");
        return;
    }

    if (strcmp(cmd_copy, "TECLADO") == 0) {
        if (!system_is_idle_for_tests()) {
            snprintf(response_buffer, max_resp_len, "busy\n");
            return;
        }
        current_state = STATE_KEYBOARD;
        keyboard_via_tcp = true;
        nfc_reader_stop_reading();
        snprintf(response_buffer, max_resp_len, "TECLADO_START_ACK\n");
        executar_loop_teclado();
        return;
    }

    if (strcmp(cmd_copy, "FIM_TECLADO") == 0) {
        if (current_state == STATE_KEYBOARD) {
            current_state = STATE_IDLE;
            nfc_reader_start_reading();
            snprintf(response_buffer, max_resp_len, "TECLADO_ENCERRADO\n");
        } else {
            snprintf(response_buffer, max_resp_len, "ERRO:Modo teclado inativo\n");
        }
        return;
    }

    if (strcmp(cmd_copy, "MINIGAME") == 0) {
        if (!system_is_idle_for_tests()) {
            snprintf(response_buffer, max_resp_len, "busy\n");
            return;
        }
        current_state = STATE_MINIGAME;
        nfc_reader_stop_reading();
        snprintf(response_buffer, max_resp_len, "MINIGAME_START_ACK\n");
        executar_loop_minigame();
        return;
    }

    if (strcmp(cmd_copy, "FIM_MINIGAME") == 0) {
        if (current_state == STATE_MINIGAME) {
            current_state = STATE_IDLE;
            nfc_reader_start_reading();
            snprintf(response_buffer, max_resp_len, "MODO_NORMAL_RESTABELECIDO\n");
        } else {
            snprintf(response_buffer, max_resp_len, "ERRO:Minigame inativo\n");
        }
        return;
    }

    if (strcmp(cmd_copy, "STATUS") == 0) {
        char nfc_report[96];
        nfc_reader_self_test(nfc_report, sizeof(nfc_report));
        snprintf(response_buffer, max_resp_len,
                 "STATUS:estado=%s,nfc=%s\n",
                 system_logic_state_name(current_state),
                 nfc_report);
        return;
    }

    if (strcmp(cmd_copy, "LISTAR") == 0) {
        char buffer_local[1024];
        storage_get_file_content(buffer_local, sizeof(buffer_local));
        snprintf(response_buffer, max_resp_len, "LISTAR:\n%s", buffer_local);
        return;
    }

    snprintf(response_buffer, max_resp_len, "COMANDO_INVALIDO\n");
}

// CORRIGIDO: Agora essa função apenas joga o dado na fila, liberando o Leitor NFC na hora.
void handle_nfc_detection(uint8_t *uid) {
    if (nfc_queue != NULL) {
        // Envia o array de 5 bytes do UID para a fila e segue o jogo. 
        xQueueSend(nfc_queue, uid, 0);
    }
}

void serial_monitor_task(void *pvParameters) {
    uint8_t *data = (uint8_t *)malloc(BUF_SIZE);

    printf("\n=== CONSOLE SERIAL PRONTO ===\n");
    printf("GRAVAR      - mapear planetas NFC\n");
    printf("LISTAR      - listar banco de dados\n");
    printf("LIMPAR      - apagar planetas.txt\n");
    printf("TECLADO     - digitar nome (5 botoes)\n");
    printf("TESTE       - LED + servo rapido\n");
    printf("TESTE_SERVO - teste completo dos servos\n");
    printf("TESTE_LED   - teste fita IR\n");
    printf("TESTE_NFC   - diagnostico + scan 15s\n");
    printf("TESTE_BOTOES- monitor de botoes 10s\n");
    printf("STATUS      - estado atual do sistema\n");
    printf("AJUDA       - mostrar este menu\n");
    printf("=============================\n\n");

    while (1) {
        int len = uart_read_bytes(EX_UART_NUM, data, BUF_SIZE - 1, pdMS_TO_TICKS(100));
        if (len > 0) {
            data[len] = 0;
            char *input = (char *)data;
            input[strcspn(input, "\r\n")] = 0;
            string_to_uppercase(input);

            if (strcmp(input, "AJUDA") == 0 || strcmp(input, "HELP") == 0) {
                printf("\nComandos: GRAVAR LISTAR LIMPAR TECLADO END PROXIMO\n");
                printf("Testes: TESTE TESTE_SERVO TESTE_LED TESTE_NFC TESTE_BOTOES STATUS\n\n");
                continue;
            }

            if (strcmp(input, "STATUS") == 0) {
                char report[96];
                nfc_reader_self_test(report, sizeof(report));
                printf("\nEstado: %s\n%s\n\n", system_logic_state_name(current_state), report);
                continue;
            }

            if (strcmp(input, "LISTAR") == 0) {
                char buffer_local[1024];
                storage_get_file_content(buffer_local, sizeof(buffer_local));
                printf("\n--- PLANETAS.TXT ---\n%s--------------------\n\n", buffer_local);
                continue;
            }

            if (strcmp(input, "TESTE_BOTOES") == 0) {
                if (system_is_idle_for_tests()) {
                    printf("\n[TESTE_BOTOES] Monitor 10s (ADC + rotulo)\n");
                    run_button_monitor_test(10000, false);
                    printf("[TESTE_BOTOES] Fim.\n\n");
                } else {
                    printf("\nSistema ocupado.\n");
                }
                continue;
            }

            if (strcmp(input, "TESTE_SERVO") == 0) {
                if (system_is_idle_for_tests()) {
                    printf("\n[TESTE_SERVO] Iniciando...\n");
                    run_servo_test_sequence();
                    printf("[TESTE_SERVO] Concluido.\n\n");
                } else {
                    printf("\nSistema ocupado.\n");
                }
                continue;
            }

            if (strcmp(input, "TESTE_LED") == 0) {
                if (system_is_idle_for_tests()) {
                    printf("\n[TESTE_LED] Sequencia IR...\n");
                    run_led_test_sequence();
                    printf("[TESTE_LED] Concluido.\n\n");
                } else {
                    printf("\nSistema ocupado.\n");
                }
                continue;
            }

            if (strcmp(input, "TESTE_NFC") == 0) {
                if (system_is_idle_for_tests()) {
                    char report[96];
                    nfc_reader_self_test(report, sizeof(report));
                    printf("\n[TESTE_NFC] %s\n", report);
                    printf("Aproxime tags por 15 segundos...\n");
                    int found = run_nfc_scan_test(15000, false);
                    printf("[TESTE_NFC] Fim. Tags unicas: %d\n\n", found);
                } else {
                    printf("\nSistema ocupado.\n");
                }
                continue;
            }

            if (strcmp(input, "TECLADO") == 0) {
                if (system_is_idle_for_tests()) {
                    current_state = STATE_KEYBOARD;
                    keyboard_via_tcp = false;
                    nfc_reader_stop_reading();
                    executar_loop_teclado();
                } else {
                    printf("\nSistema ocupado.\n");
                }
                continue;
            }

            if (strcmp(input, "END") == 0) {
                if (current_state != STATE_IDLE) {
                    current_state = STATE_IDLE;
                    target_record_planet[0] = '\0';
                    requested_planet[0] = '\0';
                    memset(last_serialized_uid, 0, sizeof(last_serialized_uid));
                    stop_leitura_timer();
                    nfc_reader_start_reading();
                    printf("\nModo normal restaurado.\n");
                } else {
                    printf("\nJa esta no modo normal.\n");
                }
                continue;
            }

            if (strcmp(input, "LIMPAR") == 0) {
                if (system_is_idle_for_tests()) {
                    storage_clear_all();
                    printf("\nBanco de dados limpo.\n");
                } else {
                    printf("\nSistema ocupado.\n");
                }
                continue;
            }

            if (strcmp(input, "TESTE") == 0) {
                if (system_is_idle_for_tests()) {
                    printf("\n[TESTE] LED + Servo 1...\n");
                    run_led_test_sequence();
                    servo_set_angle(SERVO_1, 90);
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    servo_set_angle(SERVO_1, 180);
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    servo_set_angle(SERVO_1, 0);
                    printf("[TESTE] Concluido.\n\n");
                } else {
                    printf("\nSistema ocupado.\n");
                }
                continue;
            }

            if (strcmp(input, "GRAVAR") == 0) {
                if (system_is_idle_for_tests()) {
                    current_state = STATE_RECORD_GET_PLANET;
                    printf("\n[MODO GRAVACAO] Digite o nome do planeta: ");
                    fflush(stdout);
                } else {
                    printf("\nSistema ocupado.\n");
                }
                continue;
            }

            if (strcmp(input, "PROXIMO") == 0) {
                if (current_state == STATE_RECORD_WAIT_NFC) {
                    current_state = STATE_RECORD_GET_PLANET;
                    target_record_planet[0] = '\0';
                    memset(last_serialized_uid, 0, sizeof(last_serialized_uid));
                    printf("\nDigite o nome do proximo planeta: ");
                    fflush(stdout);
                } else {
                    printf("\n'PROXIMO' so funciona durante gravacao NFC.\n");
                }
                continue;
            }

            if (strlen(input) > 0) {
                if (current_state == STATE_RECORD_GET_PLANET) {
                    strncpy(target_record_planet, input, sizeof(target_record_planet) - 1);
                    target_record_planet[sizeof(target_record_planet) - 1] = '\0';
                    current_state = STATE_RECORD_WAIT_NFC;
                    memset(last_serialized_uid, 0, sizeof(last_serialized_uid));
                    printf("\nPlaneta: %s\nAproxime as tags NFC...\n", target_record_planet);
                } else if (current_state == STATE_IDLE) {
                    printf("\nComando desconhecido. Digite AJUDA.\n");
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    free(data);
    vTaskDelete(NULL);
}

void leitura_timeout_callback(TimerHandle_t xTimer) {
    (void)xTimer;
    if (current_state == STATE_READING_NFC) {
        ESP_LOGE(TAG, "Timeout de 30s na busca NFC.");
        tcp_send_reply(TCP_REPLY_TIMEOUT);
        finish_nfc_search(false);
    }
}

void iniciar_timer_leitura(void) {
    if (leitura_timer != NULL) {
        xTimerStart(leitura_timer, 0);
    }
}   