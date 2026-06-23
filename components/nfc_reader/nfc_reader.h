#ifndef NFC_READER_H
#define NFC_READER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define PLANET_DATA_BLOCK  4

#define PIN_NUM_MISO 19
#define PIN_NUM_MOSI 23
#define PIN_NUM_CLK  18
#define PIN_NUM_CS   5
#define PIN_NUM_RST  22

#define CommandReg    0x01
#define ComIEnReg     0x04
#define ErrorReg      0x06
#define FIFODataReg   0x09
#define FIFOLevelReg  0x0A
#define BitFramingReg 0x0D
#define ModeReg       0x11
#define TxControlReg  0x14
#define VersionReg    0x37
#define RFCfgReg      0x26
#define TModeReg      0x2A
#define TPrescalerReg 0x2B
#define TReloadRegH   0x2C
#define TReloadRegL   0x2D

#define PCD_RESETPHASE   0x0F
#define PCD_TRANSCEIVE   0x0C
#define PCD_AUTHENT      0x0E
#define PICC_REQIDL      0x26
#define PICC_REQALL      0x52
#define PICC_ANTICOLL    0x93
#define PICC_WRITE       0xA0

void nfc_reader_init(void);
void nfc_reader_start_reading(void);
void nfc_reader_stop_reading(void);
void nfc_reader_pause(void);
void nfc_reader_resume(void);
bool nfc_reader_is_reading(void);

uint8_t nfc_reader_get_firmware_version(void);
bool nfc_reader_self_test(char *report, size_t max_len);
bool nfc_reader_scan_once(char *uid_out, size_t uid_max_len);
bool nfc_reader_write_data(uint8_t block, uint8_t *data_16_bytes);

#endif
