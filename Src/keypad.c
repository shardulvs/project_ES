#include "keypad.h"
#include "debug.h"

static GPIO_TypeDef * const ROW_PORT[4] = { GPIOB, GPIOB, GPIOB, GPIOB };
static const uint16_t       ROW_PIN[4]  = { GPIO_PIN_0, GPIO_PIN_1, GPIO_PIN_2, GPIO_PIN_10 };

static GPIO_TypeDef * const COL_PORT[4] = { GPIOB, GPIOB, GPIOB, GPIOB };
static const uint16_t       COL_PIN[4]  = { GPIO_PIN_12, GPIO_PIN_13, GPIO_PIN_14, GPIO_PIN_15 };

static const char KEY_MAP[4][4] = {
    {'1','2','3','A'},
    {'4','5','6','B'},
    {'7','8','9','C'},
    {'*','0','#','D'}
};

void Keypad_Init(void) {
    GPIO_InitTypeDef g = {0};
    __HAL_RCC_GPIOB_CLK_ENABLE();

    /* Rows = push-pull output, idle HIGH */
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    for (int i = 0; i < 4; i++) {
        g.Pin = ROW_PIN[i];
        HAL_GPIO_Init(ROW_PORT[i], &g);
        HAL_GPIO_WritePin(ROW_PORT[i], ROW_PIN[i], GPIO_PIN_SET);
    }

    /* Columns = input pull-up */
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_PULLUP;
    for (int i = 0; i < 4; i++) {
        g.Pin = COL_PIN[i];
        HAL_GPIO_Init(COL_PORT[i], &g);
    }

    DBG("Keypad init done");
}

char Keypad_Scan(void) {
    for (int r = 0; r < 4; r++) {
        /* Drive one row LOW, leave others HIGH */
        for (int i = 0; i < 4; i++)
            HAL_GPIO_WritePin(ROW_PORT[i], ROW_PIN[i], (i == r) ? GPIO_PIN_RESET : GPIO_PIN_SET);

        for (volatile int d = 0; d < 200; d++) __NOP();  /* settle */

        for (int c = 0; c < 4; c++) {
            if (HAL_GPIO_ReadPin(COL_PORT[c], COL_PIN[c]) == GPIO_PIN_RESET) {
                /* debounce */
                HAL_Delay(20);
                if (HAL_GPIO_ReadPin(COL_PORT[c], COL_PIN[c]) == GPIO_PIN_RESET) {
                    /* wait for release */
                    while (HAL_GPIO_ReadPin(COL_PORT[c], COL_PIN[c]) == GPIO_PIN_RESET) { }
                    /* restore all rows low for next scan */
                    for (int i = 0; i < 4; i++)
                        HAL_GPIO_WritePin(ROW_PORT[i], ROW_PIN[i], GPIO_PIN_SET);
                    DBG("Key: %c (r=%d c=%d)", KEY_MAP[r][c], r, c);
                    return KEY_MAP[r][c];
                }
            }
        }
    }
    return 0;
}

char Keypad_GetKey(uint32_t timeout_ms) {
    uint32_t start = HAL_GetTick();
    while ((HAL_GetTick() - start) < timeout_ms) {
        char k = Keypad_Scan();
        if (k) return k;
    }
    return 0;
}
