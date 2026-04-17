#include "rc522.h"
#include "bsp.h"
#include "debug.h"

/* NSS = PA4, RST = PA3 (controlled as GPIOs; SPI is software NSS) */
#define SS_LOW()   gpio_write(GPIOA, 4, 0)
#define SS_HIGH()  gpio_write(GPIOA, 4, 1)
#define RST_LOW()  gpio_write(GPIOA, 3, 0)
#define RST_HIGH() gpio_write(GPIOA, 3, 1)

static void rc_write(uint8_t reg, uint8_t val) {
    SS_LOW();
    spi1_txrx((reg << 1) & 0x7E);
    spi1_txrx(val);
    SS_HIGH();
}

static uint8_t rc_read(uint8_t reg) {
    SS_LOW();
    spi1_txrx(((reg << 1) & 0x7E) | 0x80);
    uint8_t v = spi1_txrx(0x00);
    SS_HIGH();
    return v;
}

static void rc_set_bit(uint8_t reg, uint8_t m) { rc_write(reg, rc_read(reg) |  m); }
static void rc_clr_bit(uint8_t reg, uint8_t m) { rc_write(reg, rc_read(reg) & ~m); }

static void rc_antenna_on(void) {
    if (!(rc_read(MFRC522_REG_TX_CONTROL) & 0x03))
        rc_set_bit(MFRC522_REG_TX_CONTROL, 0x03);
}

void RC522_Init(void) {
    /* PA3 (RST) and PA4 (NSS) as push-pull outputs, idle HIGH */
    gpio_clk_enable(GPIOA);
    gpio_mode (GPIOA, 3, 1); gpio_otype(GPIOA, 3, 0); gpio_speed(GPIOA, 3, 2); gpio_write(GPIOA, 3, 1);
    gpio_mode (GPIOA, 4, 1); gpio_otype(GPIOA, 4, 0); gpio_speed(GPIOA, 4, 2); gpio_write(GPIOA, 4, 1);

    /* Hardware reset pulse */
    RST_LOW();  delay_ms(5);
    RST_HIGH(); delay_ms(50);

    /* Soft reset */
    rc_write(MFRC522_REG_COMMAND, MFRC522_CMD_SOFT_RESET);
    delay_ms(50);

    /* Timer: ~24 ms */
    rc_write(MFRC522_REG_T_MODE,      0x8D);
    rc_write(MFRC522_REG_T_PRESCALER, 0x3E);
    rc_write(MFRC522_REG_T_RELOAD_L,  30);
    rc_write(MFRC522_REG_T_RELOAD_H,  0);

    rc_write(MFRC522_REG_TX_AUTO, 0x40);   /* 100% ASK */
    rc_write(MFRC522_REG_MODE,    0x3D);   /* CRC preset 0x6363 */

    rc_antenna_on();

    uint8_t v = rc_read(MFRC522_REG_VERSION);
    DBG("RC522 Version: 0x%02X", v);
    if (v == 0x00 || v == 0xFF)
        DBGE("RC522 not responding. Check wiring / 3.3V / RST.");
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
        n         = rc_read(MFRC522_REG_FIFO_LEVEL);
        last_bits = rc_read(MFRC522_REG_CONTROL) & 0x07;
        if (last_bits) *back_len = (n - 1) * 8 + last_bits;
        else           *back_len = n * 8;
        if (n == 0)  n = 1;
        if (n > 16)  n = 16;
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
    if (chk != serial[4]) { DBGE("UID BCC mismatch"); return MI_ERR; }
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
