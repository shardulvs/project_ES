/* Board support / low-level helpers - no HAL, pure CMSIS register access.
 * Works on STM32F429I-DISC1 with default clock (HSI, 16 MHz).
 */
#ifndef BSP_H
#define BSP_H

#include "stm32f4xx.h"   /* CMSIS device header (comes with Keil DFP) */
#include <stdint.h>

/* ---------- System tick (1 ms) ---------- */
void     systick_init(void);
uint32_t millis(void);
void     delay_ms(uint32_t ms);

/* ---------- GPIO helpers ---------- */
/* mode: 0=input, 1=output, 2=alternate-function, 3=analog */
/* pull: 0=none,  1=pull-up, 2=pull-down                   */
/* otype: 0=push-pull, 1=open-drain                        */
void gpio_clk_enable(GPIO_TypeDef *port);
void gpio_mode (GPIO_TypeDef *port, uint32_t pin, uint32_t mode);
void gpio_pull (GPIO_TypeDef *port, uint32_t pin, uint32_t pull);
void gpio_otype(GPIO_TypeDef *port, uint32_t pin, uint32_t otype);
void gpio_speed(GPIO_TypeDef *port, uint32_t pin, uint32_t speed);
void gpio_af   (GPIO_TypeDef *port, uint32_t pin, uint32_t af);
void gpio_write(GPIO_TypeDef *port, uint32_t pin, uint32_t val);
uint32_t gpio_read(GPIO_TypeDef *port, uint32_t pin);

/* ---------- SPI1 (for RC522) ---------- */
void    spi1_init(void);
uint8_t spi1_txrx(uint8_t tx);

/* ---------- I2C1 (for LCD) ---------- */
void i2c1_init(void);
int  i2c1_write(uint8_t addr7, const uint8_t *data, uint32_t len);

#endif
