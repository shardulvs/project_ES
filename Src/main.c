/* =============================================================================
 *  Secure Door Lock System - STM32F429I-DISC1  (single-file, no HAL)
 *
 *  Everything (drivers, storage, state machine) lives in this one file so it
 *  can be dropped straight into a Keil project with only the following RTE
 *  components ticked:
 *        CMSIS -> CORE
 *        Device -> Startup
 *  Define  STM32F429xx  in Project -> Options -> C/C++ -> Define.
 *  Tick    "Use MicroLIB" on the Target tab so printf stays small.
 *
 *  PIN MAP
 *    RC522  RST=PA3, SS=PA4, SCK=PA5, MISO=PA6, MOSI=PA7 (SPI1 AF5)
 *    Keypad rows=PB0, PB1, PB2, PB10   cols=PB12..PB15 (cols pulled up)
 *    LCD    I2C1 SDA=PB9, SCL=PB8 (AF4)
 *    Buzzer PC6   Green LED PC0   Red LED PD0   Tamper button PA0 to GND
 *
 *  Clock: default HSI = 16 MHz (no PLL is configured here).
 *  Debug: printf -> ITM SWO. In Keil: Debug Settings -> Trace -> Enable,
 *         Core Clock = 16 MHz, ITM Port 0, then open Debug (printf) Viewer.
 * =========================================================================== */

#include "stm32f4xx.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>

/* =============================================================================
 *  Forward declarations + tiny helpers
 * =========================================================================== */
static volatile uint32_t s_ticks_ms = 0;

void SysTick_Handler(void) { s_ticks_ms++; }

static uint32_t millis(void)             { return s_ticks_ms; }
static void     delay_ms(uint32_t ms)    { uint32_t s = s_ticks_ms; while ((s_ticks_ms - s) < ms) { __NOP(); } }

static void systick_init(void) {
    /* SystemCoreClock is 16 000 000 on HSI.  1 tick per ms. */
    SysTick_Config(SystemCoreClock / 1000);
}

/* ---- GPIO helpers (register-level) ----
 * mode : 0=input, 1=output, 2=alternate-function, 3=analog
 * pull : 0=none,  1=pull-up, 2=pull-down
 * otype: 0=push-pull, 1=open-drain
 */
static void gpio_clk_enable(GPIO_TypeDef *p) {
    if      (p == GPIOA) RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    else if (p == GPIOB) RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;
    else if (p == GPIOC) RCC->AHB1ENR |= RCC_AHB1ENR_GPIOCEN;
    else if (p == GPIOD) RCC->AHB1ENR |= RCC_AHB1ENR_GPIODEN;
    else if (p == GPIOE) RCC->AHB1ENR |= RCC_AHB1ENR_GPIOEEN;
    (void)RCC->AHB1ENR;
}
static void gpio_mode (GPIO_TypeDef *p, uint32_t pin, uint32_t m){ p->MODER   &= ~(3U<<(pin*2)); p->MODER   |= (m<<(pin*2)); }
static void gpio_pull (GPIO_TypeDef *p, uint32_t pin, uint32_t m){ p->PUPDR   &= ~(3U<<(pin*2)); p->PUPDR   |= (m<<(pin*2)); }
static void gpio_otype(GPIO_TypeDef *p, uint32_t pin, uint32_t m){ p->OTYPER  &= ~(1U<<pin);     p->OTYPER  |= (m<<pin);     }
static void gpio_speed(GPIO_TypeDef *p, uint32_t pin, uint32_t m){ p->OSPEEDR &= ~(3U<<(pin*2)); p->OSPEEDR |= (m<<(pin*2)); }
static void gpio_af   (GPIO_TypeDef *p, uint32_t pin, uint32_t af){
    uint32_t i=pin>>3, s=(pin&7)*4;
    p->AFR[i] &= ~(0xFU<<s);
    p->AFR[i] |=  (af<<s);
}
static void gpio_write(GPIO_TypeDef *p, uint32_t pin, uint32_t v){ p->BSRR = v ? (1U<<pin) : (1U<<(pin+16)); }
static uint32_t gpio_read(GPIO_TypeDef *p, uint32_t pin)         { return (p->IDR>>pin)&1U; }

/* =============================================================================
 *  Debug: printf -> ITM SWO
 * =========================================================================== */
int fputc(int ch, FILE *f) { (void)f; ITM_SendChar((uint32_t)ch); return ch; }

#define DBG(fmt, ...)  printf("[%lu] "      fmt "\r\n", (unsigned long)millis(), ##__VA_ARGS__)
#define DBGE(fmt, ...) printf("[%lu][ERR] " fmt "\r\n", (unsigned long)millis(), ##__VA_ARGS__)

/* =============================================================================
 *  SPI1 bring-up + single-byte transfer  (PA5/PA6/PA7 AF5)
 * =========================================================================== */
static void spi1_init(void) {
    RCC->APB2ENR |= RCC_APB2ENR_SPI1EN;
    (void)RCC->APB2ENR;

    /* BR[5:3] = 010 -> fPCLK/8 = 2 MHz (safe for RC522) */
    SPI1->CR1 = 0;
    SPI1->CR1 = SPI_CR1_MSTR | SPI_CR1_SSI | SPI_CR1_SSM | (2U << 3);
    SPI1->CR2 = 0;
    SPI1->CR1 |= SPI_CR1_SPE;
}
static uint8_t spi1_txrx(uint8_t tx) {
    while (!(SPI1->SR & SPI_SR_TXE));
    *(volatile uint8_t *)&SPI1->DR = tx;
    while (!(SPI1->SR & SPI_SR_RXNE));
    return *(volatile uint8_t *)&SPI1->DR;
}

/* =============================================================================
 *  I2C1 bring-up + blocking write  (PB8 SCL, PB9 SDA, AF4)
 * =========================================================================== */
static void i2c1_init(void) {
    RCC->APB1ENR |= RCC_APB1ENR_I2C1EN;
    (void)RCC->APB1ENR;

    I2C1->CR1   = I2C_CR1_SWRST;
    I2C1->CR1   = 0;
    I2C1->CR2   = 16;      /* APB1 freq in MHz (HSI default) */
    I2C1->CCR   = 80;      /* 100 kHz standard mode */
    I2C1->TRISE = 17;
    I2C1->CR1  |= I2C_CR1_PE;
}

/* returns 0 on success, negative on error */
static int i2c1_write(uint8_t addr7, const uint8_t *data, uint32_t len) {
    uint32_t t = 100000;
    while ((I2C1->SR2 & I2C_SR2_BUSY) && --t); if (!t) return -1;

    I2C1->CR1 |= I2C_CR1_START;
    t = 100000; while (!(I2C1->SR1 & I2C_SR1_SB) && --t); if (!t) return -2;

    I2C1->DR = addr7 << 1;
    t = 100000; while (!(I2C1->SR1 & (I2C_SR1_ADDR | I2C_SR1_AF)) && --t);
    if (!t) return -3;
    if (I2C1->SR1 & I2C_SR1_AF) { I2C1->SR1 &= ~I2C_SR1_AF; I2C1->CR1 |= I2C_CR1_STOP; return -4; }
    (void)I2C1->SR1; (void)I2C1->SR2;

    for (uint32_t i = 0; i < len; i++) {
        t = 100000; while (!(I2C1->SR1 & I2C_SR1_TXE) && --t); if (!t) return -5;
        I2C1->DR = data[i];
    }
    t = 100000; while (!(I2C1->SR1 & I2C_SR1_BTF) && --t);
    I2C1->CR1 |= I2C_CR1_STOP;
    return 0;
}

/* =============================================================================
 *  LCD - HD44780 over PCF8574 I2C backpack (4-bit mode)
 *  Bit map on backpack: P0=RS P1=RW P2=EN P3=BL  P4..P7=D4..D7
 *  If screen stays blank, try LCD_I2C_ADDR = 0x3F.
 * =========================================================================== */
#define LCD_I2C_ADDR   0x27
#define LCD_COLS       16
#define LCD_ROWS       2
#define LCD_RS   0x01
#define LCD_RW   0x02
#define LCD_EN   0x04
#define LCD_BL   0x08

static uint8_t lcd_bl = LCD_BL;

static void lcd_raw(uint8_t b)                  { i2c1_write(LCD_I2C_ADDR, &b, 1); }
static void lcd_pulse(uint8_t data) {
    lcd_raw(data | LCD_EN | lcd_bl); delay_ms(1);
    lcd_raw((data & ~LCD_EN) | lcd_bl); delay_ms(1);
}
static void lcd_send(uint8_t v, uint8_t mode) {
    lcd_pulse((v & 0xF0)            | mode | lcd_bl);
    lcd_pulse(((v << 4) & 0xF0)     | mode | lcd_bl);
}
static void lcd_cmd (uint8_t c) { lcd_send(c, 0);      }
static void lcd_data(uint8_t c) { lcd_send(c, LCD_RS); }

static void LCD_Clear(void)                 { lcd_cmd(0x01); delay_ms(2); }
static void LCD_SetCursor(uint8_t c, uint8_t r) {
    static const uint8_t off[] = { 0x00, 0x40, 0x14, 0x54 };
    if (r >= LCD_ROWS) r = LCD_ROWS - 1;
    lcd_cmd(0x80 | (c + off[r]));
}
static void LCD_Print(const char *s) { while (*s) lcd_data((uint8_t)*s++); }
static void LCD_PrintAt(uint8_t c, uint8_t r, const char *s) { LCD_SetCursor(c, r); LCD_Print(s); }

static void LCD_Init(void) {
    uint8_t zero = 0;
    if (i2c1_write(LCD_I2C_ADDR, &zero, 1) != 0)
        DBGE("LCD not ACKing at 0x%02X. Try 0x3F.", LCD_I2C_ADDR);
    else
        DBG("LCD found at 0x%02X", LCD_I2C_ADDR);

    delay_ms(50);
    lcd_raw(lcd_bl); delay_ms(10);
    lcd_pulse(0x30); delay_ms(5);
    lcd_pulse(0x30); delay_ms(1);
    lcd_pulse(0x30); delay_ms(1);
    lcd_pulse(0x20); delay_ms(1);
    lcd_cmd(0x28);                    /* 4-bit, 2-line, 5x8 */
    lcd_cmd(0x0C);                    /* display on */
    lcd_cmd(0x06);                    /* entry: increment */
    lcd_cmd(0x01); delay_ms(2);       /* clear */
    DBG("LCD init done");
}

/* =============================================================================
 *  Keypad 4x4 scanner
 *  Rows PB0 PB1 PB2 PB10 (output)
 *  Cols PB12..PB15       (input, pull-up)
 * =========================================================================== */
static const uint8_t KP_ROW[4] = { 0, 1, 2, 10 };
static const uint8_t KP_COL[4] = { 12, 13, 14, 15 };
static const char KEY_MAP[4][4] = {
    {'1','2','3','A'},
    {'4','5','6','B'},
    {'7','8','9','C'},
    {'*','0','#','D'}
};

static void Keypad_Init(void) {
    gpio_clk_enable(GPIOB);
    for (int i = 0; i < 4; i++) {
        gpio_mode (GPIOB, KP_ROW[i], 1);
        gpio_otype(GPIOB, KP_ROW[i], 0);
        gpio_speed(GPIOB, KP_ROW[i], 1);
        gpio_write(GPIOB, KP_ROW[i], 1);
    }
    for (int i = 0; i < 4; i++) {
        gpio_mode(GPIOB, KP_COL[i], 0);
        gpio_pull(GPIOB, KP_COL[i], 1);
    }
    DBG("Keypad init done");
}

static char Keypad_Scan(void) {
    for (int r = 0; r < 4; r++) {
        for (int i = 0; i < 4; i++) gpio_write(GPIOB, KP_ROW[i], (i==r)?0:1);
        for (volatile int d = 0; d < 200; d++) __NOP();

        for (int c = 0; c < 4; c++) {
            if (gpio_read(GPIOB, KP_COL[c]) == 0) {
                delay_ms(20);
                if (gpio_read(GPIOB, KP_COL[c]) == 0) {
                    while (gpio_read(GPIOB, KP_COL[c]) == 0) { __NOP(); }
                    for (int i = 0; i < 4; i++) gpio_write(GPIOB, KP_ROW[i], 1);
                    DBG("Key: %c (r=%d c=%d)", KEY_MAP[r][c], r, c);
                    return KEY_MAP[r][c];
                }
            }
        }
    }
    return 0;
}

/* =============================================================================
 *  MFRC522 SPI driver (minimal: request, anticoll, halt, UID read)
 *  NSS PA4, RST PA3 (managed as plain GPIOs; SPI uses software NSS)
 * =========================================================================== */
#define MFRC522_REG_COMMAND        0x01
#define MFRC522_REG_COM_IEN        0x02
#define MFRC522_REG_COM_IRQ        0x04
#define MFRC522_REG_ERROR          0x06
#define MFRC522_REG_FIFO_DATA      0x09
#define MFRC522_REG_FIFO_LEVEL     0x0A
#define MFRC522_REG_CONTROL        0x0C
#define MFRC522_REG_BIT_FRAMING    0x0D
#define MFRC522_REG_MODE           0x11
#define MFRC522_REG_TX_CONTROL     0x14
#define MFRC522_REG_TX_AUTO        0x15
#define MFRC522_REG_T_MODE         0x2A
#define MFRC522_REG_T_PRESCALER    0x2B
#define MFRC522_REG_T_RELOAD_H     0x2C
#define MFRC522_REG_T_RELOAD_L     0x2D
#define MFRC522_REG_VERSION        0x37

#define MFRC522_CMD_IDLE           0x00
#define MFRC522_CMD_TRANSCEIVE     0x0C
#define MFRC522_CMD_SOFT_RESET     0x0F

#define PICC_REQIDL                0x26
#define PICC_ANTICOLL              0x93
#define PICC_HALT                  0x50

#define MI_OK       0
#define MI_NOTAGERR 1
#define MI_ERR      2
#define RC522_UID_LEN 5

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
    uint8_t v = spi1_txrx(0);
    SS_HIGH();
    return v;
}
static void rc_set_bit(uint8_t reg, uint8_t m) { rc_write(reg, rc_read(reg) |  m); }
static void rc_clr_bit(uint8_t reg, uint8_t m) { rc_write(reg, rc_read(reg) & ~m); }

static void rc_antenna_on(void) {
    if (!(rc_read(MFRC522_REG_TX_CONTROL) & 0x03))
        rc_set_bit(MFRC522_REG_TX_CONTROL, 0x03);
}

static void RC522_Init(void) {
    gpio_clk_enable(GPIOA);
    /* RST PA3 and NSS PA4 as push-pull output, idle HIGH */
    gpio_mode (GPIOA, 3, 1); gpio_otype(GPIOA, 3, 0); gpio_speed(GPIOA, 3, 2); gpio_write(GPIOA, 3, 1);
    gpio_mode (GPIOA, 4, 1); gpio_otype(GPIOA, 4, 0); gpio_speed(GPIOA, 4, 2); gpio_write(GPIOA, 4, 1);

    RST_LOW();  delay_ms(5);
    RST_HIGH(); delay_ms(50);
    rc_write(MFRC522_REG_COMMAND, MFRC522_CMD_SOFT_RESET);
    delay_ms(50);

    rc_write(MFRC522_REG_T_MODE,      0x8D);
    rc_write(MFRC522_REG_T_PRESCALER, 0x3E);
    rc_write(MFRC522_REG_T_RELOAD_L,  30);
    rc_write(MFRC522_REG_T_RELOAD_H,  0);
    rc_write(MFRC522_REG_TX_AUTO, 0x40);
    rc_write(MFRC522_REG_MODE,    0x3D);
    rc_antenna_on();

    uint8_t v = rc_read(MFRC522_REG_VERSION);
    DBG("RC522 Version: 0x%02X", v);
    if (v == 0x00 || v == 0xFF) DBGE("RC522 not responding. Check SPI wiring / 3V3 / RST.");
}

static uint8_t rc_to_card(uint8_t cmd, uint8_t *send, uint8_t send_len,
                          uint8_t *back, uint16_t *back_len) {
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
    do { n = rc_read(MFRC522_REG_COM_IRQ); i--; }
    while (i && !(n & 0x01) && !(n & wait_irq));

    rc_clr_bit(MFRC522_REG_BIT_FRAMING, 0x80);

    if (i == 0) return MI_ERR;
    if (rc_read(MFRC522_REG_ERROR) & 0x1B) return MI_ERR;

    uint8_t status = MI_OK;
    if (n & irq_en & 0x01) status = MI_NOTAGERR;

    if (cmd == MFRC522_CMD_TRANSCEIVE) {
        n         = rc_read(MFRC522_REG_FIFO_LEVEL);
        last_bits = rc_read(MFRC522_REG_CONTROL) & 0x07;
        *back_len = last_bits ? (n - 1) * 8 + last_bits : n * 8;
        if (n == 0) n = 1;
        if (n > 16) n = 16;
        for (i = 0; i < n; i++) back[i] = rc_read(MFRC522_REG_FIFO_DATA);
    }
    return status;
}

static uint8_t RC522_Request(uint8_t req_mode, uint8_t *tag_type) {
    uint16_t back_bits = 0;
    rc_write(MFRC522_REG_BIT_FRAMING, 0x07);
    uint8_t data = req_mode;
    uint8_t s = rc_to_card(MFRC522_CMD_TRANSCEIVE, &data, 1, tag_type, &back_bits);
    if (s != MI_OK || back_bits != 0x10) return MI_ERR;
    return MI_OK;
}
static uint8_t RC522_Anticoll(uint8_t *serial) {
    uint8_t data[2] = { PICC_ANTICOLL, 0x20 };
    uint16_t back_bits = 0;
    rc_write(MFRC522_REG_BIT_FRAMING, 0x00);
    uint8_t s = rc_to_card(MFRC522_CMD_TRANSCEIVE, data, 2, serial, &back_bits);
    if (s != MI_OK) return s;
    uint8_t chk = 0;
    for (int i = 0; i < 4; i++) chk ^= serial[i];
    if (chk != serial[4]) { DBGE("UID BCC mismatch"); return MI_ERR; }
    return MI_OK;
}
static void RC522_Halt(void) {
    uint8_t buf[2] = { PICC_HALT, 0 };
    uint8_t back[8];
    uint16_t bl = 0;
    rc_to_card(MFRC522_CMD_TRANSCEIVE, buf, 2, back, &bl);
}
static uint8_t RC522_ReadUID(uint8_t *uid) {
    uint8_t tag_type[2];
    if (RC522_Request(PICC_REQIDL, tag_type) != MI_OK) return 0;
    if (RC522_Anticoll(uid) != MI_OK) return 0;
    RC522_Halt();
    return 1;
}

/* =============================================================================
 *  Storage (in RAM; volatile across resets)
 * =========================================================================== */
#define MAX_CARDS 8
#define PIN_LEN   4
#define UID_LEN   4

typedef struct {
    uint8_t  uid[UID_LEN];
    uint8_t  valid;
    uint8_t  blacklisted;
    char     pin[PIN_LEN + 1];
} CardEntry;

typedef struct {
    uint32_t   magic;
    char       master_pin[PIN_LEN + 1];
    char       duress_pin[PIN_LEN + 1];
    CardEntry  cards[MAX_CARDS];
    uint8_t    rfid_only;   /* 0=dual, 1=rfid only, 2=pin only */
} Config;

static Config g_cfg;

static void Storage_ResetToDefaults(void) {
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.magic = 0xDEADBEEF;
    strcpy(g_cfg.master_pin, "9999");
    strcpy(g_cfg.duress_pin, "0000");
    g_cfg.rfid_only = 0;

    /* Demo card - replace with your own real UID via the admin enroll flow */
    g_cfg.cards[0].valid = 1;
    g_cfg.cards[0].uid[0] = 0xDE;
    g_cfg.cards[0].uid[1] = 0xAD;
    g_cfg.cards[0].uid[2] = 0xBE;
    g_cfg.cards[0].uid[3] = 0xEF;
    strcpy(g_cfg.cards[0].pin, "1234");

    DBG("Storage reset. Master=%s Duress=%s", g_cfg.master_pin, g_cfg.duress_pin);
}
static void Storage_Init(void)  { if (g_cfg.magic != 0xDEADBEEF) Storage_ResetToDefaults(); }

static int Storage_FindCard(const uint8_t *uid) {
    for (int i = 0; i < MAX_CARDS; i++) {
        if (!g_cfg.cards[i].valid) continue;
        if (memcmp(g_cfg.cards[i].uid, uid, UID_LEN) == 0) return i;
    }
    return -1;
}
static int Storage_AddCard(const uint8_t *uid) {
    if (Storage_FindCard(uid) >= 0) { DBG("Card already enrolled"); return -1; }
    for (int i = 0; i < MAX_CARDS; i++) {
        if (!g_cfg.cards[i].valid) {
            g_cfg.cards[i].valid = 1;
            g_cfg.cards[i].blacklisted = 0;
            memcpy(g_cfg.cards[i].uid, uid, UID_LEN);
            strcpy(g_cfg.cards[i].pin, "1234");
            DBG("Enrolled card at slot %d", i);
            return i;
        }
    }
    DBGE("Card table full");
    return -1;
}
static int Storage_DeleteCard(const uint8_t *uid) {
    int i = Storage_FindCard(uid);
    if (i < 0) return 0;
    memset(&g_cfg.cards[i], 0, sizeof(CardEntry));
    DBG("Deleted slot %d", i);
    return 1;
}

/* =============================================================================
 *  Application - state machine, access flow, admin menu
 * =========================================================================== */
#define PIN_BUZZER 6   /* GPIOC */
#define PIN_GREEN  0   /* GPIOC */
#define PIN_RED    0   /* GPIOD */
#define PIN_TAMPER 0   /* GPIOA */

#define MAX_FAILS       3
#define LOCKOUT_MS      30000UL
#define UNLOCK_MS       5000UL
#define PIN_TIMEOUT_MS  10000UL

typedef enum { ST_IDLE, ST_GRANTED, ST_LOCKOUT, ST_ADMIN, ST_ALARM } State;

static State   g_state = ST_IDLE;
static uint8_t g_fail_count = 0;
static uint32_t g_lockout_until = 0;
static volatile uint8_t g_tamper_flag = 0;

static void led_green(uint8_t on) { gpio_write(GPIOC, PIN_GREEN,  on); }
static void led_red  (uint8_t on) { gpio_write(GPIOD, PIN_RED,    on); }
static void buzzer   (uint8_t on) { gpio_write(GPIOC, PIN_BUZZER, on); }

static void beep_short  (void) { buzzer(1); delay_ms(40);  buzzer(0); }
static void beep_granted(void) { buzzer(1); delay_ms(80); buzzer(0); delay_ms(60); buzzer(1); delay_ms(80); buzzer(0); }
static void beep_denied (void) { buzzer(1); delay_ms(400); buzzer(0); }
static void beep_lockout(void) { for (int i=0;i<6;i++){buzzer(1);delay_ms(80);buzzer(0);delay_ms(80);} }

/* EXTI0 = tamper button */
void EXTI0_IRQHandler(void) {
    if (EXTI->PR & (1U << 0)) {
        EXTI->PR = (1U << 0);
        g_tamper_flag = 1;
    }
}
static void tamper_init(void) {
    gpio_clk_enable(GPIOA);
    gpio_mode(GPIOA, PIN_TAMPER, 0);
    gpio_pull(GPIOA, PIN_TAMPER, 1);

    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;
    SYSCFG->EXTICR[0] &= ~SYSCFG_EXTICR1_EXTI0;     /* 0000 = PA */

    EXTI->IMR  |= (1U << 0);
    EXTI->FTSR |= (1U << 0);
    EXTI->RTSR &= ~(1U << 0);

    NVIC_SetPriority(EXTI0_IRQn, 2);
    NVIC_EnableIRQ(EXTI0_IRQn);
}

static void misc_gpio_init(void) {
    gpio_clk_enable(GPIOC);
    gpio_clk_enable(GPIOD);
    gpio_mode(GPIOC, PIN_BUZZER, 1); gpio_otype(GPIOC, PIN_BUZZER, 0); gpio_write(GPIOC, PIN_BUZZER, 0);
    gpio_mode(GPIOC, PIN_GREEN,  1); gpio_otype(GPIOC, PIN_GREEN,  0); gpio_write(GPIOC, PIN_GREEN,  0);
    gpio_mode(GPIOD, PIN_RED,    1); gpio_otype(GPIOD, PIN_RED,    0); gpio_write(GPIOD, PIN_RED,    0);
}

static void spi_pins_init(void) {
    gpio_clk_enable(GPIOA);
    for (int pin = 5; pin <= 7; pin++) {
        gpio_mode (GPIOA, pin, 2);
        gpio_af   (GPIOA, pin, 5);
        gpio_otype(GPIOA, pin, 0);
        gpio_pull (GPIOA, pin, 0);
        gpio_speed(GPIOA, pin, 2);
    }
}
static void i2c_pins_init(void) {
    gpio_clk_enable(GPIOB);
    int pins[2] = { 8, 9 };
    for (int i = 0; i < 2; i++) {
        gpio_mode (GPIOB, pins[i], 2);
        gpio_af   (GPIOB, pins[i], 4);
        gpio_otype(GPIOB, pins[i], 1);
        gpio_pull (GPIOB, pins[i], 1);
        gpio_speed(GPIOB, pins[i], 1);
    }
}

static void show_idle(void) {
    LCD_Clear();
    LCD_PrintAt(0, 0, "Secure Lock");
    LCD_PrintAt(0, 1, "Scan card...");
    led_green(0);
    led_red(1);
}

static void uid_to_str(const uint8_t *u, char *out) {
    sprintf(out, "%02X%02X%02X%02X", u[0], u[1], u[2], u[3]);
}

static uint8_t prompt_pin(const char *title, char out[PIN_LEN + 1]) {
    char buf[PIN_LEN + 1] = {0};
    uint8_t n = 0;

    LCD_Clear();
    LCD_PrintAt(0, 0, title);
    LCD_PrintAt(0, 1, "PIN:");

    uint32_t start = millis();
    while ((millis() - start) < PIN_TIMEOUT_MS) {
        char k = Keypad_Scan();
        if (!k) continue;
        beep_short();
        start = millis();

        if (k == 'D') { DBG("PIN cancelled"); return 0; }
        if (k == '*') {
            if (n > 0) { n--; buf[n] = 0; LCD_PrintAt(4 + n, 1, " "); }
            continue;
        }
        if (k == '#') {
            if (n == PIN_LEN) { strcpy(out, buf); return 1; }
            continue;
        }
        if (k >= '0' && k <= '9' && n < PIN_LEN) {
            buf[n++] = k;
            LCD_PrintAt(4 + (n - 1), 1, "*");
        }
    }
    DBG("PIN entry timed out");
    return 0;
}

static void admin_enroll(void) {
    LCD_Clear();
    LCD_PrintAt(0, 0, "Enroll: scan");
    LCD_PrintAt(0, 1, "card now...");
    uint8_t uid[RC522_UID_LEN];
    uint32_t start = millis();
    while ((millis() - start) < 10000) {
        led_green((millis()/200) & 1);
        if (RC522_ReadUID(uid)) {
            led_green(1);
            char s[12]; uid_to_str(uid, s);
            DBG("Enroll UID=%s", s);
            int idx = Storage_AddCard(uid);
            LCD_Clear();
            LCD_PrintAt(0, 0, idx >= 0 ? "Enrolled!" : "Failed/Full");
            LCD_PrintAt(0, 1, s);
            beep_granted();
            delay_ms(1500);
            return;
        }
        delay_ms(50);
    }
    LCD_Clear(); LCD_PrintAt(0, 0, "Enroll timeout"); delay_ms(1200);
}

static void admin_delete(void) {
    LCD_Clear();
    LCD_PrintAt(0, 0, "Delete: scan");
    LCD_PrintAt(0, 1, "card to del");
    uint8_t uid[RC522_UID_LEN];
    uint32_t start = millis();
    while ((millis() - start) < 10000) {
        if (RC522_ReadUID(uid)) {
            int ok = Storage_DeleteCard(uid);
            LCD_Clear();
            LCD_PrintAt(0, 0, ok ? "Deleted" : "Not found");
            beep_short();
            delay_ms(1200);
            return;
        }
        delay_ms(50);
    }
    LCD_Clear(); LCD_PrintAt(0, 0, "Delete timeout"); delay_ms(1200);
}

static void admin_change_pin(void) {
    char newpin[PIN_LEN + 1], confirm[PIN_LEN + 1];
    if (!prompt_pin("New master PIN", newpin))  return;
    if (!prompt_pin("Confirm PIN",    confirm)) return;
    if (strcmp(newpin, confirm) == 0) {
        strcpy(g_cfg.master_pin, newpin);
        LCD_Clear(); LCD_PrintAt(0, 0, "PIN updated"); beep_granted();
        DBG("Master PIN changed");
    } else {
        LCD_Clear(); LCD_PrintAt(0, 0, "Mismatch"); beep_denied();
    }
    delay_ms(1200);
}

static void admin_menu(void) {
    DBG("Entering admin menu");
    g_state = ST_ADMIN;
    uint32_t start = millis();

    while ((millis() - start) < 20000) {
        uint8_t b = (millis() / 300) & 1;
        led_green(b); led_red(!b);

        LCD_PrintAt(0, 0, "ADMIN MENU     ");
        LCD_PrintAt(0, 1, "A:en B:del C:pin");

        char k = Keypad_Scan();
        if (!k) continue;
        start = millis();
        beep_short();

        if      (k == 'A') admin_enroll();
        else if (k == 'B') admin_delete();
        else if (k == 'C') admin_change_pin();
        else if (k == 'D') break;
    }
    led_green(0); led_red(0);
    g_state = ST_IDLE;
    show_idle();
}

static void grant_access(int slot) {
    g_state = ST_GRANTED;
    g_fail_count = 0;

    LCD_Clear();
    LCD_PrintAt(0, 0, "ACCESS GRANTED");
    if (slot >= 0) { char s[12]; uid_to_str(g_cfg.cards[slot].uid, s); LCD_PrintAt(0, 1, s); }
    else             LCD_PrintAt(0, 1, "Master PIN");

    led_red(0); led_green(1);
    beep_granted();
    DBG("ACCESS GRANTED (slot=%d)", slot);

    uint32_t start = millis();
    while ((millis() - start) < UNLOCK_MS) {
        uint32_t left = (UNLOCK_MS - (millis() - start)) / 1000 + 1;
        char buf[17]; snprintf(buf, sizeof(buf), "Unlocked %lus  ", (unsigned long)left);
        LCD_PrintAt(0, 1, buf);
        delay_ms(100);
    }
    DBG("Auto-relock");
    g_state = ST_IDLE;
    show_idle();
}

static void deny_access(const char *reason) {
    g_fail_count++;
    DBG("ACCESS DENIED: %s (fails=%u)", reason, g_fail_count);

    LCD_Clear();
    LCD_PrintAt(0, 0, "ACCESS DENIED");
    LCD_PrintAt(0, 1, reason);

    for (int i = 0; i < 3; i++) { led_red(0); delay_ms(100); led_red(1); delay_ms(100); }
    beep_denied();
    delay_ms(800);

    if (g_fail_count >= MAX_FAILS) {
        g_state = ST_LOCKOUT;
        g_lockout_until = millis() + LOCKOUT_MS;
        beep_lockout();
    } else {
        g_state = ST_IDLE;
        show_idle();
    }
}

static void do_lockout(void) {
    DBG("Lockout active");
    while (millis() < g_lockout_until) {
        uint32_t left = (g_lockout_until - millis()) / 1000 + 1;
        char buf[17]; snprintf(buf, sizeof(buf), "LOCKED %lus     ", (unsigned long)left);
        LCD_PrintAt(0, 0, "TOO MANY FAILS ");
        LCD_PrintAt(0, 1, buf);
        led_red((millis()/150) & 1);
        delay_ms(50);
    }
    g_fail_count = 0;
    g_state = ST_IDLE;
    DBG("Lockout cleared");
    show_idle();
}

static void trigger_alarm(const char *reason, uint8_t silent) {
    DBGE("ALARM: %s silent=%u", reason, silent);
    g_state = ST_ALARM;
    LCD_Clear();
    LCD_PrintAt(0, 0, silent ? "ACCESS GRANTED" : "!! TAMPER !!");
    LCD_PrintAt(0, 1, silent ? "Unlocked 5s   " : reason);

    if (silent) {
        led_green(1); led_red(0);
        delay_ms(5000);
        led_green(0);
    } else {
        uint32_t start = millis();
        while ((millis() - start) < 8000) {
            led_red ((millis()/100) & 1);
            buzzer  ((millis()/100) & 1);
            delay_ms(20);
        }
        buzzer(0);
    }
    g_tamper_flag = 0;
    g_state = ST_IDLE;
    show_idle();
}

/* Detect '#' hold-down (row 4 driven low, col 3 read) */
static uint8_t hash_held(void) {
    gpio_write(GPIOB, 0,  1);
    gpio_write(GPIOB, 1,  1);
    gpio_write(GPIOB, 2,  1);
    gpio_write(GPIOB, 10, 0);
    for (volatile int d = 0; d < 500; d++) __NOP();
    uint8_t pressed = (gpio_read(GPIOB, 14) == 0);
    gpio_write(GPIOB, 10, 1);
    return pressed;
}

/* =============================================================================
 *  main
 * =========================================================================== */
int main(void) {
    SystemCoreClockUpdate();
    systick_init();

    misc_gpio_init();
    spi_pins_init();
    i2c_pins_init();
    tamper_init();

    spi1_init();
    i2c1_init();

    DBG("=== Secure Door Lock booting ===");
    DBG("SystemCoreClock = %lu Hz", (unsigned long)SystemCoreClock);

    LCD_Init();
    LCD_PrintAt(0, 0, "Boot...");
    Keypad_Init();
    Storage_Init();
    RC522_Init();

    show_idle();
    DBG("Ready. Max cards=%d rfid_only=%u", MAX_CARDS, g_cfg.rfid_only);

    uint32_t hash_hold_start = 0;

    for (;;) {

        if (g_tamper_flag)         { trigger_alarm("Button!", 0); continue; }
        if (g_state == ST_LOCKOUT) { do_lockout();                 continue; }

        /* Admin hotkey: hold '#' for 2 s */
        if (hash_held()) {
            if (!hash_hold_start) hash_hold_start = millis();
            else if ((millis() - hash_hold_start) > 2000) {
                hash_hold_start = 0;
                char pin[PIN_LEN + 1];
                if (prompt_pin("Admin PIN", pin) && strcmp(pin, g_cfg.master_pin) == 0) admin_menu();
                else                                                                    deny_access("Bad admin");
                continue;
            }
        } else hash_hold_start = 0;

        /* RFID scan */
        uint8_t uid[RC522_UID_LEN];
        if (RC522_ReadUID(uid)) {
            char s[12]; uid_to_str(uid, s);
            DBG("UID read: %s", s);

            int idx = Storage_FindCard(uid);
            if (idx < 0) {
                LCD_Clear();
                LCD_PrintAt(0, 0, "UNKNOWN CARD");
                LCD_PrintAt(0, 1, s);
                beep_denied();
                delay_ms(1000);
                deny_access("Unknown");
                continue;
            }
            if (g_cfg.cards[idx].blacklisted) {
                LCD_Clear(); LCD_PrintAt(0, 0, "BLACKLISTED");
                deny_access("Blacklist");
                continue;
            }

            if (g_cfg.rfid_only == 1) { grant_access(idx); continue; }

            LCD_Clear();
            LCD_PrintAt(0, 0, "Card OK");
            LCD_PrintAt(0, 1, s);
            delay_ms(700);

            char pin[PIN_LEN + 1];
            if (!prompt_pin("Enter PIN", pin)) { deny_access("PIN timeout"); continue; }

            if      (strcmp(pin, g_cfg.duress_pin) == 0)      trigger_alarm("duress", 1);
            else if (strcmp(pin, g_cfg.cards[idx].pin) == 0
                  || strcmp(pin, g_cfg.master_pin) == 0)      grant_access(idx);
            else                                               deny_access("Bad PIN");
            continue;
        }

        /* 'A' = master-PIN-only emergency entry */
        char k = Keypad_Scan();
        if (k == 'A' || g_cfg.rfid_only == 2) {
            char pin[PIN_LEN + 1];
            if (prompt_pin("Master PIN", pin)) {
                if      (strcmp(pin, g_cfg.duress_pin) == 0) trigger_alarm("duress", 1);
                else if (strcmp(pin, g_cfg.master_pin) == 0) grant_access(-1);
                else                                          deny_access("Bad master");
            }
            continue;
        }

        delay_ms(80);
    }
}
