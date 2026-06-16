#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "wifi_tcp_mgr.h"
#include <netinet/tcp.h> // Necessário para TCP_NODELAY

// ---------------- Extern Declaration ----------------

extern void process_pc_command(const char* command, char* response_buffer, size_t max_resp_len);

// ---------------- CONFIGURAÇÕES DO WI-FI (AP) ----------------
#define ESP_WIFI_SSID      "TERRAPLANISMO"
#define ESP_WIFI_PASS      "terraplanismo_adm" // Mínimo de 8 caracteres
#define MAX_STA_CONN       2

// ---------------- CONFIGURAÇÕES DO TCP -----------------------
#define PORT               3333 // Porta de comunicação

static const char *TAG = "WIFI_TCP";
static int active_sock = -1; // Guarda o socket do PC quando ele conectar

// Envia a resposta de volta para o PC conectado
void tcp_send_reply(tcp_reply_t reply_type) {
    if (active_sock < 0) return; // Ninguém conectado

    const char* msg = "";
    switch (reply_type) {
        case TCP_REPLY_ACK:     msg = "mensagem recebida (ACK)\n"; break;
        case TCP_REPLY_RECD:    msg = "reconhecido (recd)\n"; break;
        case TCP_REPLY_NRECD:   msg = "nao reconhecido (nrecd)\n"; break;
        case TCP_REPLY_NPLT:    msg = "not a planet (nplt)\n"; break;
        case TCP_REPLY_TIMEOUT: msg = "tempo esgotado (timeout)\n"; break;
        case TCP_REPLY_BUSY:    msg = "sistema ocupado (busy)\n"; break;
    }
    
    send(active_sock, msg, strlen(msg), 0);
    ESP_LOGI(TAG, "Enviado: %s", msg);
}

// Task do Servidor TCP (Roda em background escutando o PC)
static void tcp_server_task(void *pvParameters) {
    char rx_buffer[128];
    int addr_family = AF_INET;
    int ip_protocol = IPPROTO_IP;

    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(PORT);

    int listen_sock = socket(addr_family, SOCK_STREAM, ip_protocol);
    bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    listen(listen_sock, 1);

    ESP_LOGI(TAG, "Servidor TCP rodando na porta %d", PORT);

    while (1) {
        struct sockaddr_in source_addr;
        socklen_t addr_len = sizeof(source_addr);
        
        // Fica travado aqui aguardando o PC se conectar
        int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (sock < 0) continue;

        int flag = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char *) &flag, sizeof(int));

        ESP_LOGI(TAG, "Computador conectado!");
        active_sock = sock;

        // Loop de recepção de mensagens do PC
        while (1) {
            int len = recv(sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
            if (len < 0) {
                break;
            } else if (len == 0) {
                break;
            } else {
                rx_buffer[len] = 0;
                
                // Prepara um buffer para construir a resposta dinâmica
                char tx_response[1024] = {0};
                
                // Processa o comando recebido do PC
                process_pc_command(rx_buffer, tx_response, sizeof(tx_response));
                
                // Envia a resposta gerada de volta para o PC via socket
                send(sock, tx_response, strlen(tx_response), 0);
            }
        }

        if (sock != -1) {
            active_sock = -1;
            shutdown(sock, 0);
            close(sock);
        }
    }
    vTaskDelete(NULL);
}

// Inicializa o Access Point
void wifi_tcp_init(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = ESP_WIFI_SSID,
            .ssid_len = strlen(ESP_WIFI_SSID),
            .channel = 1,
            .password = ESP_WIFI_PASS,
            .max_connection = MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi AP iniciado. SSID:%s password:%s", ESP_WIFI_SSID, ESP_WIFI_PASS);

    // Inicia a Task do TCP Server
    xTaskCreate(tcp_server_task, "tcp_server", 4096, NULL, 5, NULL);
}

void wifi_tcp_send_raw(const char* data) {
    if (active_sock >= 0 && data != NULL) {
        send(active_sock, data, strlen(data), 0);
    }
}