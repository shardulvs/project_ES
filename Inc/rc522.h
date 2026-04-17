#ifndef RC522_H
#define RC522_H

#include <stdint.h>

/* Wiring: PA3=RST, PA4=SS (NSS), SPI1 on PA5/6/7 */

/* MFRC522 Register map (subset) */
#define MFRC522_REG_COMMAND        0x01
#define MFRC522_REG_COM_IEN        0x02
#define MFRC522_REG_COM_IRQ        0x04
#define MFRC522_REG_DIV_IRQ        0x05
#define MFRC522_REG_ERROR          0x06
#define MFRC522_REG_STATUS_2       0x08
#define MFRC522_REG_FIFO_DATA      0x09
#define MFRC522_REG_FIFO_LEVEL     0x0A
#define MFRC522_REG_CONTROL        0x0C
#define MFRC522_REG_BIT_FRAMING    0x0D
#define MFRC522_REG_MODE           0x11
#define MFRC522_REG_TX_MODE        0x12
#define MFRC522_REG_RX_MODE        0x13
#define MFRC522_REG_TX_CONTROL     0x14
#define MFRC522_REG_TX_AUTO        0x15
#define MFRC522_REG_CRC_RESULT_H   0x21
#define MFRC522_REG_CRC_RESULT_L   0x22
#define MFRC522_REG_T_MODE         0x2A
#define MFRC522_REG_T_PRESCALER    0x2B
#define MFRC522_REG_T_RELOAD_H     0x2C
#define MFRC522_REG_T_RELOAD_L     0x2D
#define MFRC522_REG_VERSION        0x37

/* Commands */
#define MFRC522_CMD_IDLE           0x00
#define MFRC522_CMD_CALC_CRC       0x03
#define MFRC522_CMD_TRANSCEIVE     0x0C
#define MFRC522_CMD_SOFT_RESET     0x0F

/* PICC Commands */
#define PICC_REQIDL                0x26
#define PICC_ANTICOLL              0x93
#define PICC_HALT                  0x50

#define MI_OK       0
#define MI_NOTAGERR 1
#define MI_ERR      2

#define RC522_UID_LEN 5

void    RC522_Init(void);
uint8_t RC522_Version(void);
uint8_t RC522_Request(uint8_t req_mode, uint8_t *tag_type);
uint8_t RC522_Anticoll(uint8_t *serial);
void    RC522_Halt(void);
uint8_t RC522_ReadUID(uint8_t *uid);   /* returns 1 if UID present */

#endif
