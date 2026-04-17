#ifndef KEYPAD_H
#define KEYPAD_H

#include <stdint.h>

/* Rows = PB0, PB1, PB2, PB10 (output)
 * Cols = PB12, PB13, PB14, PB15 (input, pull-up)
 * Layout:
 *   1 2 3 A
 *   4 5 6 B
 *   7 8 9 C
 *   * 0 # D
 */
void Keypad_Init(void);
char Keypad_Scan(void);                    /* 0 if no key */
char Keypad_GetKey(uint32_t timeout_ms);   /* blocking, 0 on timeout */

#endif
