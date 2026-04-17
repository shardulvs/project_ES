/* ============================================================================
 *  Secure Door Lock System — STM32F429I-DISC1
 *  Features: RFID (RC522) + PIN keypad, tamper button, lockout, admin menu,
 *            duress PIN, LEDs, buzzer, I2C LCD, ITM SWO debug prints.
 *
 *  PIN MAP
 *    RC522  RST=PA3, SS=PA4, SCK=PA5, MISO=PA6, MOSI=PA7 (SPI1)
 *    Keypad rows=PB0,PB1,PB2,PB10   cols=PB12..PB15 (cols = input pull-up)
 *    LCD    I2C1 SDA=PB9, SCL=PB8
 *    Buzzer PC6   Green LED PC0   Red LED PD0   Tamper button PA0 (to GND)
 *
 *  DEBUG
 *    printf() is redirected to ITM SWO (see debug.c). In Keil uVision:
 *      Debug settings -> Trace -> Enable, Core Clock 180 MHz, ITM Port 0
 *      View -> Serial Windows -> Debug (printf) Viewer
 * ========================================================================== */

#include "stm32f4xx_hal.h"
#include "debug.h"
#include "lcd_i2c.h"
#include "keypad.h"
#include "rc522.h"
#include "storage.h"
#include <string.h>
#include <stdio.h>

/* ----- Handles ----- */
I2C_HandleTypeDef hi2c1;
SPI_HandleTypeDef hspi1;

/* ----- Pin shortcuts ----- */
#define PIN_RFID_RST_PORT   GPIOA
#define PIN_RFID_RST        GPIO_PIN_3
#define PIN_RFID_SS_PORT    GPIOA
#define PIN_RFID_SS         GPIO_PIN_4

#define PIN_BUZZER_PORT     GPIOC
#define PIN_BUZZER          GPIO_PIN_6

#define PIN_GREEN_PORT      GPIOC
#define PIN_GREEN           GPIO_PIN_0

#define PIN_RED_PORT        GPIOD
#define PIN_RED             GPIO_PIN_0

#define PIN_TAMPER_PORT     GPIOA
#define PIN_TAMPER          GPIO_PIN_0

/* ----- State machine ----- */
typedef enum {
    ST_IDLE,
    ST_WAIT_PIN,
    ST_GRANTED,
    ST_DENIED,
    ST_LOCKOUT,
    ST_ADMIN,
    ST_ENROLL,
    ST_DELETE,
    ST_CHANGE_PIN,
    ST_ALARM
} State;

static State         g_state = ST_IDLE;
static uint8_t       g_fail_count = 0;
static uint32_t      g_lockout_until = 0;
static uint8_t       g_last_uid[UID_LEN];
static int           g_last_slot = -1;
volatile uint8_t     g_tamper_flag = 0;     /* set in EXTI ISR — see stm32f4xx_it.c */

#define MAX_FAILS       3
#define LOCKOUT_MS      30000UL
#define UNLOCK_MS       5000UL
#define PIN_TIMEOUT_MS  10000UL

/* ----- LED / buzzer helpers ----- */
static void led_green(uint8_t on) { HAL_GPIO_WritePin(PIN_GREEN_PORT, PIN_GREEN, on?GPIO_PIN_SET:GPIO_PIN_RESET); }
static void led_red(uint8_t on)   { HAL_GPIO_WritePin(PIN_RED_PORT,   PIN_RED,   on?GPIO_PIN_SET:GPIO_PIN_RESET); }
static void buzzer(uint8_t on)    { HAL_GPIO_WritePin(PIN_BUZZER_PORT,PIN_BUZZER,on?GPIO_PIN_SET:GPIO_PIN_RESET); }

static void beep_short(void)       { buzzer(1); HAL_Delay(40);  buzzer(0); }
static void beep_granted(void)     { buzzer(1); HAL_Delay(80);  buzzer(0); HAL_Delay(60); buzzer(1); HAL_Delay(80);  buzzer(0); }
static void beep_denied(void)      { buzzer(1); HAL_Delay(400); buzzer(0); }
static void beep_lockout(void)     { for (int i=0;i<6;i++){buzzer(1);HAL_Delay(80);buzzer(0);HAL_Delay(80);} }

/* ============================================================================
 *  HAL clock + peripheral init
 * ========================================================================== */
static void SystemClock_Config(void) {
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};

    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState       = RCC_HSE_ON;
    osc.PLL.PLLState   = RCC_PLL_ON;
    osc.PLL.PLLSource  = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLM = 8;    /* DISC1 has 8 MHz HSE */
    osc.PLL.PLLN = 360;
    osc.PLL.PLLP = RCC_PLLP_DIV2;
    osc.PLL.PLLQ = 7;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) while (1);

    HAL_PWREx_EnableOverDrive();

    clk.ClockType      = RCC_CLOCKTYPE_SYSCLK|RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV4;
    clk.APB2CLKDivider = RCC_HCLK_DIV2;
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_5) != HAL_OK) while (1);
}

static void MX_GPIO_Init(void) {
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();

    GPIO_InitTypeDef g = {0};

    /* Buzzer / LEDs — push-pull output */
    g.Mode = GPIO_MODE_OUTPUT_PP; g.Pull = GPIO_NOPULL; g.Speed = GPIO_SPEED_FREQ_LOW;

    g.Pin = PIN_BUZZER; HAL_GPIO_Init(PIN_BUZZER_PORT, &g); HAL_GPIO_WritePin(PIN_BUZZER_PORT, PIN_BUZZER, 0);
    g.Pin = PIN_GREEN;  HAL_GPIO_Init(PIN_GREEN_PORT,  &g); HAL_GPIO_WritePin(PIN_GREEN_PORT,  PIN_GREEN,  0);
    g.Pin = PIN_RED;    HAL_GPIO_Init(PIN_RED_PORT,    &g); HAL_GPIO_WritePin(PIN_RED_PORT,    PIN_RED,    0);

    /* RFID RST + SS — push-pull output, idle HIGH */
    g.Pin = PIN_RFID_RST; HAL_GPIO_Init(PIN_RFID_RST_PORT, &g); HAL_GPIO_WritePin(PIN_RFID_RST_PORT, PIN_RFID_RST, 1);
    g.Pin = PIN_RFID_SS;  HAL_GPIO_Init(PIN_RFID_SS_PORT,  &g); HAL_GPIO_WritePin(PIN_RFID_SS_PORT,  PIN_RFID_SS,  1);

    /* Tamper button: PA0 input, external interrupt falling edge, pull-up */
    g.Mode  = GPIO_MODE_IT_FALLING;
    g.Pull  = GPIO_PULLUP;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    g.Pin   = PIN_TAMPER;
    HAL_GPIO_Init(PIN_TAMPER_PORT, &g);
    HAL_NVIC_SetPriority(EXTI0_IRQn, 10, 0);
    HAL_NVIC_EnableIRQ(EXTI0_IRQn);
}

static void MX_SPI1_Init(void) {
    __HAL_RCC_SPI1_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    GPIO_InitTypeDef g = {0};
    g.Pin       = GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7;
    g.Mode      = GPIO_MODE_AF_PP;
    g.Pull      = GPIO_NOPULL;
    g.Speed     = GPIO_SPEED_FREQ_HIGH;
    g.Alternate = GPIO_AF5_SPI1;
    HAL_GPIO_Init(GPIOA, &g);

    hspi1.Instance               = SPI1;
    hspi1.Init.Mode              = SPI_MODE_MASTER;
    hspi1.Init.Direction         = SPI_DIRECTION_2LINES;
    hspi1.Init.DataSize          = SPI_DATASIZE_8BIT;
    hspi1.Init.CLKPolarity       = SPI_POLARITY_LOW;
    hspi1.Init.CLKPhase          = SPI_PHASE_1EDGE;
    hspi1.Init.NSS               = SPI_NSS_SOFT;
    hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_32;  /* ~2.8 MHz — safe for RC522 */
    hspi1.Init.FirstBit          = SPI_FIRSTBIT_MSB;
    hspi1.Init.TIMode            = SPI_TIMODE_DISABLE;
    hspi1.Init.CRCCalculation    = SPI_CRCCALCULATION_DISABLE;
    if (HAL_SPI_Init(&hspi1) != HAL_OK) DBGE("SPI1 init failed");
}

static void MX_I2C1_Init(void) {
    __HAL_RCC_I2C1_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    GPIO_InitTypeDef g = {0};
    g.Pin       = GPIO_PIN_8 | GPIO_PIN_9;
    g.Mode      = GPIO_MODE_AF_OD;          /* open-drain for I2C */
    g.Pull      = GPIO_PULLUP;              /* internal; still prefer external 4.7k */
    g.Speed     = GPIO_SPEED_FREQ_HIGH;
    g.Alternate = GPIO_AF4_I2C1;
    HAL_GPIO_Init(GPIOB, &g);

    hi2c1.Instance             = I2C1;
    hi2c1.Init.ClockSpeed      = 100000;
    hi2c1.Init.DutyCycle       = I2C_DUTYCYCLE_2;
    hi2c1.Init.OwnAddress1     = 0;
    hi2c1.Init.AddressingMode  = I2C_ADDRESSINGMODE_7BIT;
    hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    hi2c1.Init.NoStretchMode   = I2C_NOSTRETCH_DISABLE;
    if (HAL_I2C_Init(&hi2c1) != HAL_OK) DBGE("I2C1 init failed");
}

/* EXTI0 ISR callback — tamper button.
 * The ISR entry (EXTI0_IRQHandler) and core handlers live in Src/stm32f4xx_it.c
 * so this file doesn't collide with a CubeMX-generated one. */
extern volatile uint8_t g_tamper_flag;
void HAL_GPIO_EXTI_Callback(uint16_t pin) {
    if (pin == PIN_TAMPER) {
        g_tamper_flag = 1;
    }
}

/* ============================================================================
 *  UI helpers
 * ========================================================================== */
static void lcd_header(const char *line1) {
    LCD_Clear();
    LCD_PrintAt(0, 0, line1);
}

static void show_idle(void) {
    LCD_Clear();
    LCD_PrintAt(0, 0, "Secure Lock");
    LCD_PrintAt(0, 1, "Scan card...");
    led_green(0);
    led_red(1);            /* idle = locked; red on */
}

static void uid_to_str(const uint8_t *u, char *out) {
    sprintf(out, "%02X%02X%02X%02X", u[0], u[1], u[2], u[3]);
}

/* Ask for PIN, mask with '*'. Returns 1 if entered, 0 on timeout/cancel.
 * '#' submits, '*' backspaces, 'D' cancels. */
static uint8_t prompt_pin(const char *title, char out[PIN_LEN + 1]) {
    char buf[PIN_LEN + 1] = {0};
    uint8_t n = 0;

    LCD_Clear();
    LCD_PrintAt(0, 0, title);
    LCD_PrintAt(0, 1, "PIN:");

    uint32_t start = HAL_GetTick();
    while ((HAL_GetTick() - start) < PIN_TIMEOUT_MS) {
        char k = Keypad_Scan();
        if (!k) continue;
        beep_short();
        start = HAL_GetTick();             /* reset timeout on any key */

        if (k == 'D') { DBG("PIN cancelled"); return 0; }
        if (k == '*') {                    /* backspace */
            if (n > 0) { n--; buf[n] = 0; LCD_PrintAt(4 + n, 1, " "); LCD_SetCursor(4 + n, 1); }
            continue;
        }
        if (k == '#') {                    /* submit */
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

/* ============================================================================
 *  Admin menu
 *
 *  Entered by holding '#' 2 seconds at idle, then master PIN.
 *  Keys inside admin menu:
 *    A = enroll new card
 *    B = delete card
 *    C = change master PIN
 *    D = exit
 * ========================================================================== */
static void admin_enroll(void) {
    LCD_Clear();
    LCD_PrintAt(0, 0, "Enroll: scan");
    LCD_PrintAt(0, 1, "card now...");
    uint8_t uid[RC522_UID_LEN];
    uint32_t start = HAL_GetTick();
    while ((HAL_GetTick() - start) < 10000) {
        led_green(((HAL_GetTick()/200) & 1));   /* pulse green during enroll */
        if (RC522_ReadUID(uid)) {
            led_green(1);
            char s[12]; uid_to_str(uid, s);
            DBG("Enroll UID=%s", s);
            int idx = Storage_AddCard(uid);
            LCD_Clear();
            LCD_PrintAt(0, 0, idx >= 0 ? "Enrolled!" : "Failed/Full");
            LCD_PrintAt(0, 1, s);
            beep_granted();
            HAL_Delay(1500);
            return;
        }
        HAL_Delay(50);
    }
    LCD_Clear(); LCD_PrintAt(0, 0, "Enroll timeout"); HAL_Delay(1200);
}

static void admin_delete(void) {
    LCD_Clear();
    LCD_PrintAt(0, 0, "Delete: scan");
    LCD_PrintAt(0, 1, "card to del");
    uint8_t uid[RC522_UID_LEN];
    uint32_t start = HAL_GetTick();
    while ((HAL_GetTick() - start) < 10000) {
        if (RC522_ReadUID(uid)) {
            int ok = Storage_DeleteCard(uid);
            LCD_Clear();
            LCD_PrintAt(0, 0, ok ? "Deleted" : "Not found");
            beep_short();
            HAL_Delay(1200);
            return;
        }
        HAL_Delay(50);
    }
    LCD_Clear(); LCD_PrintAt(0, 0, "Delete timeout"); HAL_Delay(1200);
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
    HAL_Delay(1200);
}

static void admin_menu(void) {
    DBG("Entering admin menu");
    g_state = ST_ADMIN;
    uint32_t start = HAL_GetTick();
    uint8_t blink = 0;

    while ((HAL_GetTick() - start) < 20000) {
        /* Both LEDs blinking alternately */
        blink = ((HAL_GetTick() / 300) & 1);
        led_green(blink); led_red(!blink);

        LCD_PrintAt(0, 0, "ADMIN MENU     ");
        LCD_PrintAt(0, 1, "A:en B:del C:pin");

        char k = Keypad_Scan();
        if (!k) continue;
        start = HAL_GetTick();
        beep_short();

        if      (k == 'A') { admin_enroll();     }
        else if (k == 'B') { admin_delete();     }
        else if (k == 'C') { admin_change_pin(); }
        else if (k == 'D') { break;              }
    }
    led_green(0); led_red(0);
    g_state = ST_IDLE;
    show_idle();
}

/* ============================================================================
 *  Access flow
 * ========================================================================== */
static void grant_access(int card_slot) {
    g_state = ST_GRANTED;
    g_fail_count = 0;
    g_last_slot = card_slot;

    LCD_Clear();
    LCD_PrintAt(0, 0, "ACCESS GRANTED");
    if (card_slot >= 0) {
        char s[16];
        uid_to_str(g_cfg.cards[card_slot].uid, s);
        LCD_PrintAt(0, 1, s);
    } else {
        LCD_PrintAt(0, 1, "Master PIN");
    }
    led_red(0); led_green(1);
    beep_granted();

    DBG("ACCESS GRANTED (slot=%d)", card_slot);

    uint32_t start = HAL_GetTick();
    while ((HAL_GetTick() - start) < UNLOCK_MS) {
        uint32_t left = (UNLOCK_MS - (HAL_GetTick() - start)) / 1000 + 1;
        char buf[17]; snprintf(buf, sizeof(buf), "Unlocked %lus  ", (unsigned long)left);
        LCD_PrintAt(0, 1, buf);
        HAL_Delay(100);
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

    for (int i = 0; i < 3; i++) { led_red(0); HAL_Delay(100); led_red(1); HAL_Delay(100); }
    beep_denied();
    HAL_Delay(800);

    if (g_fail_count >= MAX_FAILS) {
        g_state = ST_LOCKOUT;
        g_lockout_until = HAL_GetTick() + LOCKOUT_MS;
        beep_lockout();
    } else {
        g_state = ST_IDLE;
        show_idle();
    }
}

static void do_lockout(void) {
    DBG("Lockout active");
    while (HAL_GetTick() < g_lockout_until) {
        uint32_t left = (g_lockout_until - HAL_GetTick()) / 1000 + 1;
        char buf[17];
        snprintf(buf, sizeof(buf), "LOCKED %lus     ", (unsigned long)left);
        LCD_PrintAt(0, 0, "TOO MANY FAILS ");
        LCD_PrintAt(0, 1, buf);
        /* Rapid red blink */
        led_red(((HAL_GetTick()/150) & 1));
        HAL_Delay(50);
    }
    g_fail_count = 0;
    g_state = ST_IDLE;
    DBG("Lockout cleared");
    show_idle();
}

static void trigger_alarm(const char *reason, uint8_t silent) {
    DBGE("ALARM: %s (silent=%u)", reason, silent);
    g_state = ST_ALARM;
    LCD_Clear();
    LCD_PrintAt(0, 0, silent ? "ACCESS GRANTED" : "!! TAMPER !!");
    LCD_PrintAt(0, 1, silent ? "Unlocked 5s   " : reason);

    if (silent) {
        /* Duress: appear normal, but log + keep flag in storage for forensics.
         * Here we just grant like normal but keep red LED subtly flickering. */
        led_green(1); led_red(0);
        for (int i = 0; i < 50; i++) { HAL_Delay(100); }
        led_green(0);
    } else {
        /* Audible alarm for 8 s */
        uint32_t start = HAL_GetTick();
        while ((HAL_GetTick() - start) < 8000) {
            led_red(((HAL_GetTick()/100) & 1));
            buzzer(((HAL_GetTick()/100) & 1));
            HAL_Delay(20);
        }
        buzzer(0);
    }

    g_tamper_flag = 0;
    g_state = ST_IDLE;
    show_idle();
}

/* Hold '#' for 2s at idle to attempt admin entry. */
static uint8_t check_admin_hotkey(void) {
    /* Only check if '#' is currently held down.
     * We detect by driving rows low and reading col 3 on row 4 (# position). */
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0,  GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_1,  GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_2,  GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_10, GPIO_PIN_RESET);   /* row 4 low (has *,0,#,D) */
    for (volatile int d = 0; d < 500; d++) __NOP();
    uint8_t pressed = (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_14) == GPIO_PIN_RESET);  /* col 3 = '#' */
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_10, GPIO_PIN_SET);
    return pressed;
}

/* ============================================================================
 *  Main
 * ========================================================================== */
int main(void) {
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_SPI1_Init();
    MX_I2C1_Init();

    debug_init();
    DBG("=== Secure Door Lock booting ===");
    DBG("SystemCoreClock = %lu Hz", SystemCoreClock);

    LCD_Init(&hi2c1);
    LCD_PrintAt(0, 0, "Boot...");
    Keypad_Init();
    Storage_Init();
    RC522_Init(&hspi1, PIN_RFID_SS_PORT, PIN_RFID_SS, PIN_RFID_RST_PORT, PIN_RFID_RST);

    show_idle();
    DBG("Ready. Max cards=%d, rfid_only=%u", MAX_CARDS, g_cfg.rfid_only);

    uint32_t hash_hold_start = 0;

    for (;;) {

        /* ---- Tamper handling ---- */
        if (g_tamper_flag) { trigger_alarm("Button!", 0); continue; }

        /* ---- Lockout handling ---- */
        if (g_state == ST_LOCKOUT) { do_lockout(); continue; }

        /* ---- Admin hotkey: hold '#' 2 s ---- */
        if (check_admin_hotkey()) {
            if (!hash_hold_start) hash_hold_start = HAL_GetTick();
            else if ((HAL_GetTick() - hash_hold_start) > 2000) {
                hash_hold_start = 0;
                char pin[PIN_LEN + 1];
                if (prompt_pin("Admin PIN", pin) && strcmp(pin, g_cfg.master_pin) == 0) {
                    admin_menu();
                } else {
                    deny_access("Bad admin");
                }
                continue;
            }
        } else {
            hash_hold_start = 0;
        }

        /* ---- RFID scan ---- */
        uint8_t uid[RC522_UID_LEN];
        if (RC522_ReadUID(uid)) {
            char uidstr[12]; uid_to_str(uid, uidstr);
            DBG("UID read: %s", uidstr);
            memcpy(g_last_uid, uid, UID_LEN);

            int idx = Storage_FindCard(uid);
            if (idx < 0) {
                LCD_Clear();
                LCD_PrintAt(0, 0, "UNKNOWN CARD");
                LCD_PrintAt(0, 1, uidstr);
                beep_denied();
                HAL_Delay(1000);
                deny_access("Unknown");
                continue;
            }
            if (g_cfg.cards[idx].blacklisted) {
                LCD_Clear();
                LCD_PrintAt(0, 0, "BLACKLISTED");
                deny_access("Blacklist");
                continue;
            }

            if (g_cfg.rfid_only == 1) {
                grant_access(idx);
                continue;
            }

            /* Dual-factor: require PIN now */
            LCD_Clear();
            LCD_PrintAt(0, 0, "Card OK");
            LCD_PrintAt(0, 1, uidstr);
            HAL_Delay(700);

            char pin[PIN_LEN + 1];
            if (!prompt_pin("Enter PIN", pin)) { deny_access("PIN timeout"); continue; }

            if (strcmp(pin, g_cfg.duress_pin) == 0) { trigger_alarm("duress", 1); continue; }
            if (strcmp(pin, g_cfg.cards[idx].pin) == 0 || strcmp(pin, g_cfg.master_pin) == 0) {
                grant_access(idx);
            } else {
                deny_access("Bad PIN");
            }
            continue;
        }

        /* ---- Keypad-only path: master PIN emergency entry ----
         * Press 'A' at idle to enter master-PIN-only flow. */
        char k = Keypad_Scan();
        if (k == 'A' || g_cfg.rfid_only == 2) {
            char pin[PIN_LEN + 1];
            if (prompt_pin("Master PIN", pin)) {
                if      (strcmp(pin, g_cfg.duress_pin) == 0) trigger_alarm("duress", 1);
                else if (strcmp(pin, g_cfg.master_pin) == 0) grant_access(-1);
                else deny_access("Bad master");
            }
            continue;
        }

        HAL_Delay(80);
    }
}

/* Core interrupt handlers live in Src/stm32f4xx_it.c */
