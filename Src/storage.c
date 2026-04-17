#include "storage.h"
#include "debug.h"
#include <string.h>

/* For simplicity the config lives in RAM. To persist across resets, port this
 * to STM32F4 internal flash (e.g. sector 11) using HAL_FLASH_Program. */

Config g_cfg;

void Storage_ResetToDefaults(void) {
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.magic = 0xDEADBEEF;
    strcpy(g_cfg.master_pin, "9999");
    strcpy(g_cfg.duress_pin, "0000");
    g_cfg.rfid_only = 0;

    /* Pre-enroll one demo card (replace with your own UID or enroll via admin) */
    g_cfg.cards[0].valid = 1;
    g_cfg.cards[0].uid[0] = 0xDE;
    g_cfg.cards[0].uid[1] = 0xAD;
    g_cfg.cards[0].uid[2] = 0xBE;
    g_cfg.cards[0].uid[3] = 0xEF;
    strcpy(g_cfg.cards[0].pin, "1234");

    DBG("Storage reset to defaults. Master=%s Duress=%s", g_cfg.master_pin, g_cfg.duress_pin);
}

void Storage_Init(void) {
    if (g_cfg.magic != 0xDEADBEEF) Storage_ResetToDefaults();
}

int Storage_FindCard(const uint8_t *uid) {
    for (int i = 0; i < MAX_CARDS; i++) {
        if (!g_cfg.cards[i].valid) continue;
        if (memcmp(g_cfg.cards[i].uid, uid, UID_LEN) == 0) return i;
    }
    return -1;
}

int Storage_AddCard(const uint8_t *uid) {
    if (Storage_FindCard(uid) >= 0) { DBG("Card already enrolled"); return -1; }
    for (int i = 0; i < MAX_CARDS; i++) {
        if (!g_cfg.cards[i].valid) {
            g_cfg.cards[i].valid = 1;
            g_cfg.cards[i].blacklisted = 0;
            memcpy(g_cfg.cards[i].uid, uid, UID_LEN);
            strcpy(g_cfg.cards[i].pin, "1234");
            DBG("Enrolled card at slot %d", i);
            return i;
        }
    }
    DBGE("Card table full");
    return -1;
}

int Storage_DeleteCard(const uint8_t *uid) {
    int i = Storage_FindCard(uid);
    if (i < 0) return 0;
    memset(&g_cfg.cards[i], 0, sizeof(CardEntry));
    DBG("Deleted card at slot %d", i);
    return 1;
}
