#ifndef LCD_I2C_H
#define LCD_I2C_H

#include <stdint.h>

/* PCF8574 I2C backpack address. 0x27 is typical; some modules use 0x3F. */
#define LCD_I2C_ADDR   0x27
#define LCD_COLS       16
#define LCD_ROWS       2

void LCD_Init(void);
void LCD_Clear(void);
void LCD_Home(void);
void LCD_SetCursor(uint8_t col, uint8_t row);
void LCD_Print(const char *str);
void LCD_PrintAt(uint8_t col, uint8_t row, const char *str);
void LCD_Printf(const char *fmt, ...);
void LCD_Backlight(uint8_t on);

#endif
