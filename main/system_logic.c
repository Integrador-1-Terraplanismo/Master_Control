#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/timers.h"

#include "system_logic.h"
#include "led_ctrl.h"
#include "servo_ctrl.h"
#include "wifi_tcp_mgr.h"
#include "storage_mgr.h"

static const char *TAG = "SYSTEM_LOGIC";
static TimerHandle_t leitura_timer = NULL;



// ✅ DEIXE APENAS ISTO NO system_logic.c (além dos seus outros includes e variáveis globais)
static system_state_t current_state = STATE_IDLE;static char requested_planet[32] = "";
static char target_record_planet[32] = "";
static int failed_attempts = 0;
static uint8_t last_serialized_uid[5] = {0};


void system_logic_init(void) {
    leitura_timer = xTimerCreate(
        "LeituraTimeoutTimer",
        pdMS_TO_TICKS(30000), // 30 segundos
        pdFALSE,              // pdFALSE faz o timer rodar apenas uma vez (one-shot)
        (void *)0,
        leitura_timeout_callback
    );
    
    if (leitura_timer == NULL) {
        ESP_LOGE(TAG, "FALHA CRÍTICA: Não foi possível alocar o Timer do FreeRTOS!");
    }
}

// Função interna auxiliar para converter strings para maiúsculas
static void string_to_uppercase(char *str) {
    for (int i = 0; str[i]; i++) {
        str[i] = toupper((unsigned char)str[i]);
    }
}

// Busca se o UID pertence a algum planeta no banco de dados
static bool find_planet_by_uid(const char* uid_str, char* found_name, size_t max_len) {
    char file_content[1024];
    storage_get_file_content(file_content, sizeof(file_content));
    
    char *line = strtok(file_content, "\n");
    while (line != NULL) {
        char name[32];
        char uid[32];
        if (sscanf(line, "%[^,],%s", name, uid) == 2) {
            if (strcmp(uid, uid_str) == 0) {
                strncpy(found_name, name, max_len);
                return true;
            }
        }
        line = strtok(NULL, "\n");
    }
    return false;
}

// Processa comandos operacionais vindos do Script Python via TCP
void process_pc_command(const char* command, char* response_buffer, size_t max_resp_len) {
    char cmd_copy[64];
    strncpy(cmd_copy, command, sizeof(cmd_copy));
    cmd_copy[strcspn(cmd_copy, "\r\n")] = 0;

   if (current_state == STATE_RECORD_GET_PLANET || current_state == STATE_RECORD_WAIT_NFC || current_state == STATE_MINIGAME) {
        snprintf(response_buffer, max_resp_len, "ERRO:ESP32 Ocupado\n");
        tcp_send_reply(TCP_REPLY_BUSY);
        return;
    } 

    if (strncmp(cmd_copy, "BUSCA:", 6) == 0) {
        if (current_state == STATE_IDLE) {
            iniciar_timer_leitura();
            strncpy(requested_planet, cmd_copy + 6, sizeof(requested_planet));
            string_to_uppercase(requested_planet);
            failed_attempts = 0;
           current_state = STATE_READING_NFC;
            
            // Proteção: Só inicia se o timer tiver sido criado com sucesso
            if (leitura_timer != NULL) {
                xTimerStart(leitura_timer, 0);
            } else {
                ESP_LOGE(TAG, "Erro: Tentativa de iniciar um timer nulo!");
            }
             snprintf(response_buffer, max_resp_len, "BUSCANDO:%s\n", requested_planet);
             
        } else {
            snprintf(response_buffer, max_resp_len, "ERRO:ESP32 em Modo de Gravacao Serial\n");
        }
    } else {
        snprintf(response_buffer, max_resp_len, "COMANDO_INVALIDO\n");
    }
    xTimerStop(leitura_timer, 0);
    current_state = STATE_IDLE;
}

// Executado em background pelo nfc_reader ao detectar uma Tag
void handle_nfc_detection(uint8_t *uid) {
    char uid_str[20];
    snprintf(uid_str, sizeof(uid_str), "%02X%02X%02X%02X", uid[0], uid[1], uid[2], uid[3]);

    if (current_state == STATE_RECORD_WAIT_NFC) {
        if (memcmp(last_serialized_uid, uid, 4) == 0) {
            return;
        }
        memcpy(last_serialized_uid, uid, 4);

        led_ctrl_set_state(true);
        storage_save_planet(target_record_planet, uid_str);
        printf("\n[GRAVADO] Tag %s associada ao planeta %s!\n", uid_str, target_record_planet);
        printf("Aproxime outra tag para %s ou envie 'END' para encerrar: ", target_record_planet);
        fflush(stdout);
        
        vTaskDelay(pdMS_TO_TICKS(500));
        led_ctrl_set_state(false);
        return;
    }

    if (current_state == STATE_IDLE && strlen(requested_planet) > 0) {
        char detected_planet_name[32] = "";
        bool found = find_planet_by_uid(uid_str, detected_planet_name, sizeof(detected_planet_name));

        if (found) {
            string_to_uppercase(detected_planet_name);
            
            if (strcmp(detected_planet_name, requested_planet) == 0) {
                printf("\n=================================================================\n");
                printf("[NFC] CONFIRMACAO DE LEITURA: Tag '%s' processada com sucesso.\n", uid_str);
                printf("[VINCULO] O codigo corresponde corretamente ao planeta solicitado: %s\n", detected_planet_name);
                printf("=================================================================\n\n");
                fflush(stdout);

                ESP_LOGI(TAG, "Planeta Correto: %s!", detected_planet_name);
                tcp_send_reply(TCP_REPLY_RECD);
                servo_set_angle(SERVO_1, 0);
                servo_set_angle(SERVO_2, 0);
                led_ctrl_set_state(true);
                requested_planet[0] = '\0';
                vTaskDelay(pdMS_TO_TICKS(2000));
                servo_set_angle(SERVO_1, 90);
                servo_set_angle(SERVO_2, 90);
                failed_attempts = 0;
            } 
            else {
                ESP_LOGW(TAG, "Planeta Errado: %s (Esperado: %s)", detected_planet_name, requested_planet);
                tcp_send_reply(TCP_REPLY_NRECD);
                failed_attempts++;
            }
        } 
        else {
            ESP_LOGE(TAG, "Codigo NFC Desconhecido.");
            tcp_send_reply(TCP_REPLY_NPLT);
            failed_attempts++;
        }

        if (failed_attempts >= 3) {
            printf("\n!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
            printf("[BLOQUEIO] Limite de 3 tentativas falhas atingido para: %s\n", requested_planet);
            printf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n\n");
            fflush(stdout);

            ESP_LOGE(TAG, "3 tentativas falhas atingidas. Bloqueando mecanismo...");
            servo_set_angle(SERVO_1, 180);
            servo_set_angle(SERVO_2, 180);
            led_ctrl_set_state(false);
            requested_planet[0] = '\0';
            vTaskDelay(pdMS_TO_TICKS(2000));
            servo_set_angle(SERVO_1, 90);
            servo_set_angle(SERVO_2, 90);
            failed_attempts = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
}

// Task que gerencia a entrada de comandos via Monitor Serial (UART)
void serial_monitor_task(void *pvParameters) {
    uint8_t* data = (uint8_t*) malloc(BUF_SIZE);
    printf("\n=== CONSOLE SERIAL PRONTO ===\n");
    printf("Digite 'GRAVAR' para iniciar o mapeamento de planetas\n");
    printf("=============================\n");

    while (1) {
        int len = uart_read_bytes(EX_UART_NUM, data, BUF_SIZE - 1, pdMS_TO_TICKS(100));
        if (len > 0) {
            data[len] = 0;
            char *input = (char*)data;
            input[strcspn(input, "\r\n")] = 0;
            string_to_uppercase(input);

            if (strcmp(input, "LISTAR") == 0) {
                char buffer_local[1024];
                storage_get_file_content(buffer_local, sizeof(buffer_local));
                printf("\n--- CONTEÚDO DO ARQUIVO PLANETAS.TXT ---\n");
                printf("%s", buffer_local);
                printf("----------------------------------------\n\n");
                fflush(stdout);
                continue;
            }

            if (strcmp(input, "END") == 0) {
                if (current_state != STATE_IDLE) {
                    current_state = STATE_IDLE;
                    target_record_planet[0] = '\0';
                    memset(last_serialized_uid, 0, sizeof(last_serialized_uid));
                    printf("\n[AVISO] Modo de gravacao encerrado. Retornando ao Modo Normal.\n");
                } else {
                    printf("\nO sistema ja esta no Modo Normal.\n");
                }
            }
            
            else if (strcmp(input, "LIMPAR") == 0) {
                if (current_state == STATE_IDLE) {
                    // Executa a função que apaga o arquivo do SPIFFS
                    storage_clear_all();
                    
                    printf("\n[SUCESSO] O banco de dados (planetas.txt) foi totalmente limpo!\n");
                    printf("Você já pode digitar 'GRAVAR' para iniciar um novo mapeamento.\n");
                    fflush(stdout);
                } else {
                    printf("\nNão é possível limpar o banco de dados enquanto o sistema estiver ocupado.\n");
                }
            }

            else if (strcmp(input, "GRAVAR") == 0) {
                if (current_state == STATE_IDLE) {
                    current_state = STATE_RECORD_GET_PLANET;
                    printf("\n[MODO GRAVACAO] Digite o nome do planeta: ");
                    fflush(stdout);
                } else {
                    printf("\nO sistema ja esta executando uma rotina de gravacao.\n");
                }
            }


                else if (strcmp(input, "PROXIMO") == 0) {
                if (current_state == STATE_RECORD_WAIT_NFC) {
                    current_state = STATE_RECORD_GET_PLANET;
                    target_record_planet[0] = '\0';
                    memset(last_serialized_uid, 0, sizeof(last_serialized_uid));
                    printf("\n[MODO GRAVACAO] Digite o nome do proximo planeta: ");
                    fflush(stdout);
                } else {
                    printf("\nO comando 'PROXIMO' so pode ser usado enquanto voce esta vinculando tags.\n");
                }
            }


            else if (strlen(input) > 0) {
                if (current_state == STATE_RECORD_GET_PLANET) {
                    strncpy(target_record_planet, input, sizeof(target_record_planet));
                    current_state = STATE_RECORD_WAIT_NFC;
                    memset(last_serialized_uid, 0, sizeof(last_serialized_uid));
                    
                    printf("\nPlaneta definido: %s\n", target_record_planet);
                    printf("Aproxime as tags NFC ao leitor agora para vincula-las...\n");
                    printf("Envie 'END' a qualquer momento para fechar o loop.\n");
                } 
                else if (current_state == STATE_IDLE) {
                    printf("\nComando desconhecido. Digite 'GRAVAR' para configurar.\n");
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    free(data);
    vTaskDelete(NULL);
}

void leitura_timeout_callback(TimerHandle_t xTimer) {
    if (current_state == STATE_READING_NFC) {
        ESP_LOGE(TAG, "Timeout de 30s atingido. Nenhuma tag correta lida.");
        tcp_send_reply(TCP_REPLY_TIMEOUT);
        
        current_state = STATE_IDLE;
        requested_planet[0] = '\0';
        servo_set_angle(SERVO_1, 180); 
        servo_set_angle(SERVO_2, 180);
        led_ctrl_set_state(false);
    }
}

void init_timers(void) {
    leitura_timer = xTimerCreate("LeituraTimer", pdMS_TO_TICKS(30000), pdFALSE, (void *)0, leitura_timeout_callback);
}

void iniciar_timer_leitura(void) {
    if (leitura_timer != NULL) {
        xTimerStart(leitura_timer, 0);
    }
}