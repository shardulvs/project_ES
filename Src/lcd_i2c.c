#include "lcd_i2c.h"
#include "bsp.h"
#include "debug.h"
#include <stdio.h>
#include <stdarg.h>

/* HD44780 driven by a PCF8574 I2C expander, 4-bit mode.
 * Typical bit map on PCF8574 backpack:
 *   P0=RS  P1=RW  P2=EN  P3=Backlight  P4..P7=D4..D7
 */
#define LCD_RS   0x01
#define LCD_RW   0x02
#define LCD_EN   0x04
#define LCD_BL   0x08

static uint8_t lcd_bl = LCD_BL;

static void lcd_raw(uint8_t b) {
    i2c1_write(LCD_I2C_ADDR, &b, 1);
}

static void lcd_pulse(uint8_t data) {
    lcd_raw(data | LCD_EN | lcd_bl);
    delay_ms(1);
    lcd_raw((data & ~LCD_EN) | lcd_bl);
    delay_ms(1);
}

static void lcd_send(uint8_t value, uint8_t mode) {
    uint8_t hi = value & 0xF0;
    uint8_t lo = (value << 4) & 0xF0;
    lcd_pulse(hi | mode | lcd_bl);
    lcd_pulse(lo | mode | lcd_bl);
}

static void lcd_cmd(uint8_t c)  { lcd_send(c, 0);      }
static void lcd_data(uint8_t c) { lcd_send(c, LCD_RS); }

void LCD_Init(void) {
    /* Send a single byte to probe I2C. If NACK we log but keep going. */
    uint8_t zero = 0;
    if (i2c1_write(LCD_I2C_ADDR, &zero, 1) != 0) {
        DBGE("LCD not ACKing at 0x%02X. Try 0x3F (edit Inc/lcd_i2c.h).", LCD_I2C_ADDR);
    } else {
        DBG("LCD found at 0x%02X", LCD_I2C_ADDR);
    }

    delay_ms(50);
    lcd_raw(lcd_bl);
    delay_ms(10);

    /* 4-bit init sequence (HD44780 datasheet) */
    lcd_pulse(0x30); delay_ms(5);
    lcd_pulse(0x30); delay_ms(1);
    lcd_pulse(0x30); delay_ms(1);
    lcd_pulse(0x20); delay_ms(1);    /* switch to 4-bit */

    lcd_cmd(0x28);                    /* 4-bit, 2-line, 5x8 */
    lcd_cmd(0x0C);                    /* display on, cursor off */
    lcd_cmd(0x06);                    /* entry mode: increment */
    lcd_cmd(0x01); delay_ms(2);       /* clear */

    DBG("LCD init done");
}

void LCD_Clear(void)  { lcd_cmd(0x01); delay_ms(2); }
void LCD_Home(void)   { lcd_cmd(0x02); delay_ms(2); }

void LCD_SetCursor(uint8_t col, uint8_t row) {
    static const uint8_t off[] = { 0x00, 0x40, 0x14, 0x54 };
    if (row >= LCD_ROWS) row = LCD_ROWS - 1;
    lcd_cmd(0x80 | (col + off[row]));
}

void LCD_Print(const char *s) {
    while (*s) lcd_data((uint8_t)*s++);
}

void LCD_PrintAt(uint8_t col, uint8_t row, const char *s) {
    LCD_SetCursor(col, row);
    LCD_Print(s);
}

void LCD_Printf(const char *fmt, ...) {
    char buf[32];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    LCD_Print(buf);
}

void LCD_Backlight(uint8_t on) {
    lcd_bl = on ? LCD_BL : 0;
    lcd_raw(lcd_bl);
}
