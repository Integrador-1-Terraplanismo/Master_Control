#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"

#include "system_logic.h"
#include "led_ctrl.h"
#include "servo_ctrl.h"
#include "wifi_tcp_mgr.h"
#include "nfc_reader.h"
#include "storage_mgr.h"

void app_main(void) {
    // Inicialização de todos os subcomponentes de hardware e storage
    led_ctrl_init();
    servo_ctrl_init();
    storage_init();
    nfc_reader_init();
    system_logic_init();
    vTaskDelay(pdMS_TO_TICKS(300));
    wifi_tcp_init(); 
    
    // Configura e instala o driver da UART0 para o Console Serial
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(EX_UART_NUM, BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(EX_UART_NUM, &uart_config));

    // Inicializa a varredura contínua do leitor NFC em background
    nfc_reader_start_reading();
    
    // Cria a tarefa que gerencia o console serial, agora localizada em system_logic.c
    xTaskCreate(serial_monitor_task, "serial_task", 4096, NULL, 10, NULL);

    // Define os estados iniciais seguros dos atuadores
    servo_set_angle(SERVO_1, 90);
    servo_set_angle(SERVO_2, 90);
    servo_set_angle(SERVO_3, 0);
    servo_set_angle(SERVO_4, 0);
    led_ctrl_set_state(false);

    // Loop principal da main apenas mantendo o clock vivo
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}