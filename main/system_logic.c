#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/timers.h"

#include "system_logic.h"
#include "led_ctrl.h"
#include "servo_ctrl.h"
#include "wifi_tcp_mgr.h"
#include "storage_mgr.h"
#include "nfc_reader.h"

static const char *TAG = "SYSTEM_LOGIC";
static TimerHandle_t leitura_timer = NULL;
static QueueHandle_t nfc_queue = NULL;

static system_state_t current_state = STATE_IDLE;
static char requested_planet[32] = "";
static char target_record_planet[32] = "";
static int failed_attempts = 0;
static uint8_t last_serialized_uid[5] = {0};
static bool test_nfc_servo_mode = false;

// Protótipos das funções estáticas para evitar erros de declaração implícita
static void string_to_uppercase(char *str);
static void stop_leitura_timer(void);
static void finish_nfc_search(bool success);
static bool find_planet_by_uid(const char *uid_str, char *found_name, size_t max_len);
static void logic_actuation_task(void *pvParameters);
static void run_isolated_servo_test(int servo_num);
static void run_pair_servo_test(int servo_a, int servo_b);
static bool system_is_busy_for_pc(void);
static bool system_is_idle_for_tests(void);

system_state_t system_logic_get_state(void) {
    return current_state;
}

const char *system_logic_state_name(system_state_t state) {
    switch (state) {
        case STATE_IDLE: return "IDLE";
        case STATE_RECORD_GET_PLANET: return "GRAVACAO_NOME";
        case STATE_RECORD_WAIT_NFC: return "GRAVACAO_NFC";
        case STATE_READING_NFC: return "LEITURA_NFC";
        default: return "DESCONHECIDO";
    }
}

static bool system_is_busy_for_pc(void) {
    return current_state == STATE_RECORD_GET_PLANET ||
           current_state == STATE_RECORD_WAIT_NFC ||
           current_state == STATE_READING_NFC;
}

static bool system_is_idle_for_tests(void) {
    return current_state == STATE_IDLE;
}

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

    // Criação da fila para desacoplar a leitura NFC da atuação dos servos
    nfc_queue = xQueueCreate(5, sizeof(uint8_t) * 5);
    if (nfc_queue == NULL) {
        ESP_LOGE(TAG, "Falha ao criar fila do NFC.");
    }

    // Task que processará a lógica de rotação e os tempos de espera de forma estável
    xTaskCreate(logic_actuation_task, "logic_task", 4096, NULL, 5, NULL);
}

// Tarefa Assíncrona de Processamento e Atuação
static void logic_actuation_task(void *pvParameters) {
    uint8_t uid[5];

    while (1) {
        if (xQueueReceive(nfc_queue, &uid, portMAX_DELAY)) {
            char uid_str[20];
            snprintf(uid_str, sizeof(uid_str), "%02X%02X%02X%02X", uid[0], uid[1], uid[2], uid[3]);

            // Modo de Teste Combinado: NFC + Servo Direto
            if (test_nfc_servo_mode) {
                printf("\n[TESTE NFC+SERVO] Tag detectada: %s! Movendo SERVO_1...\n", uid_str);
                fflush(stdout);
                servo_set_angle(SERVO_1, 0);
                vTaskDelay(pdMS_TO_TICKS(1000));
                servo_set_angle(SERVO_1, 90);
                continue;
            }

            // Modo de Gravação de Tags
            if (current_state == STATE_RECORD_WAIT_NFC) {
                if (memcmp(last_serialized_uid, uid, 4) == 0) {
                    continue;
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

            // Modo de Validação e Leitura Real (Transmissão para o Cliente)
            if (current_state != STATE_READING_NFC || requested_planet[0] == '\0') {
                continue;
            }

            char detected_planet_name[32] = "";
            bool found = find_planet_by_uid(uid_str, detected_planet_name, sizeof(detected_planet_name));

            if (found) {
                string_to_uppercase(detected_planet_name);

                if (strcmp(detected_planet_name, requested_planet) == 0) {
                    printf("\n[NFC OK] Tag %s confirmada para %s\n", uid_str, detected_planet_name);
                    fflush(stdout);

                    tcp_send_reply(TCP_REPLY_RECD); // Envia confirmação via TCP para o PC
                    servo_set_angle(SERVO_1, 0);
                    servo_set_angle(SERVO_2, 0);
                    led_ctrl_set_state(true);
                    
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

// Funções de Testes Individuais e em Par
static void run_isolated_servo_test(int servo_num) {
    printf("[TESTE] A mover Servo %d isolado...\n", servo_num);
    servo_set_angle(servo_num, 0);
    vTaskDelay(pdMS_TO_TICKS(1000));
    servo_set_angle(servo_num, 180);
    vTaskDelay(pdMS_TO_TICKS(1000));
    servo_set_angle(servo_num, 90);
}

static void run_pair_servo_test(int servo_a, int servo_b) {
    printf("[TESTE] A mover Par de Servos %d e %d em simultaneo...\n", servo_a, servo_b);
    servo_set_angle(servo_a, 0);
    servo_set_angle(servo_b, 0);
    vTaskDelay(pdMS_TO_TICKS(1000));
    servo_set_angle(servo_a, 180);
    servo_set_angle(servo_b, 180);
    vTaskDelay(pdMS_TO_TICKS(1000));
    servo_set_angle(servo_a, 90);
    servo_set_angle(servo_b, 90);
}

// Processamento de Comandos vindos do PC (Wi-Fi TCP)
void process_pc_command(const char *command, char *response_buffer, size_t max_resp_len) {
    char cmd_copy[64];
    strncpy(cmd_copy, command, sizeof(cmd_copy) - 1);
    cmd_copy[sizeof(cmd_copy) - 1] = '\0';
    cmd_copy[strcspn(cmd_copy, "\r\n")] = 0;

    if (system_is_busy_for_pc() && strcmp(cmd_copy, "STATUS") != 0) {
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

    // Comandos de Testes Isolados de Servos
    if (strcmp(cmd_copy, "TESTE_SERVO1") == 0) {
        if (!system_is_idle_for_tests()) { snprintf(response_buffer, max_resp_len, "busy\n"); return; }
        run_isolated_servo_test(SERVO_1);
        snprintf(response_buffer, max_resp_len, "TESTE_SERVO1_CONCLUIDO\n");
        return;
    }
    if (strcmp(cmd_copy, "TESTE_SERVO2") == 0) {
        if (!system_is_idle_for_tests()) { snprintf(response_buffer, max_resp_len, "busy\n"); return; }
        run_isolated_servo_test(SERVO_2);
        snprintf(response_buffer, max_resp_len, "TESTE_SERVO2_CONCLUIDO\n");
        return;
    }
    if (strcmp(cmd_copy, "TESTE_SERVO3") == 0) {
        if (!system_is_idle_for_tests()) { snprintf(response_buffer, max_resp_len, "busy\n"); return; }
        run_isolated_servo_test(SERVO_3);
        snprintf(response_buffer, max_resp_len, "TESTE_SERVO3_CONCLUIDO\n");
        return;
    }
    if (strcmp(cmd_copy, "TESTE_SERVO4") == 0) {
        if (!system_is_idle_for_tests()) { snprintf(response_buffer, max_resp_len, "busy\n"); return; }
        run_isolated_servo_test(SERVO_4);
        snprintf(response_buffer, max_resp_len, "TESTE_SERVO4_CONCLUIDO\n");
        return;
    }

    // Comandos de Testes em Par
    if (strcmp(cmd_copy, "TESTE_PAR12") == 0) {
        if (!system_is_idle_for_tests()) { snprintf(response_buffer, max_resp_len, "busy\n"); return; }
        run_pair_servo_test(SERVO_1, SERVO_2);
        snprintf(response_buffer, max_resp_len, "TESTE_PAR12_CONCLUIDO\n");
        return;
    }
    if (strcmp(cmd_copy, "TESTE_PAR34") == 0) {
        if (!system_is_idle_for_tests()) { snprintf(response_buffer, max_resp_len, "busy\n"); return; }
        run_pair_servo_test(SERVO_3, SERVO_4);
        snprintf(response_buffer, max_resp_len, "TESTE_PAR34_CONCLUIDO\n");
        return;
    }

    // Ativação do Modo Combinado NFC + Movimento de Servo
    if (strcmp(cmd_copy, "TESTE_NFC_SERVO") == 0) {
        if (!system_is_idle_for_tests()) { snprintf(response_buffer, max_resp_len, "busy\n"); return; }
        test_nfc_servo_mode = !test_nfc_servo_mode;
        snprintf(response_buffer, max_resp_len, "TESTE_NFC_SERVO:%s\n", test_nfc_servo_mode ? "ATIVADO" : "DESATIVADO");
        return;
    }

    if (strcmp(cmd_copy, "STATUS") == 0) {
        char nfc_report[96];
        nfc_reader_self_test(nfc_report, sizeof(nfc_report));
        snprintf(response_buffer, max_resp_len, "STATUS:estado=%s,nfc=%s,modo_combinado=%s\n",
                 system_logic_state_name(current_state), nfc_report, test_nfc_servo_mode ? "ON" : "OFF");
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

void handle_nfc_detection(uint8_t *uid) {
    if (nfc_queue != NULL) {
        xQueueSend(nfc_queue, uid, 0); // Despacha imediatamente para a fila assíncrona
    }
}

// Monitoramento e Menu via Console Serial (UART)
void serial_monitor_task(void *pvParameters) {
    uint8_t *data = (uint8_t *)malloc(BUF_SIZE);

    printf("\n=== CONSOLE SERIAL (ESSENCIAL) ===\n");
    printf("GRAVAR        - Mapear planetas NFC\n");
    printf("LISTAR        - Listar base de dados (planetas)\n");
    printf("LIMPAR        - Apagar planetas.txt\n");
    printf("STATUS        - Estado atual do sistema\n");
    printf("TESTE_SERVO1  - Testar Servo 1 isolado\n");
    printf("TESTE_SERVO2  - Testar Servo 2 isolado\n");
    printf("TESTE_SERVO3  - Testar Servo 3 isolado\n");
    printf("TESTE_SERVO4  - Testar Servo 4 isolado\n");
    printf("TESTE_PAR12   - Testar Par de Servos 1 e 2\n");
    printf("TESTE_PAR34   - Testar Par de Servos 3 e 4\n");
    printf("TESTE_NFC_SERVO - Alternar teste combinado NFC + Servo\n");
    printf("END / PROXIMO - Controlo do fluxo de gravacao\n");
    printf("==================================\n\n");

    while (1) {
        int len = uart_read_bytes(EX_UART_NUM, data, BUF_SIZE - 1, pdMS_TO_TICKS(100));
        if (len > 0) {
            data[len] = 0;
            char *input = (char *)data;
            input[strcspn(input, "\r\n")] = 0;
            string_to_uppercase(input);

            if (strcmp(input, "STATUS") == 0) {
                char report[96];
                nfc_reader_self_test(report, sizeof(report));
                printf("\nEstado: %s | Modo Combinado NFC+Servo: %s\n%s\n\n", 
                       system_logic_state_name(current_state), test_nfc_servo_mode ? "ON" : "OFF", report);
                continue;
            }

            if (strcmp(input, "LISTAR") == 0) {
                char buffer_local[1024];
                storage_get_file_content(buffer_local, sizeof(buffer_local));
                printf("\n--- PLANETAS.TXT ---\n%s--------------------\n\n", buffer_local);
                continue;
            }

            if (strcmp(input, "LIMPAR") == 0) {
                if (system_is_idle_for_tests()) { storage_clear_all(); printf("\nBase de dados limpa.\n"); }
                else { printf("\nSistema ocupado.\n"); }
                continue;
            }

            if (strncmp(input, "TESTE_SERVO", 11) == 0 || strncmp(input, "TESTE_PAR", 9) == 0 || strcmp(input, "TESTE_NFC_SERVO") == 0) {
                char resp[128];
                process_pc_command(input, resp, sizeof(resp));
                printf("\n%s", resp);
                continue;
            }

            if (strcmp(input, "GRAVAR") == 0) {
                if (system_is_idle_for_tests()) {
                    current_state = STATE_RECORD_GET_PLANET;
                    printf("\n[MODO GRAVACAO] Introduza o nome do planeta: ");
                    fflush(stdout);
                } else { printf("\nSistema ocupado.\n"); }
                continue;
            }

            if (strcmp(input, "PROXIMO") == 0) {
                if (current_state == STATE_RECORD_WAIT_NFC) {
                    current_state = STATE_RECORD_GET_PLANET;
                    target_record_planet[0] = '\0';
                    memset(last_serialized_uid, 0, sizeof(last_serialized_uid));
                    printf("\nIntroduza o nome do proximo planeta: ");
                    fflush(stdout);
                } else { printf("\n'PROXIMO' so funciona durante a gravacao NFC.\n"); }
                continue;
            }

            if (strcmp(input, "END") == 0) {
                if (current_state != STATE_IDLE || test_nfc_servo_mode) {
                    current_state = STATE_IDLE;
                    test_nfc_servo_mode = false;
                    target_record_planet[0] = '\0';
                    requested_planet[0] = '\0';
                    memset(last_serialized_uid, 0, sizeof(last_serialized_uid));
                    stop_leitura_timer();
                    printf("\nModo normal restaurado (Testes desativados).\n");
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
                    printf("\nComando desconhecido.\n");
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