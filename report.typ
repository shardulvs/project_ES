#set document(
  title: "Secure Door Lock System with RFID Authentication, PIN Keypad, and Tamper Detection",
  author: "Digital Systems Lab",
)

#set page(
  paper: "a4",
  margin: (x: 2.2cm, y: 2.4cm),
  numbering: "1",
  number-align: center,
)

#set text(font: "New Computer Modern", size: 11pt, lang: "en")
#set par(justify: true, leading: 0.65em)
#set heading(numbering: "1.1")

#show heading.where(level: 1): it => {
  set text(size: 16pt, weight: "bold")
  v(0.6em)
  it
  v(0.3em)
}
#show heading.where(level: 2): it => {
  set text(size: 13pt, weight: "bold")
  v(0.4em)
  it
  v(0.2em)
}

#show raw.where(block: true): it => block(
  fill: luma(245),
  inset: 8pt,
  radius: 4pt,
  width: 100%,
  text(size: 9pt, it),
)
#show raw.where(block: false): it => box(
  fill: luma(240),
  inset: (x: 3pt, y: 0pt),
  outset: (y: 3pt),
  radius: 2pt,
  it,
)

// ──────────────────────────────── Title page
#align(center)[
  #v(3cm)
  #text(20pt, weight: "bold")[
    Secure Door Lock System with RFID\
    Authentication, PIN Keypad, and\
    Tamper Detection
  ]

  #v(0.8cm)
  #text(13pt)[Digital Systems Laboratory -- Technical Report]

  #v(1.5cm)
  #text(12pt)[
    Platform: STM32F429I-DISC1 · RC522 RFID Module \
    Toolchain: Keil uVision 5 + STM32Cube HAL
  ]

  #v(2cm)
  #text(11pt)[Date of submission: April 17, 2026]
]

#pagebreak()

// ──────────────────────────────── Abstract + TOC
#heading(numbering: none, outlined: false)[Abstract]

This report documents the design and implementation of a dual-factor electronic
door-lock controller built on the STM32F429I-DISC1 discovery kit. The system
uses the MFRC522 RFID reader for card-based identification over SPI and a
4#sym.times 4 matrix keypad for PIN entry, complemented by an I#super[2]C
character LCD, LED and buzzer feedback, and a hardware tamper input routed to
an external interrupt line. The firmware implements a finite-state machine
covering idle, authentication, granted, lockout, admin, enrolment, and alarm
states, and exposes administrator functions (card enrolment, deletion, and
PIN change) through a keypad-driven menu. Special attention is given to
defensive features: a three-attempt lockout with live countdown, a duress PIN
that silently signals a compromised entry, an unknown-card alert, and a
tamper-driven audible alarm. The document covers the security rationale, the
peripheral-interfacing design, the state machine, the source organisation,
build and debug setup, and a short verification plan.

#v(0.5em)
#outline(indent: auto, depth: 2)

#pagebreak()

// ──────────────────────────────── 1. Introduction
= Introduction

Physical access control is a canonical embedded-systems problem because it
forces the designer to integrate authentication logic with heterogeneous
peripherals under real-time constraints. A realistic lock must not only open
the door for valid users but also degrade gracefully under failure and
actively resist attack.

This project addresses the Digital Systems Laboratory brief
_"Secure Door Lock System: RFID authentication, PIN keypad, Tamper
detection using the RC522 RFID module"_ with the following goals:

- *Two-factor authentication.* Something-you-have (an RFID card) and
  something-you-know (a numeric PIN) must both be presented for an ordinary
  unlock.
- *Graceful failure.* Repeated wrong attempts trigger a timed lockout,
  communicated clearly to the user through the LCD, the LEDs, and the buzzer.
- *Active tamper response.* A physical push-button models a case-open switch
  and is wired to an external interrupt so that tamper detection is
  independent of the main program loop.
- *Administrator workflow.* An on-device menu allows the operator to enrol
  and remove cards and to change the master PIN, without recompiling the
  firmware.
- *Defensive-in-depth extras.* A master PIN enables emergency entry when a
  card is unavailable, a duress PIN silently signals a coerced entry, and an
  unknown-card event is announced on the LCD and logged over the debug link.

The remainder of this report is organised as follows. Section 2 specifies the
hardware bill of materials and the complete pin map.  Section 3 develops the
system architecture and software organisation.  Section 4 details the
peripheral drivers and the security-relevant behaviours.  Section 5 describes
the build, flashing, and debugging workflow in Keil uVision.  Section 6 lists
verification scenarios, and Section 7 discusses limitations and future work.

// ──────────────────────────────── 2. Hardware
= Hardware Platform

== Components

#table(
  columns: (auto, 1fr),
  inset: 6pt,
  align: left,
  stroke: 0.5pt + luma(160),
  [*Component*], [*Role*],
  [STM32F429I-DISC1], [Cortex-M4F host (180 MHz, 2 MiB flash, 256 KiB SRAM)],
  [RC522 RFID reader], [13.56 MHz ISO-14443A card reader, SPI interface],
  [16 #sym.times 2 I#super[2]C LCD (PCF8574 backpack)], [User feedback, menus, countdowns],
  [4 #sym.times 4 matrix keypad], [PIN entry and admin navigation],
  [Piezo buzzer module], [Audible feedback and alarm tones],
  [Push button], [Case-tamper input (connected to EXTI)],
  [5 #sym.times green + 5 #sym.times red LEDs], [Status indication; paralleled via 220 Ω resistors],
  [Breadboard and jumpers], [Prototype wiring],
)

== Pin Map

All wiring is summarised in the table below. Rows of the keypad are driven as
push-pull outputs and columns are read with the microcontroller's internal
pull-ups enabled. The tamper switch uses the falling-edge interrupt on
#raw("EXTI0"). The I#super[2]C LCD is powered from the 5 V rail because the
HD44780 controller will not latch data at 3.3 V logic levels reliably;
SDA/SCL are still driven at 3.3 V and relying on internal or external pull-ups
is sufficient because the line levels are open-drain.

#table(
  columns: (auto, auto, 1fr),
  inset: 6pt,
  align: left,
  stroke: 0.5pt + luma(160),
  [*Peripheral*], [*STM32 pin*], [*Notes*],
  [RC522 RST],  [PA3],  [Reset, push-pull output],
  [RC522 SDA/SS], [PA4], [Chip-select, push-pull output],
  [RC522 SCK],  [PA5],  [SPI1 AF5],
  [RC522 MISO], [PA6],  [SPI1 AF5],
  [RC522 MOSI], [PA7],  [SPI1 AF5],
  [Keypad Row 1--3], [PB0, PB1, PB2], [Push-pull outputs],
  [Keypad Row 4],   [PB10], [Push-pull output],
  [Keypad Col 1--4], [PB12--PB15], [Input, internal pull-up],
  [LCD SDA],    [PB9],  [I#super[2]C1 AF4, open-drain],
  [LCD SCL],    [PB8],  [I#super[2]C1 AF4, open-drain],
  [Buzzer +],   [PC6],  [Push-pull output],
  [Green LED],  [PC0],  [220 Ω series resistor to GND],
  [Red LED],    [PD0],  [220 Ω series resistor to GND],
  [Tamper button], [PA0], [Button to GND; internal pull-up, EXTI0 falling],
)

To use all five green LEDs and all five red LEDs in parallel, each LED gets
its own series resistor and they all share the single driving GPIO. The
firmware toggles one pin per colour; no code change is needed to scale to the
full bank.

// ──────────────────────────────── 3. Architecture
= System Architecture

== Block Diagram

The system partitions cleanly into three planes: sensing (RFID, keypad,
tamper), actuation (LEDs, buzzer, and a future solenoid driver), and
supervision (the Cortex-M4 running the state machine).

#align(center)[
  #box(
    stroke: 0.8pt,
    inset: 10pt,
    radius: 3pt,
    [
      #raw(
"                  +---------------------+
                  |  STM32F429I-DISC1   |
   SPI1 <-------->|  (RC522 driver)     |
  Matrix <------->|  (keypad scanner)   |
   EXTI0 ------>  |  (tamper ISR)       |
                  |  state machine      |
                  |                     |----> GPIO --> LEDs
                  |                     |----> GPIO --> Buzzer
                  |                     |<===> I2C1 --> LCD
                  |                     |----> SWO  --> ITM printf
                  +---------------------+"
      )
    ]
  )
]

== Source Organisation

The firmware is split into small, single-responsibility modules under #raw("Src/")
with matching headers in #raw("Inc/"). The split keeps the main application
under 400 lines and lets each driver be unit-checked in isolation.

#table(
  columns: (auto, 1fr),
  inset: 6pt,
  align: left,
  stroke: 0.5pt + luma(160),
  [*File*], [*Responsibility*],
  [#raw("main.c")],      [HAL / clock setup, state machine, UI flows],
  [#raw("rc522.c/.h")],  [SPI driver for the MFRC522 (request, anti-collision, UID, halt)],
  [#raw("lcd_i2c.c/.h")],[HD44780 over PCF8574 in 4-bit mode with probe-at-init],
  [#raw("keypad.c/.h")], [4 #sym.times 4 matrix scan with debounce and wait-for-release],
  [#raw("storage.c/.h")],[In-RAM configuration: master PIN, duress PIN, card table],
  [#raw("debug.c/.h")],  [#raw("printf") retargeted to ITM SWO, plus #raw("DBG") / #raw("DBGE") macros],
  [#raw("stm32f4xx_it.c")], [SysTick, fault handlers, and EXTI0 entry],
)

== State Machine

The top-level behaviour is a Moore machine with the following states.

#table(
  columns: (auto, 1fr),
  inset: 6pt,
  align: left,
  stroke: 0.5pt + luma(160),
  [*State*], [*Meaning*],
  [#raw("ST_IDLE")],       [Locked, waiting for a card or the #raw("A") hotkey],
  [#raw("ST_WAIT_PIN")],   [Card OK, prompting for the 4-digit PIN],
  [#raw("ST_GRANTED")],    [Unlocked for #raw("UNLOCK_MS"), countdown on LCD],
  [#raw("ST_DENIED")],     [Short red blink and low tone; increments fail counter],
  [#raw("ST_LOCKOUT")],    [Blocked for 30 s after 3 fails, countdown + rapid beep],
  [#raw("ST_ADMIN")],      [Admin menu (enrol / delete / change PIN / exit)],
  [#raw("ST_ENROLL")],     [Wait for a card to register, pulse green LED],
  [#raw("ST_DELETE")],     [Wait for a card to remove from the whitelist],
  [#raw("ST_CHANGE_PIN")], [Prompt for new master PIN twice and confirm],
  [#raw("ST_ALARM")],      [8-second strobe + siren on tamper or duress event],
)

Transitions are event-driven. Card reads, key presses, and the tamper flag
(set inside the EXTI callback) are all polled in the main loop; only tamper
uses an ISR so that a reaction is guaranteed even while the loop is blocked
inside an LCD write.

// ──────────────────────────────── 4. Drivers & Features
= Driver and Feature Walkthrough

== RC522 RFID Driver

The MFRC522 is driven over SPI1 at approximately 2.8 MHz (#raw("PCLK2")/32).
Each register access follows the datasheet format: the first byte is
#raw("(addr << 1) & 0x7E") for writes or OR-ed with #raw("0x80") for reads,
and a second byte carries the data. The driver exposes:

- #raw("RC522_Init") — hardware reset pulse, soft reset, antenna on,
  CRC preset #raw("0x6363"), and timer auto-reload so that transceive
  commands have a deterministic timeout;
- #raw("RC522_Request") — issues #raw("PICC_REQIDL") and expects a
  16-bit ATQA response;
- #raw("RC522_Anticoll") — runs the cascade level 1 anti-collision
  loop and validates the BCC against the four UID bytes;
- #raw("RC522_ReadUID") — wraps the above and halts the card so that
  repeated presentations are detected as separate events.

At boot the driver reads and logs the version register. Values of
#raw("0x91") or #raw("0x92") indicate a real chip, while #raw("0x00") or
#raw("0xFF") strongly suggest a SPI wiring problem.

== I#super[2]C LCD Driver

The PCF8574 I/O expander on the LCD backpack follows the de-facto pinout
P0=RS, P1=RW, P2=EN, P3=Backlight, P4--P7=D4--D7. The driver performs the
standard HD44780 4-bit initialisation sequence after a 50 ms power-up wait,
then exposes #raw("LCD_Clear"), #raw("LCD_PrintAt"), #raw("LCD_Printf"),
and a backlight control. The initialiser probes the slave address with
#raw("HAL_I2C_IsDeviceReady") and logs the result so that the very common
#raw("0x27") / #raw("0x3F") confusion is easy to spot.

== Keypad

Rows are driven low one at a time while the columns are read. On a detected
low column the code waits 20 ms for debounce and then waits for the key to
release before returning, eliminating chatter and preventing a single press
from registering repeatedly. Every key press is also printed over SWO with
the resolved ASCII character and the row/column indices, which has been
invaluable for catching wiring swaps.

The keypad layout assumed is:

#align(center)[
  #table(
    columns: (auto, auto, auto, auto),
    inset: 8pt,
    align: center,
    [1], [2], [3], [A],
    [4], [5], [6], [B],
    [7], [8], [9], [C],
    [\*], [0], [\#], [D],
  )
]

During PIN entry the digits are masked on the LCD as #raw("*"), #raw("#")
submits, #raw("*") deletes, and #raw("D") cancels.

== Security-Relevant Behaviours

- *Dual-factor authentication.* A card read that succeeds still transitions
  the state machine into #raw("ST_WAIT_PIN"); access is granted only when
  both the card is whitelisted and a matching PIN is submitted before the
  10-second per-key timeout expires.
- *Three-strike lockout.* The failure counter increments on any authentication
  failure (unknown card, bad PIN, PIN timeout). On reaching three, the system
  enters #raw("ST_LOCKOUT") for 30 s with a live countdown on the LCD, a
  rapid red blink, and a six-beep alarm burst.
- *Duress PIN.* A second, distinct PIN (default #raw("0000")) grants entry
  but transitions through a silent alarm code path (visually identical to a
  normal unlock). In a production deployment this would trigger a radio or
  GSM notification; in the lab build it toggles a dedicated debug log line.
- *Master PIN override.* The master PIN (default #raw("9999")) unlocks the
  door even without a card, intended as a recovery path. It is also the
  passphrase for the admin menu.
- *Hardware tamper.* The push-button is wired to #raw("PA0") with the
  internal pull-up enabled and is configured for a falling-edge interrupt.
  The ISR sets a volatile flag that the main loop services at its next
  iteration by entering #raw("ST_ALARM"), which strobes the red LED and
  drives the buzzer for eight seconds. Because the handler is an ISR, the
  alarm fires even if the application is busy waiting for an LCD transaction.
- *Unknown-card alert.* A card UID that is not in the whitelist is
  displayed on the LCD in hexadecimal so that the operator can see whether
  the reader is functioning and which card was presented.
- *Auto-relock.* The unlock window is a 5-second timer; on expiry the state
  returns to #raw("ST_IDLE") regardless of whether a physical door sensor
  has been installed.

== Admin Menu

Holding the #raw("#") key for two seconds at idle, followed by the master
PIN, enters the admin menu. Both LEDs blink alternately to distinguish this
mode visually. Menu keys:

- #raw("A") — enrol: pulse green while waiting for a card, add to
  the first free slot of the whitelist, confirm on LCD;
- #raw("B") — delete: scan a card and remove it from the whitelist;
- #raw("C") — change master PIN: prompt, confirm, and update;
- #raw("D") — exit back to idle.

The whitelist lives in RAM for the scope of this lab. Porting it to the
on-chip flash (F429 sector 11, 128 KiB) requires only a unlock / erase /
program cycle in #raw("storage.c") and is called out as a future-work item.

// ──────────────────────────────── 5. Build & Debug
= Build, Flashing, and Debugging

== Keil uVision Setup

The project targets the #raw("STM32F429I") device. Under the Manage
Run-Time Environment dialog we enable CMSIS Core, Device Startup, and the
STM32Cube HAL driver modules for Common, Cortex, GPIO, RCC, PWR, SPI, and
I#super[2]C. The preprocessor symbols #raw("STM32F429xx") and
#raw("USE_HAL_DRIVER") are added to the project defines, MicroLIB is
enabled, and all files in #raw("Src/") are added to the Source Group with
#raw("Inc/") on the include path.

The clock tree is configured manually in #raw("SystemClock_Config"): the
external 8 MHz HSE feeds the PLL (#raw("M=8"), #raw("N=360"), #raw("P=2"))
to produce 180 MHz #raw("SYSCLK"), with over-drive enabled and flash latency
set to five wait states. APB1 runs at 45 MHz, APB2 at 90 MHz, giving a
#raw("PCLK2")/32 SPI baud of roughly 2.8 MHz — well within the
RC522's 10 MHz limit but slow enough to be tolerant of breadboard wiring.

== ITM SWO Debug Output

The #raw("DBG") and #raw("DBGE") macros send timestamped log lines to ITM
port 0 over the SWO pin. In Keil, open
#emph[Debug→ Settings→ Trace], enable Trace, set the Core
Clock to 180 MHz, and tick bit 0 under ITM Stimulus Ports. The output then
appears in #emph[View→ Serial Windows→ Debug (printf)
Viewer]. A representative boot log looks like:

```
[1]    === Secure Door Lock booting ===
[3]    SystemCoreClock = 180000000 Hz
[9]    LCD found at 0x27
[12]   LCD init done
[14]   Keypad init done
[17]   Storage reset to defaults. Master=9999 Duress=0000
[70]   RC522 Version: 0x92
[72]   Ready. Max cards=8, rfid_only=0
```

== First-Light Checklist

1. RC522 version prints as #raw("0x91") or #raw("0x92"). If it is
   #raw("0x00") or #raw("0xFF"), swap MISO and MOSI or check the 3.3 V
   supply current (the module pulls ~100 mA spikes).
2. LCD address probe succeeds. If it does not, try changing the macro to
   #raw("0x3F") and rebuild.
3. Pressing any key emits a #raw("[t] Key: X (r=R c=C)") line. If the row
   and column indices look wrong, the matrix wires are swapped.
4. Pushing the tamper button fires the alarm immediately, even while
   another event is on screen — this confirms that EXTI0 is wired
   to #raw("PA0") and that interrupts are enabled.

// ──────────────────────────────── 6. Verification
= Verification

The following scenarios are exercised manually. Each is tagged with the
expected LCD output and LED / buzzer response.

#table(
  columns: (auto, 1fr, 1fr),
  inset: 6pt,
  align: left,
  stroke: 0.5pt + luma(160),
  [*\#*], [*Scenario*], [*Expected outcome*],
  [1], [Known card + correct PIN], [Two-tone chime, green LED on, LCD "ACCESS GRANTED", 5 s countdown, relock],
  [2], [Known card + wrong PIN #sym.times 1], [Low tone, red triple-blink, LCD "ACCESS DENIED / Bad PIN", return to idle],
  [3], [Three consecutive fails], [Enter #raw("ST_LOCKOUT"), LCD countdown, rapid beep burst, auto-clear after 30 s],
  [4], [Unknown card], [LCD "UNKNOWN CARD" with UID, fail-counter increments],
  [5], [Card + master PIN], [Grant access, LCD shows card UID on line 2],
  [6], [Press #raw("A") at idle, enter master PIN], [Grant access with "Master PIN" on LCD],
  [7], [Card + duress PIN], [Appears to grant access; debug log marks duress],
  [8], [Tamper button during idle or countdown], [Immediate 8 s alarm, returns to idle when clear],
  [9], [Hold #raw("#") 2 s + master PIN, press #raw("A")], [Enter admin enrol, scan new card, LCD "Enrolled!"],
  [10], [Admin menu, press #raw("B"), scan existing card], [LCD "Deleted", card removed from whitelist],
)

// ──────────────────────────────── 7. Future work
= Limitations and Future Work

- *Non-volatile storage.* The whitelist and PINs live in RAM, so enrolments
  do not survive a reset. Writing the #raw("Config") struct to the last
  flash sector on every change closes this gap.
- *Physical actuator.* The green LED is a stand-in for a real door
  actuator. Replacing it with a MOSFET-driven 12 V solenoid and a
  back-EMF flyback diode turns the project into a deployable lock.
- *Cryptographic card auth.* The current design authenticates on the UID
  only, which is cloneable. Using the MFRC522's 3-pass mutual
  authentication with per-sector keys on MIFARE Classic cards raises the
  bar substantially.
- *Door sensor.* A reed switch on the door frame would distinguish
  "unlocked but not opened" from "unlocked and opened", enabling proper
  door-left-open alerts and more aggressive auto-relock.
- *Secure transport for alarms.* In a fielded system the duress PIN
  should page a remote monitor over GSM or LoRa rather than merely logging
  to the debug port.
- *Brown-out robustness.* Enabling the STM32 brown-out reset at 2.7 V
  prevents flash corruption during power glitches once persistence is
  added.

// ──────────────────────────────── 8. Conclusion
= Conclusion

The project meets the lab brief on both axes: it demonstrates concrete
peripheral-interfacing competence (SPI, I#super[2]C, matrix keypad, GPIO
output, and an external interrupt) and it applies elementary security design
principles (two-factor authentication, rate limiting, duress handling, and
active tamper response). The code base is modular, commented sparingly where
behaviour is not self-evident, and instrumented throughout with timestamped
debug prints so that the inevitable breadboarding mishaps surface in
seconds rather than minutes. Extension to non-volatile storage and a
mechanical actuator is straightforward and is described above.

#v(1cm)
#align(center)[
  #text(9pt, style: "italic")[
    Source tree layout: #raw("Inc/"), #raw("Src/"),
    #raw("README.md"), #raw("report.typ"). Compiled PDF is not checked in.
  ]
]
