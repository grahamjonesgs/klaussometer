# Switching to Arduino-ESP32 3.x

The release build is Arduino-ESP32 2.x (`espressif32@6.10.0`, ESP-IDF 4.4). A working
3.x port (Arduino 3.3.6, ESP-IDF 5.5, pioarduino 55.03.36) is on the branch
**`arduino3-port`**. It was tested on the Waveshare board on 2026-10-28, and the
decision then was to stay on 2.x. This note covers what the port found and how to
pick it up again.

## Why it wasn't adopted (2026-10-28)

- **The display still tears during flash writes.** The main reason to move was the
  screen jumping during OTA. On 3.x it was just as bad: see
  [Fixing the tearing](#fixing-the-tearing) for the next step.
- **About 40 KB less free internal heap.** Measured on the Waveshare about a minute
  after boot:

  | Build | Free heap | Lowest |
  |---|---|---|
  | 2.x | 146 KB | 97 KB |
  | 3.x | 106 KB | 59 KB |

Everything else worked on 3.x:
- WiFi, NTP and MQTT, and every API fetch;
- SD save and restore, and the web log viewer;
- a web firmware upload (23 s, against 34–39 s on 2.x);
- a 10-minute soak with no resets or heap drift;
- the screen.

The Matouch board was never tried on 3.x.

## The boot crash was a build-config bug, not the chip

Every earlier 3.x attempt crashed at boot with `LoadProhibited` at `0x64` in
`systimer_hal_enable_alarm_int`. The README used to blame early ESP32-S3 silicon.
That was wrong: both boards are chip revision v0.2, and a blank sketch on the same
platform boots fine.

The cause was `lib_ignore = Time` in `platformio.ini`. pioarduino's component manager
deletes build-script lines that loosely match an ignored library name, and "Time"
matches `-u esp_timer_init_include_func` (because "timer" contains "time"). Without
that force-link, ESP-IDF's early timer initialisation is never linked in, so the
timer's hardware pointer is still NULL when ESP-IDF first uses it.

**Never use `lib_ignore = Time` (or any name matching an ESP-IDF symbol) with
pioarduino.**

## What's on the `arduino3-port` branch

- **`platformio-v3.ini`:** a standalone 3.x config. It uses the pioarduino platform
  URL and GFX `^1.6.5`, pins LVGL to `9.5.0`, and has no `lib_ignore`. It also drops
  the three `-DconfigGENERATE_RUN_TIME_STATS` / `-DconfigUSE_STATS_FORMATTING_FUNCTIONS`
  / `-DCONFIG_SPIRAM_USE_MALLOC` flags. Those only reach the Arduino core compiled
  from source, not the precompiled ESP-IDF.
- **`#if ESP_ARDUINO_VERSION_MAJOR >= 3` branches** for the API changes:
  - `ledcAttachChannel()` / `ledcWrite(pin, …)` for the Matouch backlight;
  - `RGB565_BLACK`;
  - `esp_task_wdt_reconfigure()` with an `esp_task_wdt_config_t`.
- **`APIs.h`** includes `<WiFiClientSecure.h>` rather than forward-declaring
  `class WiFiClientSecure`. On 3.x that name is a typedef of `NetworkClientSecure`.
- **`LCD_BOUNCE_BUFFER_LINES`** in `constants.h` passes ESP-IDF 5's RGB bounce buffer
  size through GFX. It's set to 0 (off): 10 lines cost 40 KB and dropped the lowest
  free heap to 19 KB, too little for TLS.
- **Fixes that apply to 2.x too**, worth taking whichever version is used:
  - **SD card:** a mounted flag, with a failed mount retried at most once a minute
    and logged once (no card used to mean a remount attempt and an error on every
    save and log line).
  - **Watchdog:** `logAndPublish` only feeds it from subscribed tasks.
  - **WiFi:** `WiFi.disconnect()` only once WiFi has started.
  - **MAC address:** read from eFuse with `esp_read_mac()` (`WiFi.macAddress()` is
    all zeros on 3.x before WiFi starts).
  - **Board detection:** the detected board is printed on the UART as well.

## How to switch

### 1. Get the branch up to date

Merge or rebase `arduino3-port` onto `main` and resolve any conflicts.

### 2. Keep pioarduino in its own PlatformIO core

pioarduino and the official `espressif32` platform both install a package called
`framework-arduinoespressif32`. Building one overwrites the other in a shared
`~/.platformio`. While both are needed, build 3.x with its own core directory:

```sh
PLATFORMIO_CORE_DIR=~/.platformio-arduino3 pio run -c platformio-v3.ini
```

The first build downloads the toolchain and framework (about 15 minutes). After
that, every build is a full rebuild of about 5–6 minutes.

If 3.x becomes the only build, move the contents of `platformio-v3.ini` into
`platformio.ini`. Do that only once no other project on the machine still uses
`espressif32@6.x`.

### 3. Flash

- **USB, first time on a board:**

  ```sh
  PLATFORMIO_CORE_DIR=~/.platformio-arduino3 pio run -c platformio-v3.ini -t upload --upload-port <port>
  ```

  This also writes the 3.x bootloader and partition table.
- **App only, faster:** don't use `-t nobuild`, which breaks pioarduino's upload
  arguments. Use esptool instead:

  ```sh
  ~/.platformio/penv/bin/python -m esptool --chip esp32s3 --port <port> --baud 921600 \
      write_flash 0x10000 .pio/build/klaussometer-v3/firmware.bin
  ```

- **OTA / web upload:** works. A 3.x app boots from the 2.x bootloader that an
  OTA-updated board keeps (tested).

### 4. Expect on first boot

- **SD restore failures for five blocks:** `solar_token`, `weather`, `uv`,
  `air_quality` and `solar_forecast` fail with "size N, struct expects N+4 bytes".
  `time_t` is 32-bit on 2.x and 64-bit on 3.x, so every persisted struct holding a
  timestamp grows by 4 bytes. The size check rejects the old file and the data is
  fetched again within about 20 seconds. The same happens going back to 2.x.

### 5. Build quirks

- **`TypeError: expected str, bytes or os.PathLike object, not NoneType`:** the first
  build after a library change sometimes fails with this. Run it again.
- **`src/idf_component.yml`:** pioarduino generates it (and a `.orig`) in the
  project. It's gitignored on the branch.
- **VS Code:** the PlatformIO extension sometimes empties `.pio/build`. Copy the
  firmware somewhere else before a long test.

## Fixing the tearing

On ESP-IDF 4.4 there is no fix. ESP-IDF 5 has three relevant features:

1. **`CONFIG_LCD_RGB_RESTART_IN_VSYNC`:** resyncs the panel every frame. It's already
   on in pioarduino's prebuilt configuration, and on its own it wasn't enough.
2. **Bounce buffers:** the panel is fed from internal RAM instead of DMA straight
   from PSRAM. Available through `LCD_BOUNCE_BUFFER_LINES`.
3. **`CONFIG_SPIRAM_XIP_FROM_PSRAM` + `CONFIG_LCD_RGB_ISR_IRAM_SAFE`:** code runs from
   PSRAM, so the CPU can keep refilling the bounce buffers while flash is written.
   Neither is in the prebuilt configuration.

Espressif documents 2 and 3 together for this problem. Untried next step:
- Set `LCD_BOUNCE_BUFFER_LINES` to 4 (must divide 600; costs 2 × 8 KB of internal
  RAM).
- Add to `platformio-v3.ini`:

  ```ini
  custom_sdkconfig =
      CONFIG_SPIRAM_XIP_FROM_PSRAM=y
      CONFIG_LCD_RGB_ISR_IRAM_SAFE=y
  ```

  `custom_sdkconfig` makes pioarduino compile ESP-IDF and the Arduino libraries
  from source. Expect a first build of 20–40 minutes.

Then check the free heap on the web status page, and watch the screen during a web
upload.
