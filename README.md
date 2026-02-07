# tdeck-pro

> **!! WORK IN PROGRESS !!**
> This is an active development project. Things will break, APIs will change, and features are incomplete. Contributions and bug reports welcome.

A clean, hackable firmware for the **LilyGo T-Deck Pro** (ESP32-S3 + e-ink display). Meant to be an easy development platform for your T-Deck Pro projects — fork it, gut what you don't need, plug your own apps into the grid, and go.

The goal is a minimal but functional base layer: home screen, settings, hardware toggles, WiFi management, and keyboard input — so you can focus on building your actual project instead of fighting with init sequences and pin conflicts.

---

## Hardware

| Component | Chip / Module | Interface | Notes |
|---|---|---|---|
| MCU | ESP32-S3 | - | PSRAM enabled, USB CDC |
| Display | GDEQ031T10 (3.1" e-ink) | SPI | 240x320 BW, GxEPD2 driver |
| Keyboard | TCA8418 | I2C (0x34) | 4x10 matrix, BB Q10 layout |
| Touch | CST328 | I2C + GPIO IRQ | Uses factory hyn_touch driver |
| LoRa | SX1262 | SPI (shared bus) | Enable pin controlled |
| GPS | L76K | UART | Enable pin controlled |
| Gyroscope | QMI8658 | I2C | 1.8V rail controlled |
| WiFi/BT | ESP32-S3 built-in | - | Software controlled |
| SD Card | - | SPI (shared bus) | CS must be HIGH during other SPI |
| KB Backlight | LED on GPIO 42 | PWM | 5-step brightness |

## Pin Map

```
SPI Bus
  SCK   = 36        MOSI  = 33

E-Ink Display
  CS    = 34        DC    = 35
  BUSY  = 37        RST   = -1 (not connected)

LoRa Radio
  CS    = 3         RST   = 4
  EN    = 46        (HIGH = on, LOW = off)

SD Card
  CS    = 48

I2C Bus
  SDA   = 13        SCL   = 14

Keyboard
  I2C addr = 0x34   LED   = 42 (PWM backlight)

Touch
  SDA   = 13        SCL   = 14 (shared I2C)
  RST   = 45        INT   = 12

Enable Pins
  GPS   = 39        (HIGH = on, LOW = off)
  1V8   = 38        (HIGH = on, LOW = off — powers gyroscope)
```

**Important:** LoRa, SD, and EPD share the SPI bus. All CS lines must be pulled HIGH before initializing any device, or you'll get bus conflicts.

## Building

### Requirements

- [Arduino CLI](https://arduino.github.io/arduino-cli/) (or Arduino IDE)
- ESP32 Arduino core **2.0.17** (NOT 3.x — GxEPD2 has compatibility issues with 3.x)
- Libraries: `GxEPD2`, `Adafruit TCA8418`, `Adafruit GFX`, `Adafruit BusIO`

### Install (CLI)

```bash
# Install ESP32 core
arduino-cli core install esp32:esp32@2.0.17

# Install libraries
arduino-cli lib install "GxEPD2" "Adafruit TCA8418" "Adafruit GFX Library" "Adafruit BusIO"
```

### Compile & Flash

```bash
# Compile
arduino-cli compile -b esp32:esp32:esp32s3 \
  --build-property "compiler.cpp.extra_flags=-DBOARD_HAS_PSRAM -DARDUINO_USB_CDC_ON_BOOT=1" \
  ./

# Flash (adjust port as needed)
arduino-cli upload -b esp32:esp32:esp32s3 -p /dev/ttyACM0 ./
```

**Note on build flags:** Use `compiler.cpp.extra_flags`, not `build.extra_flags`. The latter overwrites the built-in `-DESP32` define and breaks the build.

---

## Architecture

### Screen System

The firmware uses a simple state machine for screens:

```cpp
enum Screen { SCREEN_HOME, SCREEN_SETTINGS, SCREEN_WIFI_POPUP, SCREEN_WIFI_PASSWORD };
Screen currentScreen = SCREEN_HOME;
```

Each screen has a `draw*()` function for rendering and a block in `handleKeyboard()` for input. Navigation flows:

```
HOME  --[Enter]--> App (Settings, etc.)
  |                  |
  |                  '--[Backspace]--> HOME
  |
  '--[Alt + key]--> App shortcut
```

### Adding Your Own App

1. **Add to the enum:**
```cpp
enum Screen { SCREEN_HOME, SCREEN_SETTINGS, ..., SCREEN_MYAPP };
```

2. **Register on the home grid** (slots 1-7 are empty):
```cpp
const char* appNames[ICON_COUNT] = {
    "Settings", "", "MyApp", "", "", "", "", "", ""
};
const char appShortcuts[ICON_COUNT] = {
    's', 0, 'y', 0, 0, 0, 0, 0, 'm'   // Alt+Y opens MyApp
};
```

3. **Add open handler:**
```cpp
void openApp(int index) {
    if (index == 0) { /* settings */ }
    else if (index == 2) {
        currentScreen = SCREEN_MYAPP;
        drawMyApp();
    }
}
```

4. **Add draw + input handler:**
```cpp
void drawMyApp() {
    display.setRotation(DISPLAY_ROTATION);
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        // your rendering here
    } while (display.nextPage());
}

// In handleKeyboard(), add:
} else if (currentScreen == SCREEN_MYAPP) {
    if (c == '\b') {
        currentScreen = SCREEN_HOME;
        delay(150); drawHomeScreen();
    }
    // your input handling here
}
```

### Drawing Conventions

- `display.setRotation(0)` = correct landscape orientation
- Top bar is 20px tall with a divider line at y=19
- Use `GxEPD_BLACK` and `GxEPD_WHITE` — it's a 1-bit display
- Full page refresh via `firstPage()`/`nextPage()` loop (GxEPD2 paging)
- Fonts available: `FreeSansBold9pt7b`, `FreeSans9pt7b`, `Picopixel`

### Reusable UI Helpers

```cpp
// Toggle switch widget (40x22px)
drawToggle(x, y, isOn, foregroundColor);

// Value selector with < > arrows
drawSelector("75%", textY, hasLeft, hasRight, foregroundColor);
```

---

## Peripheral Interfaces

### Keyboard

The TCA8418 is a 4x10 matrix scanner. Key events come in as press/release with matrix position encoded. The firmware maps positions to characters:

```
     q  w  e  r  t  y  u  i  o  p
     a  s  d  f  g  h  j  k  l  [BKSP]
 [ALT]  z  x  c  v  b  n  m  $  [ENTER]
[LSFT]  [MIC] [--SPACE--] [SYM] [RSFT]
```

**Shift (held):**
```
     Q  W  E  R  T  Y  U  I  O  P
     A  S  D  F  G  H  J  K  L  [BKSP]
 [ALT]  Z  X  C  V  B  N  M  $  [ENTER]
[LSFT]  [MIC] [--SPACE--] [SYM] [RSFT]
```

**SYM (held):**
```
     #  1  2  3  (  )  _  -  +  @
     *  4  5  6  /  :  ;  '  "  [BKSP]
 [ALT]  7  8  9  ?  |  ,  .  $  [ENTER]
[LSFT]   0    [--SPACE--] [SYM] [RSFT]
```

**Reading keys:**
```cpp
if (keypad.available() > 0) {
    int k = keypad.getEvent();
    if (k & 0x80) {  // press event only
        k = (k & 0x7F) - 1;
        int row = k / KB_COLS;
        int col = (KB_COLS - 1) - k % KB_COLS;  // columns are reversed
        char c = keymap[row][col];
    }
}
```

**SYM key** (number input): press SYM then a letter to get its number.

| Key | q | w | e | r | t | y | u | i | o | p |
|-----|---|---|---|---|---|---|---|---|---|---|
| SYM | # | 1 | 2 | 3 | ( | ) | _ | - | + | @ |

| Key | a | s | d | f | g | h | j | k | l |
|-----|---|---|---|---|---|---|---|---|---|
| SYM | * | 4 | 5 | 6 | / | : | ; | ' | " |

| Key | z | x | c | v | b | n | m |
|-----|---|---|---|---|---|---|---|
| SYM | 7 | 8 | 9 | ? | \| | , | . |

The `0` is accessed via SYM + MIC (the key left of spacebar). `$` is a dedicated key on row 2.

**SHIFT key:** toggles next letter to uppercase. Used in text input screens.

**ALT key:** on home screen, activates shortcut mode — press Alt then a letter to jump to an app. The first letter of each app name is underlined while Alt is active.

### KB Backlight

GPIO 42, PWM controlled via `analogWrite()`:

```cpp
analogWrite(BOARD_KB_LED, brightness);  // 0-255
```

The firmware provides 5 steps: Off, 25% (64), 50% (128), 75% (192), 100% (255). Auto-off timer is configurable (2-128 seconds).

### Display (E-Ink)

GxEPD2 with paged drawing. The display object is global:

```cpp
// Full screen redraw
display.setRotation(DISPLAY_ROTATION);
display.setFullWindow();
display.firstPage();
do {
    display.fillScreen(GxEPD_WHITE);
    // all your drawing calls here
} while (display.nextPage());
```

Resolution is 240x320 in the rotation used (landscape). E-ink refreshes are slow (~1-2s for full), so minimize redraws.

### Touch (CST328)

Uses the factory `hyn_touch` driver (ESP-IDF I2C, not Arduino Wire). The driver files (`hyn_*.c`, `hyn_*.h`) are included in the project.

```cpp
extern "C" {
    int hyn_touch_init(void);           // returns 1 on success
    uint8_t hyn_touch_get_point(int16_t *x, int16_t *y, uint8_t max_points);
}

// Poll in loop:
int16_t tx, ty;
uint8_t touched = hyn_touch_get_point(&tx, &ty, 1);
if (touched) {
    // tx, ty are screen coordinates
}
```

> **See known issues below** — touch is currently non-functional.

### WiFi

Standard ESP32 WiFi API. The firmware manages enable/disable state:

```cpp
WiFi.mode(WIFI_STA);         // enable
WiFi.mode(WIFI_OFF);         // disable

WiFi.scanNetworks();          // blocking scan
WiFi.begin(ssid, password);   // connect
WiFi.status();                // check WL_CONNECTED
```

Credentials are saved to NVS (ESP32 Preferences library) under namespace `"wifi"`. Up to 5 networks stored, oldest evicted when full.

### Bluetooth

```cpp
btStart();    // enable
btStop();     // disable
```

No BT functionality is implemented yet — just the power toggle. You'd add your own BLE/Classic stack on top.

### LoRa (SX1262)

Directly controlled via enable pin. The LoRa radio shares the SPI bus with the display and SD card.

```cpp
digitalWrite(BOARD_LORA_EN, HIGH);  // power on
digitalWrite(BOARD_LORA_EN, LOW);   // power off
```

No LoRa stack is implemented — just power management. For custom LoRa, you'd init the SX1262 on the shared SPI bus (CS = pin 3, RST = pin 4) after pulling the enable pin HIGH.

### GPS (L76K)

```cpp
digitalWrite(BOARD_GPS_EN, HIGH);   // power on
digitalWrite(BOARD_GPS_EN, LOW);    // power off
```

GPS communicates via UART (not yet initialized in this firmware). You'd add `Serial1.begin(9600, ...)` with the appropriate RX/TX pins and parse NMEA.

### Gyroscope (QMI8658)

Powered by the 1.8V rail:

```cpp
digitalWrite(BOARD_1V8_EN, HIGH);   // power on
digitalWrite(BOARD_1V8_EN, LOW);    // power off
```

Communicates over I2C (shared bus). No driver initialized — you'd add the QMI8658 library and read from the shared Wire bus.

---

## Navigation Reference

| Context | Key | Action |
|---|---|---|
| **Home** | W/A/S/D | Move selection in grid |
| | Enter | Open selected app |
| | Alt | Toggle shortcut mode |
| | Alt + letter | Jump to app by first letter |
| **Settings** | W/S | Navigate items |
| | A/D | Adjust value / toggle off-on |
| | Enter | Open sub-menu (WiFi) |
| | Backspace | Back to home |
| **WiFi Popup** | W/S | Navigate network list |
| | Enter | Connect (saved/open) or enter password |
| | R | Rescan networks |
| | Backspace | Back to settings |
| **WiFi Password** | Type | Enter password characters |
| | SYM + key | Type number (see table above) |
| | Shift | Toggle next letter uppercase |
| | Backspace | Delete last character |
| | Alt + Backspace | Cancel, back to network list |
| | Enter | Connect |

---

## Devlog / Known Issues

### Touchscreen — NOT WORKING

The CST328 touch controller initializes without error (`hyn_touch_init()` returns success), but `hyn_touch_get_point()` never returns touch data. Multiple approaches tried:

1. **SensorLib / TouchDrvCSTXXX** — the "official" Arduino touch library. `touch.begin()` is literally commented out in LilyGo's own factory firmware (line 492 of `factory.ino`). It does not work with this hardware.
2. **Factory hyn_touch driver** — copied all driver files (`hyn_i2c.c`, `hyn_touch.cpp`, `hyn_cst3xx.c`, `hyn_cst66xx.c`, `hyn_cst226se.c`, `hyn_ts_ext.c`, `hyn_core.h`, `hyn_cfg.h`). The driver uses ESP-IDF I2C directly (not Arduino Wire). Modified `hyn_i2c.c` to tolerate the I2C driver already being installed by Wire. Init succeeds, but no touch events fire.
3. **Init order sensitivity** — putting touch init before keyboard init breaks the keyboard entirely (no input at all). Current order: keyboard first, touch second.

The touch toggle exists in settings but currently has no effect since touch never reports points. The `touchReady` flag is set based on init return value.

**If you're investigating:** the interrupt pin is GPIO 12, configured for falling edge. The ISR sets a flag and queues to a FreeRTOS queue. The issue might be I2C bus contention between Wire (keyboard) and the ESP-IDF I2C driver (touch), or the interrupt never firing. Serial debug output has been unreliable (often nothing appears on the USB CDC serial).

### Keyboard Mapping — MOSTLY BROKEN

The letter layout (rows 0-1) and basic navigation work. Row 2 and 3 physical layout has been identified but matrix-to-physical mapping is best-effort and may be wrong. Known issues:

- **SYM key** — full layout now mapped (numbers, symbols, punctuation). See keyboard reference above for the complete table.
- **Row 3 matrix positions** — the 5 physical keys (LShift, Mic/0, Space, Sym, RShift) map to 10 matrix columns. Space spans multiple columns. The exact column assignments for SYM and the two shift keys are best guesses based on the factory firmware — if a key produces the wrong character, the matrix column mapping needs adjusting.
- **`-` and `*`** — accessible via SYM+I and SYM+R row 3 standalone mapping was incorrect.
- **`$` is a standalone key** — row 2 position 8 produces a literal `$` character (not a modifier).
- **Mic/Mute button** — produces `'0'` only when SYM is active. Without SYM it does nothing yet — could be repurposed as a function key (mute toggle, etc.).
- **Column reversal** — the TCA8418 column order is electrically reversed from physical layout. The firmware corrects with `col = (KB_COLS - 1) - k % KB_COLS`. If you're getting wrong characters, check that this reversal matches your hardware revision.
- **SYM only works in WiFi password screen** — extending it to other text input screens is straightforward (check `symNext` and look up `symMap[]`), but it's not wired up globally yet.

If you have the device in hand and can map out the full SYM layout from the keycap labels, PRs welcome.

### E-Ink Refresh Speed

Full page redraws take ~1-2 seconds. The firmware does full refreshes for every screen change. Partial refresh support exists in GxEPD2 but is not used — it could improve responsiveness for small updates (e.g., moving a highlight bar) at the cost of ghosting.

### Serial Debug Output

USB CDC serial (`Serial.begin(115200)`) is initialized but output is unreliable. Sometimes nothing appears after boot. This may be related to the `ARDUINO_USB_CDC_ON_BOOT` flag or timing. If you need reliable debug output, try adding a delay after `Serial.begin()` or using `Serial.flush()`.

### Not Yet Implemented

- **LoRa** — enable pin works, no radio stack.
- **GPS** — enable pin works, UART not initialized, no NMEA parsing.
- **Gyroscope** — 1.8V rail toggle works, no QMI8658 driver.
- **Bluetooth** — `btStart()`/`btStop()` works, no BLE/Classic services.
- **SD Card** — CS pin managed (held HIGH), not initialized or mounted.
- **App slots 2-8** — empty grid positions, ready for your projects.
- **Persistent settings** — only WiFi credentials are saved to NVS. Other settings (brightness, toggles) reset on reboot.
- **Battery monitoring** — not implemented.
- **Deep sleep / power management** — not implemented.

---

## License

Do whatever you want with this. No warranty, no guarantees, no support obligations.
