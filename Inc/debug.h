#ifndef DEBUG_H
#define DEBUG_H

#include <stdio.h>
#include "stm32f4xx_hal.h"

/* Debug prints are sent via ITM (SWO). In Keil uVision:
 *   Debug -> Settings -> Trace -> Enable, set Core Clock = 180 MHz (or your HCLK)
 *   Then View -> Serial Windows -> Debug (printf) Viewer
 *
 * If SWO is not available, change DBG() to use UART or a no-op.
 */

void debug_init(void);

#define DBG(fmt, ...)  printf("[%lu] " fmt "\r\n", HAL_GetTick(), ##__VA_ARGS__)
#define DBGE(fmt, ...) printf("[%lu][ERR] " fmt "\r\n", HAL_GetTick(), ##__VA_ARGS__)

#endif
