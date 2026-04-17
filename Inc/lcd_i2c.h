#ifndef LCD_I2C_H
#define LCD_I2C_H

#include "stm32f4xx_hal.h"

/* Default PCF8574 backpack address. Many modules are 0x27; some are 0x3F.
 * If screen stays blank, try 0x3F. Address is 7-bit, shifted left by 1 for HAL.
 */
#define LCD_I2C_ADDR   (0x27 << 1)
#define LCD_COLS       16
#define LCD_ROWS       2

void LCD_Init(I2C_HandleTypeDef *hi2c);
void LCD_Clear(void);
void LCD_Home(void);
void LCD_SetCursor(uint8_t col, uint8_t row);
void LCD_Print(const char *str);
void LCD_PrintAt(uint8_t col, uint8_t row, const char *str);
void LCD_Printf(const char *fmt, ...);
void LCD_Backlight(uint8_t on);

#endif
