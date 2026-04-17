#include "debug.h"
#include "stm32f4xx.h"

void debug_init(void) { /* ITM is enabled by the debugger */ }

/* Retarget printf to ITM Stimulus Port 0 (SWO) - requires MicroLIB */
int fputc(int ch, FILE *f) {
    (void)f;
    ITM_SendChar((uint32_t)ch);
    return ch;
}
