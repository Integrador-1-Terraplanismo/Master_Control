#include <stdio.h>
#include <string.h>
#include "nfc_reader.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern void handle_nfc_detection(uint8_t *uid);

static const char *TAG = "NFC_READER";
static spi_device_handle_t spi_handle;
static TaskHandle_t nfc_task_handle = NULL;
static volatile bool nfc_reading_active = false;

static const uint8_t FACTORY_KEY[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static uint8_t last_uid[4] = {0};
static TickType_t last_uid_tick = 0;
#define NFC_DEBOUNCE_MS 1500

static void rc522_write_reg(uint8_t reg, uint8_t val) {
    uint8_t tx_data[2] = {(reg << 1) & 0x7E, val};
    spi_transaction_t t = {.length = 16, .tx_buffer = tx_data};
    spi_device_polling_transmit(spi_handle, &t);
}

static uint8_t rc522_read_reg(uint8_t reg) {
    uint8_t tx_data[2] = {((reg << 1) & 0x7E) | 0x80, 0};
    uint8_t rx_data[2] = {0};
    spi_transaction_t t = {.length = 16, .tx_buffer = tx_data, .rx_buffer = rx_data};
    spi_device_polling_transmit(spi_handle, &t);
    return rx_data[1];
}

static void rc522_set_bit_mask(uint8_t reg, uint8_t mask) {
    rc522_write_reg(reg, rc522_read_reg(reg) | mask);
}

static void rc522_clear_bit_mask(uint8_t reg, uint8_t mask) {
    rc522_write_reg(reg, rc522_read_reg(reg) & (~mask));
}

static esp_err_t rc522_to_card(uint8_t command, uint8_t *send_data, uint8_t send_len, uint8_t *back_data, uint32_t *back_len) {
    rc522_write_reg(CommandReg, 0x00);
    rc522_write_reg(ComIEnReg, 0x7F);
    rc522_set_bit_mask(FIFOLevelReg, 0x80);

    for (int i = 0; i < send_len; i++) {
        rc522_write_reg(FIFODataReg, send_data[i]);
    }

    rc522_write_reg(CommandReg, command);
    if (command == PCD_TRANSCEIVE) {
        rc522_set_bit_mask(BitFramingReg, 0x80);
    }

    int timeout = 2500;
    uint8_t n;
    do {
        n = rc522_read_reg(ComIEnReg);
        timeout--;
        esp_rom_delay_us(10);
    } while (timeout > 0 && !(n & 0x30));

    rc522_clear_bit_mask(BitFramingReg, 0x80);

    if (timeout == 0) {
        return ESP_ERR_TIMEOUT;
    }

    if (!(rc522_read_reg(ErrorReg) & 0x1B)) {
        if (back_data && back_len) {
            n = rc522_read_reg(FIFOLevelReg);
            if (n > *back_len) {
                n = *back_len;
            }
            *back_len = n;
            for (int i = 0; i < n; i++) {
                back_data[i] = rc522_read_reg(FIFODataReg);
            }
        }
        return ESP_OK;
    }
    return ESP_FAIL;
}

static esp_err_t rc522_request(uint8_t req_mode, uint8_t *tag_type) {
    uint32_t len = 2;
    rc522_write_reg(BitFramingReg, 0x07);
    return rc522_to_card(PCD_TRANSCEIVE, &req_mode, 1, tag_type, &len);
}

static esp_err_t rc522_anticoll(uint8_t *uid) {
    uint32_t len = 5;
    uint8_t cmd[2] = {PICC_ANTICOLL, 0x20};
    rc522_write_reg(BitFramingReg, 0x00);
    return rc522_to_card(PCD_TRANSCEIVE, cmd, 2, uid, &len);
}

static bool uid_is_duplicate(uint8_t *uid) {
    TickType_t now = xTaskGetTickCount();
    if (memcmp(last_uid, uid, 4) == 0 &&
        (now - last_uid_tick) < pdMS_TO_TICKS(NFC_DEBOUNCE_MS)) {
        return true;
    }
    memcpy(last_uid, uid, 4);
    last_uid_tick = now;
    return false;
}

void nfc_reader_init(void) {
    ESP_LOGI(TAG, "Configurando hardware SPI do RC522...");

    gpio_reset_pin(PIN_NUM_RST);
    gpio_set_direction(PIN_NUM_RST, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_NUM_RST, 1);

    spi_bus_config_t buscfg = {
        .miso_io_num = PIN_NUM_MISO,
        .mosi_io_num = PIN_NUM_MOSI,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 0
    };

    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 1000000,
        .mode = 0,
        .spics_io_num = PIN_NUM_CS,
        .queue_size = 7,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST, &devcfg, &spi_handle));

    gpio_set_level(PIN_NUM_RST, 0);
    esp_rom_delay_us(10);
    gpio_set_level(PIN_NUM_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    rc522_write_reg(CommandReg, PCD_RESETPHASE);
    vTaskDelay(pdMS_TO_TICKS(10));

    rc522_write_reg(TModeReg, 0x8D);
    rc522_write_reg(TPrescalerReg, 0x3E);
    rc522_write_reg(TReloadRegH, 0x00);
    rc522_write_reg(TReloadRegL, 0x1E);
    rc522_write_reg(ModeReg, 0x3D);
    rc522_write_reg(RFCfgReg, 0x70);
    rc522_set_bit_mask(TxControlReg, 0x03);

    uint8_t versao = rc522_read_reg(VersionReg);
    if (versao == 0x00 || versao == 0xFF) {
        ESP_LOGE(TAG, "Falha SPI RC522 (versao=0x%02X). Verifique cabos MISO/MOSI/CS/RST.", versao);
    } else {
        ESP_LOGI(TAG, "RC522 OK (firmware 0x%02X). Antena ativa.", versao);
    }
}

uint8_t nfc_reader_get_firmware_version(void) {
    if (spi_handle == NULL) {
        return 0;
    }
    return rc522_read_reg(VersionReg);
}

bool nfc_reader_self_test(char *report, size_t max_len) {
    uint8_t version = nfc_reader_get_firmware_version();
    bool ok = (version != 0x00 && version != 0xFF);
    if (report != NULL && max_len > 0) {
        snprintf(report, max_len,
                 "NFC: versao=0x%02X, leitura=%s, debounce=%dms",
                 version,
                 nfc_reader_is_reading() ? "ativa" : "parada",
                 NFC_DEBOUNCE_MS);
    }
    return ok;
}

bool nfc_reader_scan_once(char *uid_out, size_t uid_max_len) {
    uint8_t tag_type[2];
    uint8_t uid[5];

    if (rc522_request(PICC_REQALL, tag_type) != ESP_OK) {
        return false;
    }
    if (rc522_anticoll(uid) != ESP_OK) {
        return false;
    }

    if (uid_out != NULL && uid_max_len > 0) {
        snprintf(uid_out, uid_max_len, "%02X%02X%02X%02X", uid[0], uid[1], uid[2], uid[3]);
    }
    return true;
}

static void nfc_reading_task(void *pvParameters) {
    uint8_t tag_type[2];
    uint8_t uid[5];

    ESP_LOGI(TAG, "Tarefa de leitura NFC iniciada.");
    while (nfc_reading_active) {
        if (rc522_request(PICC_REQALL, tag_type) == ESP_OK &&
            rc522_anticoll(uid) == ESP_OK &&
            !uid_is_duplicate(uid)) {
            handle_nfc_detection(uid);
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    nfc_task_handle = NULL;
    vTaskDelete(NULL);
}

void nfc_reader_start_reading(void) {
    if (nfc_task_handle != NULL) {
        return;
    }
    nfc_reading_active = true;
    xTaskCreate(nfc_reading_task, "nfc_reading_task", 4096, NULL, 4, &nfc_task_handle);
    ESP_LOGI(TAG, "Leitura continua ativada.");
}

void nfc_reader_stop_reading(void) {
    if (nfc_task_handle == NULL) {
        return;
    }
    nfc_reading_active = false;
    for (int i = 0; i < 50 && nfc_task_handle != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (nfc_task_handle != NULL) {
        vTaskDelete(nfc_task_handle);
        nfc_task_handle = NULL;
    }
    ESP_LOGI(TAG, "Leitura continua desativada.");
}

bool nfc_reader_is_reading(void) {
    return nfc_reading_active && nfc_task_handle != NULL;
}

void nfc_reader_pause(void) {
    if (nfc_task_handle != NULL) {
        vTaskSuspend(nfc_task_handle);
    }
}

void nfc_reader_resume(void) {
    if (nfc_task_handle != NULL) {
        vTaskResume(nfc_task_handle);
    }
}

bool nfc_reader_write_data(uint8_t block, uint8_t *data_16_bytes) {
    uint8_t uid[5];
    uint8_t tag_type[2];

    if (rc522_request(PICC_REQIDL, tag_type) != ESP_OK || rc522_anticoll(uid) != ESP_OK) {
        ESP_LOGE(TAG, "Gravacao abortada: nenhuma tag presente.");
        return false;
    }

    uint8_t auth_cmd[12];
    auth_cmd[0] = 0x60;
    auth_cmd[1] = block;
    memcpy(&auth_cmd[2], FACTORY_KEY, 6);
    memcpy(&auth_cmd[8], uid, 4);

    uint32_t back_len = 0;
    if (rc522_to_card(PCD_AUTHENT, auth_cmd, 12, NULL, &back_len) != ESP_OK) {
        ESP_LOGE(TAG, "Erro de autenticacao no bloco %d.", block);
        return false;
    }

    uint8_t write_cmd[2] = {PICC_WRITE, block};
    if (rc522_to_card(PCD_TRANSCEIVE, write_cmd, 2, NULL, &back_len) != ESP_OK) {
        return false;
    }

    if (rc522_to_card(PCD_TRANSCEIVE, data_16_bytes, 16, NULL, &back_len) == ESP_OK) {
        ESP_LOGI(TAG, "Dados gravados no bloco %d.", block);
        return true;
    }

    ESP_LOGE(TAG, "Falha na gravacao fisica.");
    return false;
}
