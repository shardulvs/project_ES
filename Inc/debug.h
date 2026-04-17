#ifndef DEBUG_H
#define DEBUG_H

#include <stdio.h>
#include "bsp.h"

/* Debug prints go out over ITM SWO. In Keil uVision:
 *   Debug Settings -> Trace -> Enable, Core Clock = 16 MHz, ITM Port 0
 *   View -> Serial Windows -> Debug (printf) Viewer
 */
void debug_init(void);

#define DBG(fmt, ...)  printf("[%lu] " fmt "\r\n", (unsigned long)millis(), ##__VA_ARGS__)
#define DBGE(fmt, ...) printf("[%lu][ERR] " fmt "\r\n", (unsigned long)millis(), ##__VA_ARGS__)

#endif
