#ifndef STORAGE_H
#define STORAGE_H

#include "stm32f4xx_hal.h"

#define MAX_CARDS       8
#define PIN_LEN         4
#define UID_LEN         4       /* first 4 bytes of RC522 anti-collision serial */

typedef struct {
    uint8_t  uid[UID_LEN];
    uint8_t  valid;             /* 1 = slot in use */
    uint8_t  blacklisted;       /* 1 = denied even if matches */
    char     pin[PIN_LEN + 1];  /* per-user PIN, null-terminated */
} CardEntry;

typedef struct {
    uint32_t   magic;           /* 0xDEADBEEF when initialized */
    char       master_pin[PIN_LEN + 1];
    char       duress_pin[PIN_LEN + 1];
    CardEntry  cards[MAX_CARDS];
    uint8_t    rfid_only;       /* modes: 0=dual (default), 1=rfid only, 2=pin only */
} Config;

extern Config g_cfg;

void Storage_Init(void);
void Storage_ResetToDefaults(void);
int  Storage_FindCard(const uint8_t *uid);      /* returns index or -1 */
int  Storage_AddCard(const uint8_t *uid);       /* returns index or -1 if full */
int  Storage_DeleteCard(const uint8_t *uid);    /* returns 1 if deleted */

#endif
