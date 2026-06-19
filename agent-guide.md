# AI Agent Guide: M5Stack Cardputer Development

Practical reference for AI agents working on Cardputer firmware projects.
Everything here was discovered hands-on — read this before touching the code.

---

## Hardware at a glance

| Property | Value |
|---|---|
| Chip | ESP32-S3 |
| Flash | ~8 MB (3.3 MB app partition) |
| RAM | 320 KB internal + 2 MB PSRAM |
| Display | 240×135 TFT (landscape, rotation 1) |
| USB | Native USB CDC (not a UART) |
| SD slot | SPI, FAT32 |

---

## PlatformIO setup

### `platformio.ini` minimum

```ini
[env:m5stack-stamps3]
platform = espressif32
board = m5stack-stamps3
framework = arduino
lib_deps =
    m5stack/M5Cardputer@^1.1.1

monitor_speed = 115200

build_flags =
    -D ARDUINO_USB_MODE=1
    -D ARDUINO_USB_CDC_ON_BOOT=1

upload_port = /dev/cu.usbmodem2101
```

- **Board name is `m5stack-stamps3`** — not `m5stack-cardputer` or anything else.
- `ARDUINO_USB_MODE=1` + `ARDUINO_USB_CDC_ON_BOOT=1` are both mandatory. Without them, flashing still works but the serial monitor won't receive output without a manual reset.
- `monitor_speed` is technically irrelevant (USB CDC baud is virtual) but must be present or PlatformIO will warn.
- `upload_port` pattern is `/dev/cu.usbmodem*` on macOS; `/dev/ttyACM*` on Linux.

### PlatformIO CLI path

`pio` is not on the default PATH. Use:

```sh
~/.platformio/penv/bin/pio run
~/.platformio/penv/bin/pio run -t upload
~/.platformio/penv/bin/pio device monitor
```

Or activate the virtualenv once: `source ~/.platformio/penv/bin/activate`.

---

## M5Cardputer library

### Initialisation

```cpp
#include "M5Cardputer.h"

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);  // true = enable keyboard
}

void loop() {
    M5Cardputer.update();  // must call every iteration
    // ...
}
```

**`M5Cardputer.update()` must be called every loop** — it drives the keyboard state machine, USB stack, and power management.

### Keyboard

```cpp
if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
    auto st = M5Cardputer.Keyboard.keysState();

    // Printable characters typed this frame
    for (char c : st.word) { /* c is the character */ }

    // Special keys
    st.del;    // backspace/delete
    st.enter;  // enter/return
    st.tab;    // tab
    st.fn;     // fn modifier held (use with other keys for chords)

    // Low-level: specific key held right now (not just this frame)
    M5Cardputer.Keyboard.isKeyPressed('a');
}
```

### Key chords (fn + key mapping)

The fn key activates a second layer. Confirmed chords:

| Physical key | fn + key produces | Typical use |
|---|---|---|
| `` ` `` | Escape / esc | back/cancel |
| `;` | Up arrow | menu up |
| `.` | Down arrow | menu down |
| `,` | Left arrow | — |
| `/` | Right arrow | — |

Detect chords by checking `st.fn && wordHas(st, '<key>')`:

```cpp
bool esc  = st.fn && wordHas(st, '`');
bool up   = st.fn && wordHas(st, ';');
bool down = st.fn && wordHas(st, '.');

// helper (wordHas is not in the library — define your own)
static bool wordHas(const Keyboard_Class::KeysState& st, char c) {
    for (auto k : st.word) if (k == c) return true;
    return false;
}
```

**When `fn` is held, `st.word` still contains the chord character** — filter with `if (!st.fn)` when accumulating typed text so you don't append `;`/`.`/etc. during navigation.

---

## Display

### Setup

```cpp
M5Cardputer.Display.setRotation(1);  // landscape — always set this
```

### Double-buffered rendering (no flicker)

Use an `M5Canvas` sprite — draw into it, then push in one blit:

```cpp
static M5Canvas canvas(&M5Cardputer.Display);

void setup() {
    // ...
    canvas.setColorDepth(8);       // 8-bit saves RAM vs 16-bit
    canvas.createSprite(M5Cardputer.Display.width(),
                        M5Cardputer.Display.height());
    canvas.setFont(&fonts::Font0); // NOT setTextFont — that is deprecated
    canvas.setTextSize(1);
    canvas.setTextDatum(top_left);
}

void loop() {
    canvas.fillSprite(TFT_BLACK);
    // ... draw ...
    canvas.pushSprite(0, 0);
}
```

### Font0 grid

`fonts::Font0` at size 1 is **6×8 px per character**:
- Columns: `display_width / 6` = 40 chars on a 240 px wide screen
- Rows: `display_height / 8` = 16 rows on a 135 px tall screen

This is the smallest readable monospace font. `fonts::Font2` (14px) is good for headings.

### Deprecated API

`setTextFont()` is deprecated — use `setFont()`:

```cpp
// WRONG (generates -Wdeprecated warnings)
canvas.setTextFont(&fonts::Font0);

// RIGHT
canvas.setFont(&fonts::Font0);
```

---

## SD card

### Pins (hardcoded on the Cardputer)

| Signal | GPIO |
|---|---|
| SCK | 40 |
| MISO | 39 |
| MOSI | 14 |
| CS | 12 |

### Mounting

```cpp
#include <SD.h>
#include <SPI.h>

static constexpr int SD_SCK = 40, SD_MISO = 39, SD_MOSI = 14, SD_CS = 12;

bool mountSD() {
    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    return SD.begin(SD_CS, SPI, 25000000);  // 25 MHz
}
```

`SD.begin()` registers the FAT partition at `/sd` in the ESP-IDF VFS, so all C `fopen`/`fread` calls (and anything built on them) automatically work:

```cpp
File f = SD.open("/yourfile.txt");  // SD library API
FILE* fp = fopen("/sd/yourfile.txt", "r");  // POSIX API — same file
```

Cards formatted as FAT32 work out of the box; exFAT requires an extra library.

### Hot-insert

A card inserted after boot isn't automatically detected. To support it, re-call `SD.begin()` each time the user enters the file browser. No need to `SPI.begin()` again.

---

## USB serial

### Why `pio device monitor` works but other monitors may not

The serial interface is **native USB CDC**, not a UART-over-USB chip (like CP2102). The host's DTR/RTS lines are wired into the ESP32-S3's reset/bootloader logic. Many serial monitors toggle DTR on connect and accidentally trigger a reset. Fix in your monitor: assert both DTR and RTS (don't leave them floating).

Requirements for any serial terminal:
- DTR and RTS both asserted (not toggled)
- Send a line ending (`\n` or `\r`) with Enter — the firmware needs it to execute a line
- Baud rate value is irrelevant (CDC is virtual); 115200 is the conventional choice

`pio device monitor` does all of this correctly by default.

---

## Loop task stack

The default Arduino loop task stack is **8 KB**, which is enough for simple programs but insufficient for anything recursive (parsers, interpreters, deep call stacks). Raise it globally with a single line anywhere in your source:

```cpp
SET_LOOP_TASK_STACK_SIZE(32 * 1024);
```

---

## Including C libraries in C++ (Arduino/ESP-IDF)

C headers without `extern "C"` guards will fail to compile from `.cpp` files. Wrap them:

```cpp
extern "C" {
#include "someclib.h"
}
```

If a C library has a `#define SOME_OPTION default_value` that you need to override from `build_flags`, check whether the define is guarded:

```c
// upstream code (won't respect -DSOME_OPTION=1 from the compiler)
#define SOME_OPTION 0

// patched version
#if !defined(SOME_OPTION)
#define SOME_OPTION 0
#endif
```

**Build flags in `lib/*/library.json` only apply to that library's own `.c` files.** If your application code (`src/`) needs to see the same define, put it in `platformio.ini` `build_flags` so it applies globally.

---

## Common pitfalls & fixes

| Symptom | Cause | Fix |
|---|---|---|
| Serial monitor shows nothing | ARDUINO_USB_CDC_ON_BOOT missing | Add to `build_flags` |
| Monitor disconnects/reboots on connect | Third-party terminal toggles DTR | Use `pio device monitor` or force DTR/RTS on |
| Flashing fails with "port busy" | Monitor still holding the port | Close monitor, then flash |
| Stack overflow / guru meditation | Loop task stack too small | `SET_LOOP_TASK_STACK_SIZE(32 * 1024)` |
| `fn` key inserts `;` `.` etc into text | Not filtering `st.fn` before appending `st.word` | Wrap printable accumulation in `if (!st.fn)` |
| C header compile errors from `.cpp` | Missing `extern "C"` | Wrap the `#include`s |
| `#define` in a library ignores build flag | Upstream uses plain `#define`, not `#ifndef` | Patch the header with `#if !defined(...)` guard |
| SD file not found | Path missing `/sd/` prefix | Use `/sd/filename.lua`, not `/filename.lua` |
| `setTextFont` deprecation warnings | Old API | Use `setFont()` instead |
| Display shows garbage or is sideways | Rotation not set | `Display.setRotation(1)` in setup |
