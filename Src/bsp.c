#include "bsp.h"
#include "debug.h"

/* ============================================================================
 *  SysTick - 1 ms tick
 * ========================================================================== */
static volatile uint32_t s_ticks_ms = 0;

void SysTick_Handler(void) { s_ticks_ms++; }

void systick_init(void) {
    /* After reset, SystemCoreClock = 16000000 (HSI). 1 tick per 1 ms. */
    SysTick_Config(SystemCoreClock / 1000);
}

uint32_t millis(void)            { return s_ticks_ms; }
void     delay_ms(uint32_t ms)   { uint32_t s = s_ticks_ms; while ((s_ticks_ms - s) < ms) { __NOP(); } }

/* ============================================================================
 *  GPIO register helpers
 * ========================================================================== */
void gpio_clk_enable(GPIO_TypeDef *port) {
    if      (port == GPIOA) RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    else if (port == GPIOB) RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;
    else if (port == GPIOC) RCC->AHB1ENR |= RCC_AHB1ENR_GPIOCEN;
    else if (port == GPIOD) RCC->AHB1ENR |= RCC_AHB1ENR_GPIODEN;
    else if (port == GPIOE) RCC->AHB1ENR |= RCC_AHB1ENR_GPIOEEN;
    (void)RCC->AHB1ENR;      /* dummy read to ensure clock is up */
}

void gpio_mode(GPIO_TypeDef *port, uint32_t pin, uint32_t mode) {
    port->MODER &= ~(0x3U << (pin * 2));
    port->MODER |=  (mode  << (pin * 2));
}

void gpio_pull(GPIO_TypeDef *port, uint32_t pin, uint32_t pull) {
    port->PUPDR &= ~(0x3U << (pin * 2));
    port->PUPDR |=  (pull  << (pin * 2));
}

void gpio_otype(GPIO_TypeDef *port, uint32_t pin, uint32_t otype) {
    port->OTYPER &= ~(0x1U << pin);
    port->OTYPER |=  (otype << pin);
}

void gpio_speed(GPIO_TypeDef *port, uint32_t pin, uint32_t speed) {
    port->OSPEEDR &= ~(0x3U << (pin * 2));
    port->OSPEEDR |=  (speed << (pin * 2));
}

void gpio_af(GPIO_TypeDef *port, uint32_t pin, uint32_t af) {
    uint32_t idx   = pin >> 3;              /* 0 for pin 0-7, 1 for 8-15 */
    uint32_t shift = (pin & 7) * 4;
    port->AFR[idx] &= ~(0xFU << shift);
    port->AFR[idx] |=  (af   << shift);
}

void gpio_write(GPIO_TypeDef *port, uint32_t pin, uint32_t val) {
    port->BSRR = val ? (1U << pin) : (1U << (pin + 16));
}

uint32_t gpio_read(GPIO_TypeDef *port, uint32_t pin) {
    return (port->IDR >> pin) & 1U;
}

/* ============================================================================
 *  SPI1 - master, 8-bit, CPOL=0, CPHA=0, software NSS
 *  Pins: PA5=SCK, PA6=MISO, PA7=MOSI (AF5)
 *  Clock: APB2 = 16 MHz at default HSI -> prescaler /8 -> 2 MHz (safe for RC522)
 * ========================================================================== */
void spi1_init(void) {
    RCC->APB2ENR |= RCC_APB2ENR_SPI1EN;
    (void)RCC->APB2ENR;

    /* BR[5:3] = 010 -> fPCLK/8 (2 MHz) */
    SPI1->CR1 = 0;
    SPI1->CR1 = SPI_CR1_MSTR | SPI_CR1_SSI | SPI_CR1_SSM | (2U << 3);
    SPI1->CR2 = 0;
    SPI1->CR1 |= SPI_CR1_SPE;
}

uint8_t spi1_txrx(uint8_t tx) {
    /* Wait TXE, write, wait RXNE, read */
    while (!(SPI1->SR & SPI_SR_TXE));
    *(volatile uint8_t *)&SPI1->DR = tx;
    while (!(SPI1->SR & SPI_SR_RXNE));
    return *(volatile uint8_t *)&SPI1->DR;
}

/* ============================================================================
 *  I2C1 - 100 kHz standard mode
 *  Pins: PB8=SCL, PB9=SDA (AF4), open-drain + pull-up
 *  Clock: APB1 = 16 MHz at default HSI
 *    FREQ  = 16
 *    CCR   = 80    (100 kHz)
 *    TRISE = 17
 * ========================================================================== */
void i2c1_init(void) {
    RCC->APB1ENR |= RCC_APB1ENR_I2C1EN;
    (void)RCC->APB1ENR;

    /* Software reset */
    I2C1->CR1 = I2C_CR1_SWRST;
    I2C1->CR1 = 0;

    I2C1->CR2   = 16;           /* APB1 freq in MHz */
    I2C1->CCR   = 80;           /* Std-mode 100 kHz */
    I2C1->TRISE = 17;
    I2C1->CR1  |= I2C_CR1_PE;
}

/* Blocking I2C write. Returns 0 on success, negative error code on failure. */
int i2c1_write(uint8_t addr7, const uint8_t *data, uint32_t len) {
    uint32_t t;

    /* Wait for bus free */
    t = 100000;
    while ((I2C1->SR2 & I2C_SR2_BUSY) && --t);
    if (!t) return -1;

    /* START */
    I2C1->CR1 |= I2C_CR1_START;
    t = 100000;
    while (!(I2C1->SR1 & I2C_SR1_SB) && --t);
    if (!t) return -2;

    /* Send address + W */
    I2C1->DR = addr7 << 1;
    t = 100000;
    while (!(I2C1->SR1 & (I2C_SR1_ADDR | I2C_SR1_AF)) && --t);
    if (!t)                        return -3;
    if (I2C1->SR1 & I2C_SR1_AF) { I2C1->SR1 &= ~I2C_SR1_AF; I2C1->CR1 |= I2C_CR1_STOP; return -4; }
    (void)I2C1->SR1; (void)I2C1->SR2;    /* clear ADDR */

    /* Payload */
    for (uint32_t i = 0; i < len; i++) {
        t = 100000;
        while (!(I2C1->SR1 & I2C_SR1_TXE) && --t);
        if (!t) return -5;
        I2C1->DR = data[i];
    }

    t = 100000;
    while (!(I2C1->SR1 & I2C_SR1_BTF) && --t);
    I2C1->CR1 |= I2C_CR1_STOP;
    return 0;
}
