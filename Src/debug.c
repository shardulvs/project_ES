#include "debug.h"

/* Keil MicroLIB retarget: fputc sends chars to ITM port 0 (SWO) */
void debug_init(void) { /* ITM is set up by the debugger automatically */ }

#ifdef __GNUC__
int _write(int file, char *ptr, int len) {
    for (int i = 0; i < len; i++) ITM_SendChar(ptr[i]);
    return len;
}
#else
int fputc(int ch, FILE *f) {
    (void)f;
    ITM_SendChar(ch);
    return ch;
}
#endif
