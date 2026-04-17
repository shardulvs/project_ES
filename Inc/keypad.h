#ifndef KEYPAD_H
#define KEYPAD_H

#include "stm32f4xx_hal.h"

/* Rows as OUTPUTS, Columns as INPUT PULL-UP.
 * Row pins:  PB0, PB1, PB2, PB10
 * Col pins:  PB12, PB13, PB14, PB15
 * Layout:
 *   1 2 3 A
 *   4 5 6 B
 *   7 8 9 C
 *   * 0 # D
 */

void Keypad_Init(void);
char Keypad_Scan(void);           /* 0 if no key, else ASCII char */
char Keypad_GetKey(uint32_t timeout_ms);  /* blocking, 0 on timeout */

#endif
