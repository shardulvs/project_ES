/* ============================================================================
 *  Secure Door Lock System - STM32F429I-DISC1 (no HAL, CMSIS only)
 *
 *  PIN MAP
 *    RC522  RST=PA3, SS=PA4, SCK=PA5, MISO=PA6, MOSI=PA7 (SPI1, AF5)
 *    Keypad rows=PB0,PB1,PB2,PB10   cols=PB12..PB15
 *    LCD    I2C1 SDA=PB9, SCL=PB8 (AF4, open-drain, pulled up)
 *    Buzzer PC6   Green LED PC0   Red LED PD0   Tamper button PA0 (to GND)
 *
 *  Clock: default HSI = 16 MHz (no PLL for simplicity)
 *
 *  Debug: printf -> ITM SWO. In Keil: Debug Settings -> Trace -> Enable,
 *         Core Clock = 16 MHz, ITM Port 0, then open Debug (printf) Viewer.
 * ========================================================================== */

#include "stm32f4xx.h"
#include "bsp.h"
#include "debug.h"
#include "lcd_i2c.h"
#include "keypad.h"
#include "rc522.h"
#include "storage.h"
#include <string.h>
#include <stdio.h>

/* Pin numbers (bit position inside the port) */
#define PIN_BUZZER    6    /* GPIOC */
#define PIN_GREEN     0    /* GPIOC */
#define PIN_RED       0    /* GPIOD */
#define PIN_TAMPER    0    /* GPIOA */

/* ---- State machine ---- */
typedef enum { ST_IDLE, ST_GRANTED, ST_LOCKOUT, ST_ADMIN, ST_ALARM } State;

static State   g_state = ST_IDLE;
static uint8_t g_fail_count = 0;
static uint32_t g_lockout_until = 0;
volatile uint8_t g_tamper_flag = 0;

#define MAX_FAILS       3
#define LOCKOUT_MS      30000UL
#define UNLOCK_MS       5000UL
#define PIN_TIMEOUT_MS  10000UL

/* ---- LED / buzzer ---- */
static void led_green(uint8_t on) { gpio_write(GPIOC, PIN_GREEN,  on); }
static void led_red  (uint8_t on) { gpio_write(GPIOD, PIN_RED,    on); }
static void buzzer   (uint8_t on) { gpio_write(GPIOC, PIN_BUZZER, on); }

static void beep_short  (void) { buzzer(1); delay_ms(40);  buzzer(0); }
static void beep_granted(void) { buzzer(1); delay_ms(80);  buzzer(0); delay_ms(60); buzzer(1); delay_ms(80); buzzer(0); }
static void beep_denied (void) { buzzer(1); delay_ms(400); buzzer(0); }
static void beep_lockout(void) { for (int i=0;i<6;i++){buzzer(1);delay_ms(80);buzzer(0);delay_ms(80);} }

/* ============================================================================
 *  GPIO + external-interrupt setup for tamper button
 * ========================================================================== */
static void tamper_init(void) {
    gpio_clk_enable(GPIOA);
    gpio_mode(GPIOA, PIN_TAMPER, 0);    /* input */
    gpio_pull(GPIOA, PIN_TAMPER, 1);    /* pull-up */

    /* SYSCFG: map EXTI0 to PA0 */
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;
    SYSCFG->EXTICR[0] &= ~SYSCFG_EXTICR1_EXTI0;    /* 0000 = PA */

    /* Unmask EXTI0, falling-edge trigger (button pulls to GND) */
    EXTI->IMR  |= (1U << 0);
    EXTI->FTSR |= (1U << 0);
    EXTI->RTSR &= ~(1U << 0);

    NVIC_SetPriority(EXTI0_IRQn, 2);
    NVIC_EnableIRQ(EXTI0_IRQn);
}

/* EXTI0 ISR - also defined in bsp.c area? No, we put it here for locality. */
void EXTI0_IRQHandler(void) {
    if (EXTI->PR & (1U << 0)) {
        EXTI->PR = (1U << 0);      /* clear pending (write 1) */
        g_tamper_flag = 1;
    }
}

static void misc_gpio_init(void) {
    gpio_clk_enable(GPIOC);
    gpio_clk_enable(GPIOD);

    /* Buzzer PC6, Green PC0, Red PD0 - push-pull output, start LOW */
    gpio_mode(GPIOC, PIN_BUZZER, 1); gpio_otype(GPIOC, PIN_BUZZER, 0); gpio_write(GPIOC, PIN_BUZZER, 0);
    gpio_mode(GPIOC, PIN_GREEN,  1); gpio_otype(GPIOC, PIN_GREEN,  0); gpio_write(GPIOC, PIN_GREEN,  0);
    gpio_mode(GPIOD, PIN_RED,    1); gpio_otype(GPIOD, PIN_RED,    0); gpio_write(GPIOD, PIN_RED,    0);
}

static void spi_pins_init(void) {
    gpio_clk_enable(GPIOA);
    for (int pin = 5; pin <= 7; pin++) {
        gpio_mode (GPIOA, pin, 2);    /* AF */
        gpio_af   (GPIOA, pin, 5);    /* AF5 = SPI1 */
        gpio_otype(GPIOA, pin, 0);    /* push-pull */
        gpio_pull (GPIOA, pin, 0);    /* no pull */
        gpio_speed(GPIOA, pin, 2);
    }
}

static void i2c_pins_init(void) {
    gpio_clk_enable(GPIOB);
    for (int i = 0; i < 2; i++) {
        int pin = (i == 0) ? 8 : 9;
        gpio_mode (GPIOB, pin, 2);    /* AF */
        gpio_af   (GPIOB, pin, 4);    /* AF4 = I2C1 */
        gpio_otype(GPIOB, pin, 1);    /* open-drain */
        gpio_pull (GPIOB, pin, 1);    /* internal pull-up; external 4.7k still recommended */
        gpio_speed(GPIOB, pin, 1);
    }
}

/* ============================================================================
 *  UI helpers
 * ========================================================================== */
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

/* ---- Admin mode ---- */
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

/* ============================================================================
 *  Access flow
 * ========================================================================== */
static void grant_access(int card_slot) {
    g_state = ST_GRANTED;
    g_fail_count = 0;

    LCD_Clear();
    LCD_PrintAt(0, 0, "ACCESS GRANTED");
    if (card_slot >= 0) {
        char s[12]; uid_to_str(g_cfg.cards[card_slot].uid, s);
        LCD_PrintAt(0, 1, s);
    } else {
        LCD_PrintAt(0, 1, "Master PIN");
    }
    led_red(0); led_green(1);
    beep_granted();
    DBG("ACCESS GRANTED (slot=%d)", card_slot);

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
    DBGE("ALARM: %s (silent=%u)", reason, silent);
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

/* Check if '#' is currently held. Used to detect admin hotkey. */
static uint8_t hash_held(void) {
    gpio_write(GPIOB, 0,  1);
    gpio_write(GPIOB, 1,  1);
    gpio_write(GPIOB, 2,  1);
    gpio_write(GPIOB, 10, 0);         /* drive row 4 (*,0,#,D) low */
    for (volatile int d = 0; d < 500; d++) __NOP();
    uint8_t pressed = (gpio_read(GPIOB, 14) == 0);    /* col 3 = '#' */
    gpio_write(GPIOB, 10, 1);
    return pressed;
}

/* ============================================================================
 *  main
 * ========================================================================== */
int main(void) {
    SystemCoreClockUpdate();
    systick_init();

    misc_gpio_init();
    spi_pins_init();
    i2c_pins_init();
    tamper_init();

    spi1_init();
    i2c1_init();

    debug_init();
    DBG("=== Secure Door Lock booting ===");
    DBG("SystemCoreClock = %lu Hz", (unsigned long)SystemCoreClock);

    LCD_Init();
    LCD_PrintAt(0, 0, "Boot...");
    Keypad_Init();
    Storage_Init();
    RC522_Init();

    show_idle();
    DBG("Ready. Max cards=%d, rfid_only=%u", MAX_CARDS, g_cfg.rfid_only);

    uint32_t hash_hold_start = 0;

    for (;;) {

        if (g_tamper_flag)          { trigger_alarm("Button!", 0); continue; }
        if (g_state == ST_LOCKOUT)  { do_lockout();                 continue; }

        /* Admin hotkey: hold '#' for 2 s */
        if (hash_held()) {
            if (!hash_hold_start) hash_hold_start = millis();
            else if ((millis() - hash_hold_start) > 2000) {
                hash_hold_start = 0;
                char pin[PIN_LEN + 1];
                if (prompt_pin("Admin PIN", pin) && strcmp(pin, g_cfg.master_pin) == 0) {
                    admin_menu();
                } else {
                    deny_access("Bad admin");
                }
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

            /* Dual-factor: need PIN too */
            LCD_Clear();
            LCD_PrintAt(0, 0, "Card OK");
            LCD_PrintAt(0, 1, s);
            delay_ms(700);

            char pin[PIN_LEN + 1];
            if (!prompt_pin("Enter PIN", pin)) { deny_access("PIN timeout"); continue; }

            if      (strcmp(pin, g_cfg.duress_pin) == 0)     trigger_alarm("duress", 1);
            else if (strcmp(pin, g_cfg.cards[idx].pin) == 0
                  || strcmp(pin, g_cfg.master_pin) == 0)     grant_access(idx);
            else                                              deny_access("Bad PIN");
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
