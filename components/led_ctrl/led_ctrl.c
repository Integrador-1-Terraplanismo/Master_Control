#include "led_ctrl.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static portMUX_TYPE ir_mux = portMUX_INITIALIZER_UNLOCKED;

void led_ctrl_init(void) {
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    // Controladoras IR operam em nível ALTO (1) quando ociosas
    gpio_set_level(LED_GPIO, 1); 
}

// Envia um comando de 32 bits simulando o receptor IR (Protocolo NEC Real)
void led_ctrl_send_command(uint32_t nec_code) {
    // ✅ CORREÇÃO: Separa os bytes na ordem exata de transmissão do protocolo NEC
    uint8_t bytes[4];
    bytes[0] = (nec_code >> 24) & 0xFF; // Endereço (ex: 0x00)
    bytes[1] = (nec_code >> 16) & 0xFF; // Endereço Invertido (ex: 0xFF)
    bytes[2] = (nec_code >> 8) & 0xFF;  // Comando (ex: 0xB0)
    bytes[3] = nec_code & 0xFF;         // Comando Invertido (ex: 0x4F)

    vTaskSuspendAll();
    taskENTER_CRITICAL(&ir_mux);

    // 1. Leader Pulse (9ms em BAIXO, 4.5ms em ALTO)
    gpio_set_level(LED_GPIO, 0);
    esp_rom_delay_us(9000);
    gpio_set_level(LED_GPIO, 1);
    esp_rom_delay_us(4500);

    // 2. Envia os 4 bytes (Cada um do bit menos significativo LSB para o MSB)
    for (int b = 0; b < 4; b++) {
        for (int i = 0; i < 8; i++) {
            bool bit = (bytes[b] >> i) & 1;

            // Todo bit começa com um pulso BAIXO de 562us
            gpio_set_level(LED_GPIO, 0);
            esp_rom_delay_us(562);

            // O tempo em ALTO define se o bit é 0 ou 1
            gpio_set_level(LED_GPIO, 1);
            if (bit) {
                esp_rom_delay_us(1687); // Bit 1 = 1687us em ALTO
            } else {
                esp_rom_delay_us(562);  // Bit 0 = 562us em ALTO
            }
        }
    }

    // 3. Pulso de parada (Stop Bit)
    gpio_set_level(LED_GPIO, 0);
    esp_rom_delay_us(562);
    gpio_set_level(LED_GPIO, 1); // Retorna ao estado ocioso (ALTO)

    taskEXIT_CRITICAL(&ir_mux);
    xTaskResumeAll();
    
    // Pausa para a controladora processar o comando
    vTaskDelay(pdMS_TO_TICKS(150)); 
}

void led_ctrl_set_state(bool state) {
    if (state) {
        led_ctrl_send_command(IR_CMD_ON);
    } else {
        led_ctrl_send_command(IR_CMD_OFF);
    }
}