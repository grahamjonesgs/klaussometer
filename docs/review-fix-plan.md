# Fix plan for the 2026-10-27 code review

This plan fixes every finding in [code-review-2026-10-27.md](code-review-2026-10-27.md) and removes the touch screen. It is written to be carried out unattended.

## Decisions (agreed 2026-10-27)

| Topic | Decision |
|---|---|
| Touch | Remove completely: GT911/TAMC code, LVGL input device, the tap handler, and `tamctec/TAMC_GT911` from `platformio.ini`. |
| Clock | Show time only (`HH:MM:SS`). Drop `showDate`. |
| Web `/update` and `/reboot` | Leave unauthenticated (trusted LAN). Only add the web-upload-vs-OTA guard. |
| Inside air quality | Restore green/amber/red using the `CO2_/PM25_THRESHOLD_*` constants. Stale shows grey (`COLOR_STALE`). |
| Battery min/max | Keep in SD only. Remove the NVS path and the global `Preferences storage`. |
| Structural refactors | Do all of them, after the bug and performance fixes, in separate commits. |
| Git | New branch `review-fixes` off `restructure-source-files`. One commit per group, each must pass `pio run`. Do not push. Never amend. |

**Defaults I chose without asking.** Change these before the run if you disagree.

- **OTA:** record the version that was tried in NVS. Skip that version for 24 h after a failed install, or after an install that rebooted into the same `FIRMWARE_VERSION`. This stops both the re-download loop and the reboot loop.
- **LVGL:** set `LV_OS_NONE`. With a single draw unit, the loop task already waits for the hidden render thread to finish each frame, so running rendering inline loses nothing. It removes an unpinned priority-3 task with an 8 KB stack.
- **SD file format:** `DataHeader` gains a magic number, a per-struct version and a CRC32 (`esp_rom_crc32_le`). Every `.bin` file is rejected once after flashing, so data is refetched and readings come back from retained MQTT messages. Logs are unaffected.
- **Board detection:** retry the expander probe 5 times, 20 ms apart. With touch gone there is no GT911 to positively identify a Matouch board.
- **Waveshare touch reset:** hold the GT911 in reset (expander IO1 low) so the chip stays off the I2C bus.
- **Native tests:** add `[env:native]` and fix the stale `uv_color` → `uvColor` test.

## Work groups (one commit each, in this order)

Each group lists the review findings it closes (appendix A numbers in the review doc).

### 1. Remove touch, make the clock non-blocking
- Delete `touchInit`/`touchRead`/`probeGT911Address`, `ts`, `touchLastX/Y`, `touchAvailable`, the `lv_indev_create` registration, `#include <TAMC_GT911.h>`, `onTimeTouched` and its event callback, and `showDate`.
- Remove the `touchInt`/`touchRst`/`touchRawMaxY` fields from `BoardConfig` (keep `i2cSda`/`i2cScl`, which the expander needs). Remove the `TOUCH_*` constants and the TAMC entry in `platformio.ini`.
- Waveshare: hold the touch RST low in `waveshare_expander_init`.
- Clock: `updatePeriodicStatus` uses `time(nullptr)`. Show "Syncing" while `now < TIME_SYNC_THRESHOLD`, otherwise `localtime_r` into a local `struct tm`. Redraw the clock when the second changes (`static time_t lastSec`), not on a 1000 ms millis gate.
- Remove the global `timeinfo`. `time_init()` uses a local.
- Closes: GT911 overflow (A7), touch mapping (A32), getLocalTime block (A2), clock skip (A33), timeinfo race (A23, A60).

### 2. MQTT single owner
- The receive task owns `mqttClient`. It connects and subscribes when WiFi is up and the client is disconnected. The Connectivity task only manages WiFi and NTP.
- Use `mqttClient.onMessage(cb)`. Messages that arrive during the subscribe burst, including retained ones, are then delivered instead of discarded. Zero-length payloads no longer put the parser out of step, and empty messages are logged in the callback.
- `logAndPublish`/`errorPublish` enqueue `{topic, payload, retained}` on a new `mqttTxQueue` (non-blocking, drop on full), and the receive task drains it. Remove `mqttMutex`.
- Call `mqttClient.setConnectionTimeout(5000)`, feed the watchdog before connect and per subscribe, and abort the subscribe loop on the first failure.
- `display.cpp` reads the connection state through `bool mqttIsConnected()`, backed by an atomic flag that the owner task sets.
- Closes: A1, A3, A4, A15, A20, A25, A45, A47.

### 3. Networking and API fixes
- **Solar token:** remove the `solarToken.token[0] = '\0'` before refresh (APIs.cpp:624). Check the `snprintf` return value and fail on truncation, so backoff applies.
- **OTA:** NVS skip-version guard (see defaults). Make `updateFirmware()` return `bool`. Use a 2 KB static buffer, and `vTaskDelay` only when nothing is available. Feed the watchdog between the version check and the GET.
- **Web upload vs OTA:** an `std::atomic<bool> otaInProgress` claimed with compare-exchange by both paths. A web upload is rejected while an HTTP OTA is running, and the API OTA is skipped while a web upload is running.
- **WiFi:** `WiFi.disconnect(false)` on failure. Simplify the `WIFI_RECONNECT_GROUP` waiting and fix the comment.
- **Chunked reader:** return -1 on a short chunk read, a bad CRLF, or a missing terminating zero-size chunk.
- **JSON:** validate key fields with `is<float>()` or keep the previous value with `| previous`, for solar current/daily/monthly and weather daily arrays.
- **Battery min/max reset:** key it on a stored `tm_yday` in the Solar struct, not on `tm_hour == 0`.
- **UV:** `lroundf` when storing the index.
- **setReuse:** drop the per-call toggles. The global `setReuse(false)` stays.
- **Solar token error log:** include the HTTP status.
- Closes: A16, A17, A21, A22, A38, A40, A46, A48, A49, A50, A51, A52, A53, A54. A18 (web auth) is accepted as-is by decision.

### 4. SD integrity and performance
- **`loadDataBlock`:** read into a PSRAM scratch buffer and copy only after the size, magic, version and CRC checks pass. Add per-struct `sanitize()` helpers that NUL-terminate char fields.
- **`writeDataBlock`:** write `<name>.tmp`, check write lengths, flush, close, remove the old file, rename. On load, fall back to `.tmp` if the main file is missing or invalid.
- **`DataHeader`:** `{uint32 magic; uint16 version; uint16 reserved; uint32 size; uint32 crc32}`.
- **Log rotation:** only remove and rename after the temp file is fully written. Set the line count from the lines actually copied.
- **Line counting and tail reading:** 512-byte block reads with `memchr`. Count lines once in `sdcard_init`, so the web handler never counts.
- **`getLogsJSON`:** stream or send without the second ~200 KB `String` copy.
- **Save throttle:** in `mqtt.cpp`, advance `lastReadingsSave`/`lastInsideAQSave` only when the save succeeds. Rate-limit a log line on SD mutex timeout.
- **New `sdcard_append_line_now()`:** one line format, under `sdMutex` with a short timeout, used by the shutdown handler and the boot-reason path in `main.cpp`.
- Don't call `logAndPublish` while holding `sdMutex`. Record the error text and log it after releasing (now cheap, because group 2 makes it a queue push).
- **ESP_LOG vprintf hook:** use a static buffer guarded by a try-take mutex, falling back to plain `vprintf`, so system task stacks don't carry 256 bytes.
- Closes: A5, A6, A10, A11, A12, A13, A19, A24, A26, A41, A43, A44, A62, A76.

### 5. Display correctness and redraw cost
- **Status dots:** treat `updateTime == 0 || now < TIME_SYNC_THRESHOLD` as stale (red).
- **Grid percentage:** show `--` when today's usage is ≤ 0. Compute with `lroundf(buy/use*100)` clamped to 0..100.
- **Inside AQ:** threshold colours restored, stale grey.
- **UV panel:** set `dirtyUv` (and `dirtyWeather`) whenever `isDay` changes, both in `staleness.cpp` and `adjustDayNightMode`.
- **Packed field:** copy `fc.ts` into a local `time_t` before calling `localtime_r`.
- **`updatePeriodicStatus`:** set label text and styles only when they change (cache the last value).
- **Staleness checks:** run them once a second from `loop()`, not every 20 ms pass.
- Closes: A9, A28, A31, A34, A35, A36, A37, A39, A55, A56, A57.

### 6. Board and LVGL setup
- **Waveshare:** run the expander init (IO modes, SD CS low, backlight off, LCD/touch reset) straight after board detection and before `sdcard_mount()`.
- **Board detection:** retry 5 times.
- **Draw buffer:** try `MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA` first, falling back to SPIRAM. Size it with `LV_COLOR_DEPTH/8`, not `sizeof(lv_color_t)`.
- **LVGL OS:** `LV_OS_NONE` in `lv_conf.h`.
- **Magic numbers:** replace the ones in `board.cpp` (I2C pins and address, PWM cap, delays) with `constants.h` names.
- Closes: A8 (Waveshare SD), A14, A29, A30, A42, A58, A63.

### 7. Battery min/max in SD only
- Remove the NVS read and write and the global `Preferences storage`. Initialise min to 100 and max to 0 when the SD restore fails.
- Closes: A78.

### 8. Dead code and compile-time checks
- **Unused items:** delete the unused constants (`DATA_SETTING`, `DATA_ONOFF`, `TASK_STACK_LARGE`, `SD_CS_PIN`, `LOG_TOPIC`, `ERROR_TOPIC`, `SOLAR_TOKEN_WAIT_SEC`, `API_SEMAPHORE_WAIT_SEC`, `TIME_RETRIES`, `WIFI_SLASH`, and others the grep finds), the `LogEntry` struct, the stray `extern Solar solar` and the unused parameter.
- **mountain_sunset generator:** stop it emitting the unused `mountain_offset_min`/`MS_NONE8`, and regenerate.
- **v3 branches:** remove all `ESP_ARDUINO_VERSION_MAJOR >= 3` branches (staying on v2 is deliberate) and move the upgrade notes to the README.
- **Readings layout:** add `static_assert`s on the readings table against `MAX_READINGS` and `3 * ROOM_COUNT`.
- Closes: A59, A61, A65.

### 9. Structure: state ownership and header hygiene
- **New `state.h`/`state.cpp`:** data structs, `readings[]`, `numberOfReadings` (constexpr), `dataMutex` and the dirty flags. Remove the hand-written `extern` blocks from the `.cpp` files.
- **Client ownership:** `mqttClient` (and `espClient`) move into `mqtt.cpp`. `http`/`httpsClient` move into `APIs.cpp`, and `OTA.cpp` gets what it needs through a function.
- **Headers:** `types.h` stops including `config.h` and `logging.h`. Each `.cpp` includes what it uses. The `ui_*` label macros move from `constants.h` into `display.cpp`. Drop the unused `#include "OTA.h"` in `connections.cpp`. The `BoardConfig` tables move into `board.cpp` behind `board_select()`.
- **Internal linkage:** make module-internal buffers and functions `static`, and drop them from the headers.
- Closes: A67, A74, A75.

### 10. Structure: API layer
- **`apiRequest()` helper:** a static helper (URL, optional POST body, optional auth, `JsonDocument&`) that always calls `http.end()`, logs consistently and returns an error code. All 7 fetchers use it. Daily and monthly solar share one function with a period parameter.
- **Job table:** `api_manager_t` becomes a table of `ApiJob { name, backoff, fetch, lastUpdate, intervalSec, enabled }`. The UV night-zeroing moves into `applyUvNight()`. OTA timing moves into an OTA-owned static, which removes the exported `lastOTAUpdateCheck`.
- **HWM helper:** `logStackHwmIfDue(task, &lastMs)` in `logging.cpp`, used by all tasks.
- Closes: A66, A69, A77.

### 11. Structure: restore and display
- **Restore table:** a `PersistedBlock` table plus `restorePersistedState()`, which replaces the 8 load-and-log blocks in `setup()`. Every dirty flag is set after restore.
- **Display:** each display update copies its struct under `dataMutex`, releases it, then renders.
- **Weather min/max:** the widening moves into `fetchWeather()`.
- **Day/night theme:** one shared function for `display_init()` and `adjustDayNightMode()`.
- **`updateInsideAirQuality()`:** becomes a table of 4 sensors instead of 4 copied branches.
- Closes: A27, A64, A68, A70, A71, A72, A73.

### 12. Native tests
- `[env:native]` with `platform = native`, `test_build_src = yes`, `build_src_filter = -<*> +<utils.cpp>`. Fix the stale test names. Add tests for the pure helpers touched above, such as grid percentage and the chunked reader if it moves to `http_util.cpp`.
- Run `pio test -e native`.
- Closes: A79.

## Verification

- **Each commit:** `~/.platformio/penv/bin/pio run` must succeed with no new warnings. Fix and re-run before committing.
- **At the end:** `pio test -e native` passes. Also confirm with `git grep` that nothing references `TAMC`, `touch`, `mqttMutex`, `timeinfo` or `storage`.
- **Not possible unattended:** flashing and on-device checks. See the checklist below.

## Hardware checklist for you, after flashing

1. **Boot with WiFi off:**
   - The clock shows "Syncing" immediately and the UI stays responsive.
   - The status dots are red, not green.
2. **Waveshare cold power-on (unplug for 10 s):**
   - The SD card mounts and state is restored.
   - The backlight follows day and night.
3. **MQTT:**
   - Readings arrive after a broker restart.
   - `mosquitto_pub -r -n -t kitchen/co2/set` doesn't drop the connection.
4. **First boot after flashing:** all `.bin` files are rejected once (format change). Data refetches within a few minutes.
5. **Logs:** open `/logs`, then `/api/logs/error`, straight after boot. The UI shouldn't freeze.
6. **Inside AQ:** CO2 or PM2.5 colours change at the thresholds.
7. **Heap and stacks:** compare the Stack HWM and free-heap lines with the current firmware.
