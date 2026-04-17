# Secure Door Lock - STM32F429I-DISC1 (Keil uVision, **no HAL**)

This version uses **only CMSIS and direct register access**. You do **not** need
to install STM32Cube HAL. This is the smallest possible setup.

## Files

```
Inc/
  bsp.h       debug.h    lcd_i2c.h   keypad.h   rc522.h   storage.h
Src/
  main.c      bsp.c      debug.c     lcd_i2c.c  keypad.c  rc522.c  storage.c
```

## Keil uVision setup (minimal)

1. **File -> New -> uVision Project...** -> pick **STMicroelectronics / STM32F429 / STM32F429ZI / STM32F429ZITx** (DISC1 has ZI).
2. In the *Manage Run-Time Environment* dialog, tick **only**:
   - **CMSIS -> CORE**
   - **Device -> Startup**

   That's it. Nothing else. Click **Resolve**, then **OK**. uVision drops a `system_stm32f4xx.c` and a `startup_stm32f429xx.s` into the project automatically.
3. In the project tree, add both `Inc/` and `Src/` folders. Right-click *Source Group 1* -> **Add Existing Files to Group** -> select every `.c` inside `Src/`.
4. **Project -> Options for Target** (Alt+F7):
   - Tab **C/C++ (AC6)** -> **Include Paths**: click `...` -> **New (Insert)** -> browse to your `Inc/` folder. OK.
   - Tab **C/C++ (AC6)** -> **Define**: add `STM32F429xx` (no spaces).
   - Tab **Target** -> tick **Use MicroLIB**.
5. **Build** (F7). Should finish with 0 errors.
6. **Flash** (F8).

## Enabling `printf` over SWO (debug prints)

The macros `DBG(...)` and `DBGE(...)` send text over ITM stimulus port 0, which the ST-Link carries out on the SWO pin.

- **Project -> Options -> Debug** -> choose **ST-Link Debugger** (right radio), click **Settings**.
- Tab **Trace**:
  - Tick **Enable**.
  - **Core Clock:** `16.000000 MHz` (we run on default HSI; no PLL is set up).
  - **Trace Port:** *Serial Wire Output - Manchester* (or *UART*, either works).
  - In *ITM Stimulus Ports*, make sure **Port 0** is ticked (the dropdown / bit-0 enable).
- Start debugging (Ctrl+F5). Open **View -> Serial Windows -> Debug (printf) Viewer**. You should see the boot banner immediately.

If SWO doesn't work for you, you can disable prints quickly by editing `Inc/debug.h`:

```c
#define DBG(fmt, ...)   ((void)0)
#define DBGE(fmt, ...)  ((void)0)
```

## Pin map

| Peripheral    | Pin(s) |
|---|---|
| SPI1 / RC522  | PA3=RST, PA4=SS, PA5=SCK, PA6=MISO, PA7=MOSI |
| I2C1 / LCD    | PB8=SCL, PB9=SDA (5 V LCD, 3.3 V I2C is fine) |
| Keypad rows   | PB0, PB1, PB2, PB10 (output) |
| Keypad cols   | PB12..PB15 (input, internal pull-up) |
| Buzzer        | PC6 |
| Green LED     | PC0 (through 220 ohm to GND) |
| Red LED       | PD0 (through 220 ohm to GND) |
| Tamper button | PA0 to GND (internal pull-up, EXTI falling edge) |

Only one green + one red LED are driven. To light multiple LEDs per color, wire them in parallel, each with its own 220 ohm series resistor, to the same GPIO.

## Keypad layout assumed

```
1 2 3 A
4 5 6 B
7 8 9 C
* 0 # D
```

**PIN entry:** digits enter the PIN (masked as `*`), `#` submits, `*` is backspace, `D` cancels.

**At idle:**
- Tap `A` -> master-PIN-only emergency entry path.
- Hold `#` for 2 s -> admin mode (requires master PIN to enter).

**In admin menu:** `A`=enroll, `B`=delete, `C`=change master PIN, `D`=exit.

## Defaults (in `Src/storage.c`)

- Master PIN: `9999`
- Duress PIN: `0000` (appears to unlock but flags a silent alarm)
- Per-card PIN (set on enrollment): `1234`
- One demo card `DE AD BE EF` is pre-enrolled so you can test the flow before you scan a real card.

## LCD I2C address

Typical PCF8574 backpack is at `0x27`; some are `0x3F`. On boot, the debug log tells you whether ACK was received. If not, edit `#define LCD_I2C_ADDR 0x27` in `Inc/lcd_i2c.h` to `0x3F`.

## Flow summary

1. Idle: red LED on, LCD shows "Scan card..."
2. Scan known card -> LCD prompts PIN -> enter 4 digits + `#`.
3. Correct PIN -> green LED, 2-tone beep, 5-second unlock with on-screen countdown.
4. Wrong PIN / unknown card / bad master -> red blink + low tone; 3 wrong tries in a row -> 30-second lockout countdown + rapid beeping.
5. Tamper button press at any time -> 8-second audible alarm.
6. Duress PIN -> grants access silently while logging an alarm (route this to a radio/GSM in a real build).

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| LCD stays blank | Wrong I2C address (try `0x3F`), missing external 4.7 k pull-ups on SDA/SCL, 5 V vs 3.3 V mismatch on VCC. |
| `RC522 Version: 0x00` or `0xFF` | SPI wiring off, RST not pulsed, RC522 not powered at 3.3 V. Make sure MOSI/MISO aren't swapped. |
| Keypad reads ghost keys | Rows/cols swapped, cols missing pull-ups (we enable internal ones; a 10 k external is OK). |
| Tamper alarm triggers on boot | PA0 floating (make sure the button actually pulls to GND) or noisy wire; add a 100 nF debounce cap. |
| No printf output | SWO not enabled in Keil Trace settings, or Core Clock is wrong (must be 16 MHz unless you add a PLL setup). |

## What's intentionally *not* included

- Persistent storage (flash). Enrollments vanish on reset. For a real build, write `g_cfg` to an internal-flash sector.
- Hardware timer for buzzer patterns (we use blocking delays).
- A relay/solenoid driver. Drive one from the green LED pin through a transistor + flyback diode.
