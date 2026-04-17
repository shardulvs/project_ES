#include "lcd_i2c.h"
#include "debug.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* HD44780 over PCF8574 (I2C backpack) in 4-bit mode.
 * Bit map (typical backpack): P0=RS, P1=RW, P2=EN, P3=Backlight, P4..P7=D4..D7.
 */
#define LCD_RS   0x01
#define LCD_RW   0x02
#define LCD_EN   0x04
#define LCD_BL   0x08

static I2C_HandleTypeDef *lcd_hi2c = NULL;
static uint8_t lcd_bl = LCD_BL;

static HAL_StatusTypeDef lcd_write_byte(uint8_t b) {
    return HAL_I2C_Master_Transmit(lcd_hi2c, LCD_I2C_ADDR, &b, 1, 50);
}

static void lcd_pulse_en(uint8_t data) {
    lcd_write_byte(data | LCD_EN | lcd_bl);
    HAL_Delay(1);
    lcd_write_byte((data & ~LCD_EN) | lcd_bl);
    HAL_Delay(1);
}

static void lcd_send_4bits(uint8_t nibble) {
    lcd_pulse_en(nibble & 0xF0);
}

static void lcd_send(uint8_t value, uint8_t mode) {
    uint8_t hi = value & 0xF0;
    uint8_t lo = (value << 4) & 0xF0;
    lcd_pulse_en(hi | mode | lcd_bl);
    lcd_pulse_en(lo | mode | lcd_bl);
}

static void lcd_cmd(uint8_t c)  { lcd_send(c, 0); }
static void lcd_data(uint8_t c) { lcd_send(c, LCD_RS); }

void LCD_Init(I2C_HandleTypeDef *hi2c) {
    lcd_hi2c = hi2c;

    /* Probe to help debug wrong address */
    if (HAL_I2C_IsDeviceReady(lcd_hi2c, LCD_I2C_ADDR, 3, 100) != HAL_OK) {
        DBGE("LCD not found at 0x%02X. Try 0x3F.", LCD_I2C_ADDR >> 1);
    } else {
        DBG("LCD found at 0x%02X", LCD_I2C_ADDR >> 1);
    }

    HAL_Delay(50);                 /* power-up wait */
    lcd_write_byte(lcd_bl);
    HAL_Delay(10);

    /* 4-bit init sequence per HD44780 datasheet */
    lcd_send_4bits(0x30); HAL_Delay(5);
    lcd_send_4bits(0x30); HAL_Delay(1);
    lcd_send_4bits(0x30); HAL_Delay(1);
    lcd_send_4bits(0x20); HAL_Delay(1);      /* set 4-bit */

    lcd_cmd(0x28);                  /* 4-bit, 2 lines, 5x8 font */
    lcd_cmd(0x0C);                  /* display on, cursor off */
    lcd_cmd(0x06);                  /* entry mode: increment */
    lcd_cmd(0x01); HAL_Delay(2);    /* clear */

    DBG("LCD init done");
}

void LCD_Clear(void)  { lcd_cmd(0x01); HAL_Delay(2); }
void LCD_Home(void)   { lcd_cmd(0x02); HAL_Delay(2); }

void LCD_SetCursor(uint8_t col, uint8_t row) {
    static const uint8_t offsets[] = {0x00, 0x40, 0x14, 0x54};
    if (row >= LCD_ROWS) row = LCD_ROWS - 1;
    lcd_cmd(0x80 | (col + offsets[row]));
}

void LCD_Print(const char *str) {
    while (*str) lcd_data((uint8_t)*str++);
}

void LCD_PrintAt(uint8_t col, uint8_t row, const char *str) {
    LCD_SetCursor(col, row);
    LCD_Print(str);
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
    lcd_write_byte(lcd_bl);
}
