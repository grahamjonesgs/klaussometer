# Arduino-ESP32 3.x

Since 2026-11-02 the release build is Arduino-ESP32 3.x (Arduino 3.3.6, ESP-IDF 5.5,
pioarduino 55.03.36). Before that it was 2.x (`espressif32@6.10.0`, ESP-IDF 4.4); the
last 2.x commit is tagged **`arduino2-final`**. This note covers what the port found,
how to build and flash, and how to go back.

## Why it was adopted

- **No idle flicker with bounce buffers.** Without them the RGB panel reads PSRAM
  directly and flickers every few seconds, even with nothing happening. 4-line bounce
  buffers (`LCD_BOUNCE_BUFFER_LINES`, ESP-IDF 5 only) stop that.
- **The tearing during flash writes is still there.** Everything 3.x offers for it
  has been tried: see [The tearing](#the-tearing).
- **Faster web uploads:** about 18–23 s, against 34–39 s on 2.x.

The cost is internal heap. Measured on the Waveshare about a minute after boot:

| Build | Free heap | Lowest |
|---|---|---|
| 2.x | 146 KB | 97 KB |
| 3.x, no bounce buffers | 106 KB | 59 KB |
| 3.x, 4-line bounce buffers | 92 KB | 46 KB |
| 3.x, 10-line bounce buffers | 67 KB | 19 KB (TLS fails) |

Tested on the Waveshare: WiFi, NTP and MQTT, every API fetch, SD save and restore
(including a card inserted after boot), the web log viewer, web firmware upload, and
soaks with no resets. The Matouch first ran 3.x on 2026-11-11: WiFi, NTP, MQTT and the
API fetches worked, and 20 of 20 web reboots reset cleanly. No soak or web upload yet.

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

## What changed for 3.x

- **`platformio.ini`:** the pioarduino platform URL, libraries pinned to the exact
  tested versions (GFX 1.6.9, LVGL 9.5.0), no `lib_ignore`, and its own `core_dir` (see below). It drops the three
  `-DconfigGENERATE_RUN_TIME_STATS` / `-DconfigUSE_STATS_FORMATTING_FUNCTIONS` /
  `-DCONFIG_SPIRAM_USE_MALLOC` flags. Those only reach the Arduino core compiled
  from source, not the precompiled ESP-IDF.
- **`#if ESP_ARDUINO_VERSION_MAJOR >= 3` branches** for the API changes, so the code
  still builds on 2.x:
  - `ledcAttachChannel()` / `ledcWrite(pin, …)` for the Matouch backlight;
  - `RGB565_BLACK`;
  - `esp_task_wdt_reconfigure()` with an `esp_task_wdt_config_t`;
  - bounce buffers passed to the GFX panel constructor.
- **`APIs.h`** includes `<WiFiClientSecure.h>` rather than forward-declaring
  `class WiFiClientSecure`. On 3.x that name is a typedef of `NetworkClientSecure`.
- **`LCD_BOUNCE_BUFFER_LINES`** in `constants.h`: 4 (must divide 600).
- **Fixes that apply to 2.x too:**
  - **SD card:** a mounted flag, with a failed mount retried at most once a minute
    and logged once, so a card inserted after boot is picked up.
  - **Watchdog:** `logAndPublish` only feeds it from subscribed tasks.
  - **WiFi:** `WiFi.disconnect()` only once WiFi has started.
  - **MAC address:** read from eFuse with `esp_read_mac()` (`WiFi.macAddress()` is
    all zeros on 3.x before WiFi starts).
  - **Board detection:** the detected board is printed on the UART as well.

## Building and flashing

### Own PlatformIO core

pioarduino and the official `espressif32` platform both install a package called
`framework-arduinoespressif32`, and building one overwrites the other in a shared
`~/.platformio`. `platformio.ini` sets `core_dir = ~/.platformio-arduino3`, so this
project's packages live there and other projects on `espressif32@6.x` are left alone.
VS Code and `pio run` pick it up with no extra steps.

The first build on a machine downloads the toolchain and framework (about 15
minutes). After that, every build is a full rebuild of about 5–6 minutes.

### Flash

- **USB, first time on a board:** `pio run -t upload --upload-port <port>`. This
  also writes the 3.x bootloader and partition table.
- **App only, faster:** don't use `-t nobuild`, which breaks pioarduino's upload
  arguments. Use esptool instead:

  ```sh
  ~/.platformio/penv/bin/python -m esptool --chip esp32s3 --port <port> --baud 921600 \
      write_flash 0x10000 .pio/build/klaussometer/firmware.bin
  ```

- **OTA / web upload:** works. A 3.x app boots from the 2.x bootloader that an
  OTA-updated board keeps (tested). Make sure the OTA server has the 3.x build:
  a 2.x build with a higher version number would be installed over it.

### Going back to 2.x

Check out the `arduino2-final` tag (or take its `platformio.ini`, which uses the
shared core and `lib_ignore = Time`) and flash as before. Expect the same one-off
SD restore failures as below.

### Expect on first boot

- **SD restore failures for five blocks:** `solar_token`, `weather`, `uv`,
  `air_quality` and `solar_forecast` fail with "size N, struct expects N+4 bytes".
  `time_t` is 32-bit on 2.x and 64-bit on 3.x, so every persisted struct holding a
  timestamp grows by 4 bytes. The size check rejects the old file and the data is
  fetched again within about 20 seconds. The same happens going back to 2.x.

### Build quirks

- **`TypeError: expected str, bytes or os.PathLike object, not NoneType`:** the first
  build after a library change, or after removing `custom_sdkconfig`, sometimes fails
  with this. Run it again.
- **`src/idf_component.yml`:** pioarduino generates it (and a `.orig`) in the
  project. It's gitignored.
- **VS Code:** the PlatformIO extension sometimes empties `.pio/build`. Copy the
  firmware somewhere else before a long test.

## The tearing

The screen still jumps and tears during a web upload or OTA. Updates are rare, so
this is accepted. On ESP-IDF 4.4 there is no fix. ESP-IDF 5 has three relevant
features, and all three have been tried on the Waveshare:

1. **`CONFIG_LCD_RGB_RESTART_IN_VSYNC`:** resyncs the panel every frame. It's already
   on in pioarduino's prebuilt configuration. Not enough on its own.
2. **Bounce buffers:** the panel is fed from internal RAM instead of DMA straight
   from PSRAM. On, through `LCD_BOUNCE_BUFFER_LINES`. With 4 lines they stop the idle
   flicker, but not the tearing.
3. **`CONFIG_SPIRAM_XIP_FROM_PSRAM` + `CONFIG_LCD_RGB_ISR_IRAM_SAFE`:** code runs from
   PSRAM and the panel interrupt stays live, so the bounce buffers can be refilled
   while flash is written. Espressif documents this, with 2, as the fix. **Tested
   2026-11-02 together with 2 and 1: the tearing was unchanged.** The board was also
   worse: slow web responses, failed HTTPS fetches and MQTT subscribes, and a task
   watchdog reset in the first minutes, presumably because instruction fetches now
   compete with the panel for PSRAM. It was backed out.

A full-screen black overlay during the update was tried on 2.x and didn't help
either.

### Custom ESP-IDF settings with pioarduino

Notes from the test in 3, if it is ever tried again. The settings go in
`platformio.ini`:

```ini
custom_sdkconfig =
    CONFIG_SPIRAM_XIP_FROM_PSRAM=y
    CONFIG_LCD_RGB_ISR_IRAM_SAFE=y
```

- pioarduino then compiles ESP-IDF and the Arduino libraries from source (about
  17 minutes and a large download) and replaces the prebuilt libraries in `core_dir`.
  It fingerprints the inline text, so changing or removing it rebuilds or reinstalls
  them. Keep it inline: the contents of a `file://` reference aren't fingerprinted.
- **Turn off the PlatformIO extension's auto-rebuild first**
  (`"platformio-ide.autoRebuildAutocompleteIndex": false` in `.vscode/settings.json`).
  Otherwise it runs the same build at the same time when files change, and the two
  corrupt the downloads (missing CMake modules, "component not registered",
  missing files under `managed_components`).
- **Remove the unused add-on components** with `custom_component_remove`
  (`espressif/esp_rainmaker`, `rmaker_common`, `esp_insights`, `esp_diagnostics`,
  `esp_diag_data_store`, `network_provisioning`, and similar). pioarduino's build
  doesn't generate the `.S` files for components that embed data, so esp_insights
  fails with "https_server.crt.S not found".
- **Copy `default_16MB.csv` into the project root.** In this mode the partition table
  is looked up there, not in the framework.
- It writes `sdkconfig.defaults`, `sdkconfig.klaussometer`, `.dummy/`,
  `managed_components/` and `dependencies.lock` into the project, and deletes the old
  root `CMakeLists.txt`.
- Going back: remove the settings, move those files out, `git checkout CMakeLists.txt`,
  and build twice (the first build removes the custom libraries and fails with the
  `TypeError` above; the second reinstalls the prebuilt ones).
