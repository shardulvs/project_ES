#include "rc522.h"
#include "debug.h"

static SPI_HandleTypeDef *rc_hspi;
static GPIO_TypeDef *rc_ss_port, *rc_rst_port;
static uint16_t      rc_ss_pin,  rc_rst_pin;

static inline void CS_LOW(void)  { HAL_GPIO_WritePin(rc_ss_port, rc_ss_pin, GPIO_PIN_RESET); }
static inline void CS_HIGH(void) { HAL_GPIO_WritePin(rc_ss_port, rc_ss_pin, GPIO_PIN_SET);   }

static void rc_write(uint8_t reg, uint8_t val) {
    uint8_t tx[2] = { (reg << 1) & 0x7E, val };
    CS_LOW();
    HAL_SPI_Transmit(rc_hspi, tx, 2, 100);
    CS_HIGH();
}

static uint8_t rc_read(uint8_t reg) {
    uint8_t tx = ((reg << 1) & 0x7E) | 0x80;
    uint8_t rx = 0, zero = 0;
    CS_LOW();
    HAL_SPI_Transmit(rc_hspi, &tx, 1, 100);
    HAL_SPI_TransmitReceive(rc_hspi, &zero, &rx, 1, 100);
    CS_HIGH();
    return rx;
}

static void rc_set_bit(uint8_t reg, uint8_t mask) { rc_write(reg, rc_read(reg) | mask); }
static void rc_clr_bit(uint8_t reg, uint8_t mask) { rc_write(reg, rc_read(reg) & ~mask); }

static void rc_antenna_on(void) {
    if (!(rc_read(MFRC522_REG_TX_CONTROL) & 0x03))
        rc_set_bit(MFRC522_REG_TX_CONTROL, 0x03);
}

static void rc_reset(void) {
    HAL_GPIO_WritePin(rc_rst_port, rc_rst_pin, GPIO_PIN_RESET);
    HAL_Delay(5);
    HAL_GPIO_WritePin(rc_rst_port, rc_rst_pin, GPIO_PIN_SET);
    HAL_Delay(50);
    rc_write(MFRC522_REG_COMMAND, MFRC522_CMD_SOFT_RESET);
    HAL_Delay(50);
}

void RC522_Init(SPI_HandleTypeDef *hspi, GPIO_TypeDef *ss_port, uint16_t ss_pin,
                GPIO_TypeDef *rst_port, uint16_t rst_pin) {
    rc_hspi = hspi;
    rc_ss_port = ss_port;  rc_ss_pin = ss_pin;
    rc_rst_port = rst_port; rc_rst_pin = rst_pin;
    CS_HIGH();

    rc_reset();

    /* Timer: TPrescaler*TreloadVal/6.78MHz = 24ms */
    rc_write(MFRC522_REG_T_MODE,      0x8D);
    rc_write(MFRC522_REG_T_PRESCALER, 0x3E);
    rc_write(MFRC522_REG_T_RELOAD_L,  30);
    rc_write(MFRC522_REG_T_RELOAD_H,  0);

    rc_write(MFRC522_REG_TX_AUTO, 0x40);   /* 100% ASK modulation */
    rc_write(MFRC522_REG_MODE,    0x3D);   /* CRC preset 0x6363 */

    rc_antenna_on();

    uint8_t v = rc_read(MFRC522_REG_VERSION);
    DBG("RC522 Version: 0x%02X", v);
    if (v == 0x00 || v == 0xFF) {
        DBGE("RC522 not responding. Check SPI wiring / power / RST.");
    }
}

uint8_t RC522_Version(void) { return rc_read(MFRC522_REG_VERSION); }

static uint8_t rc_to_card(uint8_t cmd, uint8_t *send, uint8_t send_len,
                          uint8_t *back, uint16_t *back_len) {
    uint8_t status = MI_ERR;
    uint8_t irq_en = 0, wait_irq = 0, last_bits;
    uint16_t n, i;

    if (cmd == MFRC522_CMD_TRANSCEIVE) { irq_en = 0x77; wait_irq = 0x30; }

    rc_write(MFRC522_REG_COM_IEN, irq_en | 0x80);
    rc_clr_bit(MFRC522_REG_COM_IRQ, 0x80);
    rc_set_bit(MFRC522_REG_FIFO_LEVEL, 0x80);

    rc_write(MFRC522_REG_COMMAND, MFRC522_CMD_IDLE);
    for (i = 0; i < send_len; i++) rc_write(MFRC522_REG_FIFO_DATA, send[i]);
    rc_write(MFRC522_REG_COMMAND, cmd);
    if (cmd == MFRC522_CMD_TRANSCEIVE) rc_set_bit(MFRC522_REG_BIT_FRAMING, 0x80);

    i = 2000;
    do {
        n = rc_read(MFRC522_REG_COM_IRQ);
        i--;
    } while (i && !(n & 0x01) && !(n & wait_irq));

    rc_clr_bit(MFRC522_REG_BIT_FRAMING, 0x80);

    if (i == 0) return MI_ERR;
    if (rc_read(MFRC522_REG_ERROR) & 0x1B) return MI_ERR;

    status = MI_OK;
    if (n & irq_en & 0x01) status = MI_NOTAGERR;

    if (cmd == MFRC522_CMD_TRANSCEIVE) {
        n = rc_read(MFRC522_REG_FIFO_LEVEL);
        last_bits = rc_read(MFRC522_REG_CONTROL) & 0x07;
        if (last_bits) *back_len = (n - 1) * 8 + last_bits;
        else           *back_len = n * 8;
        if (n == 0)   n = 1;
        if (n > 16)   n = 16;
        for (i = 0; i < n; i++) back[i] = rc_read(MFRC522_REG_FIFO_DATA);
    }
    return status;
}

uint8_t RC522_Request(uint8_t req_mode, uint8_t *tag_type) {
    uint16_t back_bits = 0;
    rc_write(MFRC522_REG_BIT_FRAMING, 0x07);
    uint8_t data = req_mode;
    uint8_t status = rc_to_card(MFRC522_CMD_TRANSCEIVE, &data, 1, tag_type, &back_bits);
    if (status != MI_OK || back_bits != 0x10) return MI_ERR;
    return MI_OK;
}

uint8_t RC522_Anticoll(uint8_t *serial) {
    uint8_t data[2] = { PICC_ANTICOLL, 0x20 };
    uint16_t back_bits = 0;
    uint8_t chk = 0;

    rc_write(MFRC522_REG_BIT_FRAMING, 0x00);
    uint8_t status = rc_to_card(MFRC522_CMD_TRANSCEIVE, data, 2, serial, &back_bits);
    if (status != MI_OK) return status;

    for (int i = 0; i < 4; i++) chk ^= serial[i];
    if (chk != serial[4]) {
        DBGE("UID BCC mismatch");
        return MI_ERR;
    }
    return MI_OK;
}

void RC522_Halt(void) {
    uint8_t buf[2] = { PICC_HALT, 0 };
    uint8_t back[8];
    uint16_t bl = 0;
    rc_to_card(MFRC522_CMD_TRANSCEIVE, buf, 2, back, &bl);
}

uint8_t RC522_ReadUID(uint8_t *uid) {
    uint8_t tag_type[2];
    if (RC522_Request(PICC_REQIDL, tag_type) != MI_OK) return 0;
    if (RC522_Anticoll(uid) != MI_OK) return 0;
    RC522_Halt();
    return 1;
}
