#include "keypad.h"
#include "bsp.h"
#include "debug.h"

static const uint8_t ROW_PIN[4] = { 0, 1, 2, 10 };    /* PB0 PB1 PB2 PB10 */
static const uint8_t COL_PIN[4] = { 12, 13, 14, 15 };  /* PB12..PB15        */

static const char KEY_MAP[4][4] = {
    {'1','2','3','A'},
    {'4','5','6','B'},
    {'7','8','9','C'},
    {'*','0','#','D'}
};

void Keypad_Init(void) {
    gpio_clk_enable(GPIOB);

    /* Rows: push-pull output, idle HIGH */
    for (int i = 0; i < 4; i++) {
        gpio_mode (GPIOB, ROW_PIN[i], 1);   /* output */
        gpio_otype(GPIOB, ROW_PIN[i], 0);
        gpio_speed(GPIOB, ROW_PIN[i], 1);
        gpio_write(GPIOB, ROW_PIN[i], 1);
    }
    /* Columns: input with pull-up */
    for (int i = 0; i < 4; i++) {
        gpio_mode(GPIOB, COL_PIN[i], 0);    /* input */
        gpio_pull(GPIOB, COL_PIN[i], 1);    /* pull-up */
    }
    DBG("Keypad init done");
}

char Keypad_Scan(void) {
    for (int r = 0; r < 4; r++) {
        for (int i = 0; i < 4; i++)
            gpio_write(GPIOB, ROW_PIN[i], (i == r) ? 0 : 1);
        for (volatile int d = 0; d < 200; d++) __NOP();

        for (int c = 0; c < 4; c++) {
            if (gpio_read(GPIOB, COL_PIN[c]) == 0) {
                delay_ms(20);                                  /* debounce */
                if (gpio_read(GPIOB, COL_PIN[c]) == 0) {
                    while (gpio_read(GPIOB, COL_PIN[c]) == 0) { __NOP(); }  /* wait release */
                    for (int i = 0; i < 4; i++) gpio_write(GPIOB, ROW_PIN[i], 1);
                    DBG("Key: %c (r=%d c=%d)", KEY_MAP[r][c], r, c);
                    return KEY_MAP[r][c];
                }
            }
        }
    }
    return 0;
}

char Keypad_GetKey(uint32_t timeout_ms) {
    uint32_t start = millis();
    while ((millis() - start) < timeout_ms) {
        char k = Keypad_Scan();
        if (k) return k;
    }
    return 0;
}
