# Secure Door Lock — STM32F429I-DISC1 (Keil uVision)

## Files

```
Inc/
  debug.h       lcd_i2c.h    keypad.h    rc522.h    storage.h
Src/
  main.c        debug.c      lcd_i2c.c   keypad.c   rc522.c   storage.c
```

## Creating the Keil project (quickest path)

1. **File → New → uVision Project…**, pick device **STM32F429ZITx** (Discovery has ZI).
2. In the Manage Run-Time Environment dialog, tick:
   - **CMSIS → CORE**
   - **Device → Startup**
   - **Device → STM32Cube Framework (API) → Classic** (or use the HAL driver pack)
   - **Device → STM32Cube HAL → Common, Cortex, GPIO, SPI, I2C, RCC, PWR**
3. Add all files from `Src/` to the project's Source Group.
4. Project → Options → C/C++ → Include Paths: add the `Inc/` directory and the Keil HAL include paths (they're added automatically if you used the RTE).
5. Project → Options → C/C++ → Define: add `STM32F429xx` and `USE_HAL_DRIVER`.
6. Project → Options → Target → **MicroLIB** should be ticked (so `printf` is small).
7. Build (F7). Flash (F8).

If you prefer CubeMX: generate a Keil project targeting STM32F429ZI-Discovery with clock set to 180 MHz (HSE 8 MHz, PLL/M=8, N=360, P=2), enable SPI1 (master, 8-bit, CPOL=Low CPHA=1Edge, prescaler 32), I2C1 at 100 kHz, then copy our `Src/*.c` and `Inc/*.h` in and overwrite the generated `main.c`.

## Enabling SWO (printf) in Keil

Our `debug.c` sends `printf` to ITM port 0 over SWO.

- Project → Options → Debug → pick **ST-Link Debugger**, click **Settings**.
- Tab **Trace**: tick *Enable*, Core Clock = **180 MHz**, Port 0 → *ITM Stimulus Ports* box `0x00000001`.
- Start debug session. **View → Serial Windows → Debug (printf) Viewer**.

If SWO does not work on your board (some DISC1 revisions route PB3 oddly), swap `ITM_SendChar` in `debug.c` for a UART TX call (e.g. USART1 on PA9/PA10 via a USB-TTL adapter).

## Pin map (recap)

| Peripheral | Pin(s) |
|---|---|
| SPI1 / RC522  | PA3=RST  PA4=SS  PA5=SCK  PA6=MISO  PA7=MOSI |
| I2C1 / LCD    | PB8=SCL  PB9=SDA  (5V power, level shifts via pull-ups) |
| Keypad rows   | PB0, PB1, PB2, PB10 (output) |
| Keypad cols   | PB12..PB15 (input pull-up) |
| Buzzer        | PC6 |
| Green LED     | PC0 |
| Red LED       | PD0 |
| Tamper btn    | PA0 (to GND; internal pull-up, EXTI falling) |

Note: only one green + one red LED are used in firmware. If you want all five of each wired, connect them in parallel through their own resistors to the same GPIO. The design docs mention multiple LEDs but one GPIO per color is sufficient.

## Keypad layout assumed

```
1 2 3 A
4 5 6 B
7 8 9 C
* 0 # D
```

In PIN entry:
- digits 0–9 enter the PIN (masked as `*`)
- `#` submits
- `*` is backspace
- `D` cancels

At idle:
- Tap `A` → master-PIN only entry path
- Hold `#` for 2 s → admin entry (requires master PIN)

Admin menu keys: `A`=enroll card, `B`=delete card, `C`=change master PIN, `D`=exit.

## Defaults (see `Src/storage.c`)

- Master PIN: `9999`
- Duress PIN: `0000` (grants entry, silently flags an alarm — change for real use)
- Per-card PIN: `1234` (set on enroll; edit `storage.c` to customise)
- One demo card with UID `DE AD BE EF` is pre-enrolled for testing the flow before you scan a real card.

## LCD I²C address

Most PCF8574 backpacks are at `0x27`; some are `0x3F`. If the screen is blank after boot:
1. Watch the debug printout — we probe the address on init.
2. If not found, change `LCD_I2C_ADDR` in `Inc/lcd_i2c.h`.

## Flow summary

1. Idle: red LED on, LCD shows "Scan card…".
2. Scan known card → LCD prompts for PIN → enter 4 digits + `#`.
3. Correct PIN ⇒ green LED + 2-tone beep + 5 s unlock window with countdown.
4. Wrong PIN / unknown card / bad master ⇒ red blink + low tone; 3 fails in a row ⇒ 30 s lockout with on-screen countdown + rapid beep.
5. Tamper button press at any time ⇒ 8-second audible alarm.
6. Duress PIN ⇒ grants access but logs a silent alarm (for real deployment route this to a radio / GSM module).

## Things to change for a "real" product

- Persist `Config` into internal flash (sector 11 is a good candidate on F429) so enrolments survive a reboot.
- Drive the solenoid/relay with the green LED pin (or a dedicated pin) through a transistor + flyback diode.
- Add a reed switch so "door left open" can be detected and trigger an auto-alarm.
- Replace the buzzer beep timing loops with a hardware timer so they don't block the state machine during long events.
