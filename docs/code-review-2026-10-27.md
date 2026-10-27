# Code Review — 2026-10-27

**Branch reviewed:** `restructure-source-files` @ `9f97b78` ("Split main.cpp and OTA.cpp into focused source files")
**Scope:** hand-written source in `src/*.cpp` and `src/*.h`. Excluded: the generated SquareLine `src/ui/`, `lv_conf.h` and the `html.h` page string.
**Method:** five reviewers, one per area (concurrency, networking, memory and data handling, display and board, structure). A separate verifier then tried to disprove each finding against the code and the library sources. 80 findings came out; 79 held up and 1 was refuted. About 15 of the 79 are the same issue found from two areas, so there are roughly 60 distinct issues. The summary below merges the duplicates; the appendix lists every verified finding as reported.

Verdicts: **CONFIRMED** means the verifier traced the problem in the code. **PLAUSIBLE** means it is likely real but the verifier could not fully confirm the conditions that trigger it.

Line numbers refer to commit `9f97b78`.

---

## Summary

### Fix first: real bugs

1. **The MQTT client is used from two tasks without the lock.** [`connections.cpp:47-71`](../src/connections.cpp#L47-L71)
   - `mqtt_connect()` runs `connect()` and 20 `subscribe()` calls without `mqttMutex`. Meanwhile the receive task ([`mqtt.cpp:55`](../src/mqtt.cpp#L55)) and `logAndPublish` use the same client.
   - The MQTT library keeps a send buffer pointer that each call points at that call's own stack. If another task interrupts, the rest of a subscribe packet is written onto that other task's stack.
   - Separately, any retained message that arrives during the subscribe burst is thrown away by the library. So retained forecast or readings can be lost after every reconnect, even with the lock added.
   - **Fix:** let one task own the client. Connect and subscribe from the receive task, and give `logAndPublish` a send queue that the receive task drains.
2. **`getLocalTime()` waits up to 5 s per call** (5 s is its default timeout). [`display.cpp:551`](../src/display.cpp#L551), [`:596`](../src/display.cpp#L596)
   - Until NTP syncs, every 1 s status pass freezes LVGL, touch and the web server for 5 s.
   - **Fix:** use `time(nullptr)` with `localtime_r` into a local `struct tm`, and drop the global `timeinfo`. Two tasks write that global.
3. **One failed I2C read kills touch until reboot.** [`board.cpp:150`](../src/board.cpp#L150)
   - When the GT911 doesn't respond, the TAMC library's `read()` sees 0xFF and treats it as 15 touches. It then writes 15 points into `points[5]`, which overwrites `addr`, `width` and `height` inside the library object.
   - Every later read goes to the wrong address, and there is a phantom press near x=65535. This may explain the "i2cRead Error -1" log flood.
   - **Fix:** write a small `touchRead` that checks `requestFrom()` and clamps the count to 1–5.
4. **The solar token is cleared before it is refreshed.** [`APIs.cpp:624`](../src/APIs.cpp#L624)
   - If the refresh at 12 h fails, solar updates stop even though the old token is valid for another ~12 h.
   - **Fix:** delete the line. A rejected token is already cleared on the "auth invalid token" path.
5. **An empty MQTT payload forces a disconnect.** [`mqtt.cpp:62`](../src/mqtt.cpp#L62)
   - A zero-length message, such as someone clearing a retained topic, puts the library's parser out of step and makes it drop the connection.
   - **Fix:** switch to the `onMessage` callback. The "Empty MQTT message" branch can then never be hit by a zero-length message.
6. **OTA can loop forever.** [`OTA.cpp:24`](../src/OTA.cpp#L24)
   - If the advertised image fails `Update.end()`, the whole binary is downloaded again every 5 minutes, and the API task is blocked each time.
   - If the version file is bumped before the new `.bin` is uploaded, the device installs the old image, reboots, and repeats: a reboot loop.
   - **Fix:** remember which version failed or was installed.
7. **Status dots show green with no data.** [`display.cpp:263`](../src/display.cpp#L263)
   - When data was never fetched or the clock isn't synced, the dot is green.
   - **Fix:** treat `updateTime == 0 || now < TIME_SYNC_THRESHOLD` as stale.
8. **Waveshare mounts the SD card before the expander drives SD CS (IO4).** [`main.cpp:196`](../src/main.cpp#L196) *(PLAUSIBLE)*
   - This would make the SD card fail only on a cold power-on, which looks intermittent.
   - **Fix:** initialise the expander straight after board detection.
9. **`WiFi.disconnect(true)` turns the radio off.** [`connections.cpp:26`](../src/connections.cpp#L26)
   - The "let autoReconnect work" iterations that follow therefore do nothing, adding about 20 s of dead time per recovery cycle.

### SD data integrity

- [`SDCard.cpp:162`](../src/SDCard.cpp#L162): `loadDataBlock` reads straight into the live global. A failed checksum leaves it half-overwritten, and `solarToken.token` can end up without a terminating NUL. **Fix:** read into a scratch buffer first and copy only after the checks pass.
- [`SDCard.cpp:74`](../src/SDCard.cpp#L74): the file is truncated before it is rewritten, so a power cut during a save loses the last good copy. **Fix:** write to a temp file, then rename it.
- [`SDCard.cpp:265`](../src/SDCard.cpp#L265): log rotation deletes the log even when the temp file couldn't be created.
- [`utils.cpp:201`](../src/utils.cpp#L201): the checksum is an 8-bit XOR. **Fix:** use `esp_rom_crc32_le()`, which is free (it's in ROM), and add a magic number and version to `DataHeader`.
- [`mqtt.cpp:120`](../src/mqtt.cpp#L120): the save throttle timestamp is updated even when the save fails.

### Smaller correctness issues

- **Missing JSON fields become 0:** responses with `success:true` but absent fields write zeros into displayed values ([`APIs.cpp:405`](../src/APIs.cpp#L405)).
- **Chunked reader:** [`APIs.cpp:726`](../src/APIs.cpp#L726) carries on after a short read and returns a garbled body as success.
- **Battery min/max reset:** it only happens if a fetch succeeds during hour 0 ([`APIs.cpp:436`](../src/APIs.cpp#L436)). Key it on the date instead.
- **UV index:** truncated rather than rounded ([`APIs.cpp:143`](../src/APIs.cpp#L143)).
- **Grid percentage:** shows 100% when today's usage is 0 ([`display.cpp:109`](../src/display.cpp#L109)).
- **Inside air-quality colours:** the threshold colouring was dropped in commit 95318f7, so the `CO2_/PM25_THRESHOLD_*` constants are unused. Stale values also show red, where rooms use grey.
- **UV panel:** isn't refreshed when the clock-derived day/night state flips ([`staleness.cpp:125`](../src/staleness.cpp#L125)).
- **Clock:** the seconds visibly skip because the 1 s timer drifts against wall-clock seconds ([`display.cpp:522`](../src/display.cpp#L522)).
- **Web endpoints:** `/update` and `/reboot` have no authentication, and any web page open on the LAN can trigger them. A web upload can also abort an OTA download already in progress ([`web_server.cpp:81`](../src/web_server.cpp#L81)).
- **Packed field address:** `&fc.ts` (a field of a packed struct) is passed to `localtime_r` ([`display.cpp:485`](../src/display.cpp#L485)).
- **Token truncation:** the `snprintf` return value for the token isn't checked ([`APIs.cpp:331`](../src/APIs.cpp#L331)).

### Performance

- **Byte-by-byte line counting:** `countLinesInFile` reads up to 1 MB one byte at a time while holding `sdMutex` ([`SDCard.cpp:27`](../src/SDCard.cpp#L27)). The first `/api/logs/error` request runs this on the LVGL loop, freezing the UI and causing other tasks' SD saves to time out. **Fix:** use block reads with `memchr`.
- **Staleness checks:** they take `dataMutex` about 50 times a second ([`main.cpp:329`](../src/main.cpp#L329)), for data that only changes on a 30-minute scale. Running them once a second is plenty.
- **Status row:** `updatePeriodicStatus` re-sets identical text and styles every second, which forces a redraw ([`display.cpp:528`](../src/display.cpp#L528)).
- **LVGL draw buffer:** PSRAM is tried before internal RAM, and the buffer is sized with the 3-byte `lv_color_t` ([`board.cpp:196`](../src/board.cpp#L196)).
- **Hidden LVGL render task:** `LV_OS_FREERTOS` with a single draw unit creates an unpinned priority-3 render task that gains nothing. Choose `LV_OS_NONE` or 2 draw units deliberately.
- **OTA download:** a 128-byte buffer with a 1 ms delay on every read adds ≥20 s ([`OTA.cpp:66`](../src/OTA.cpp#L66)).
- **TLS session left open:** `setReuse(false)` comes after `http.end()`, so the Solarman TLS session (about 40 KB) stays open ([`APIs.cpp:361`](../src/APIs.cpp#L361)).
- **Blocking logging:** `logAndPublish` does blocking socket I/O on the caller's task, sometimes while `sdMutex` is held ([`logging.cpp:25`](../src/logging.cpp#L25)).

### Structure

The split into separate files is a good start, but **`main.cpp` still owns every module's state**. More than 30 hand-written `extern` declarations are repeated across 8 `.cpp` files, and nothing checks them against each other. Suggestions:

- **Single owner for state:** add a `state.h`/`state.cpp` for the data structs, `dataMutex` and the dirty flags. Move `mqttClient` into `mqtt.cpp` and `http` into `APIs.cpp`, and make internal helpers `static`.
- **API request helper:** one `apiRequest()` function for the 7 near-identical fetch functions. Daily and monthly solar are almost copies of each other.
- **Tables instead of repeated code:**
  - A table of persisted blocks, so `setup()` doesn't repeat load-and-log 8 times. The restore step should set every dirty flag; the `dirtyInsideAQ` bug was exactly this kind of slip.
  - A job table for the `api_manager_t` scheduler.
  - One stack high-water-mark helper instead of 5 copies.
- **Display:** copy the data under the mutex, then render. Right now the mutex is held across dozens of LVGL calls. Move the weather min/max widening out of the display code into `fetchWeather()`.
- **Headers:** `types.h` includes `config.h` (secrets) everywhere, and `constants.h` refers to `ui_*` symbols.
- **SD writes:** the shutdown and boot-reason writes in `main.cpp` bypass `sdMutex`, and the line format exists in three copies.
- **Battery min/max:** stored in both NVS and SD, with NVS overriding SD on boot. Pick one.
- **Readings layout:** add `static_assert`s for the readings table against `MAX_READINGS` and the 3×`ROOM_COUNT` layout.
- **Dead code:** about 13 unused constants, plus the v3 `#if` branches. One of them uses a type name that doesn't exist (`esp_task_wdt_config`), so it would fail to compile if ever enabled.
- **Tests:** the unit test file is stale. It calls `uv_color()`, but the function is now `uvColor()`, so it wouldn't compile. Adding an `[env:native]` test environment would catch that.

---

## Appendix A: All verified findings

These are sorted by severity. Where two areas found the same issue, both entries are kept, because each adds a different angle.

### A1. MQTT connect/subscribe run on the Connectivity task without mqttMutex, racing the receive task and logAndPublish on the same MqttClient

**Severity:** high · **Verdict:** CONFIRMED · **Area:** concurrency  
**Location:** [`src/connections.cpp:48`](../src/connections.cpp#L48)

**What:** mqtt_connect() calls mqttClient.connect() (line 48) and 20x mqttClient.subscribe() (lines 56, 67) without taking mqttMutex. Other tasks use the same client object at the same time. The Receive MQTT task (priority 2) runs parseMessage()/poll()/read() at mqtt.cpp:61-83, and it checks connected() at mqtt.cpp:55 outside the mutex. The API Manager and other tasks publish through logAndPublish (logging.cpp:25-31). In ArduinoMqttClient, subscribe() spins on poll() for up to _connectionTimeout and calls stop() if it times out. connect() calls _client->stop() and resets _rxState. Every packet builder (subscribe, ping from poll, publishHeader from beginMessage/endMessage) sets the shared member _txBuffer to point at a buffer on the caller's own stack. None of this is safe across tasks.

**Failure scenario:** (a) After a broker reconnect, the Connectivity task is inside subscribe() between beginPacket() and endPacket(). The higher-priority receive task preempts it, takes the free mqttMutex, and its poll() sends a keep-alive PINGREQ, which repoints _txBuffer at the receive task's stack frame. When the Connectivity task resumes it writes the rest of its SUBSCRIBE bytes through _txBuffer into another task's stack. That corrupts memory or crashes, and garbage goes out on the wire, so the broker drops the connection. The API task's logAndPublish (same priority, time-sliced) can do the same thing through publishHeader. (b) The broker sends a retained PUBLISH (for example solar/forecast) right after a SUBACK. poll() in the next subscribe() loop moves the state machine to READ_PUBLISH_PAYLOAD, and the next poll() discards the payload, so the retained forecast or readings are silently lost after every reconnect. (c) A subscribe() timeout calls stop() on the WiFiClient, which resets its _rxBuffer shared_ptr while the receive task is inside read(). That is a use-after-free.

**Suggested fix:** Make one task the only owner of mqttClient. The cleanest option is to move connect/subscribe into receive_mqtt_messages_t: when connected() is false and WiFi is up, call mqtt_connect() there. Then have logAndPublish/errorPublish enqueue {topic, payload, retained} onto a small MQTT TX queue that the same task drains between parseMessage() calls. No other task touches the client and mqttMutex is no longer needed. Minimal fix: wrap the whole connect plus subscribe sequence in xSemaphoreTake(mqttMutex, ...)/Give, and move the connected() check at mqtt.cpp:55 inside the mutex. logAndPublish already uses MUTEX_NOWAIT, so its calls inside mqtt_connect will just skip MQTT instead of deadlocking.

**Verifier notes:** Confirmed. connections.cpp:47-71 calls connected(), connect() and 20 subscribe() calls with no mqttMutex. In MqttClient, connect() sets _connected=true as soon as CONNACK arrives (MqttClient.cpp:950). From that point the Receive task (priority 2, main.cpp:307) passes its unlocked connected() check (mqtt.cpp:55) and runs parseMessage()/poll() under mqttMutex, while the Connectivity task (priority 1) is still in subscribe()'s own poll() loop (MqttClient.cpp:349) without the mutex. Both tasks then drive the same _rxState/_rxLength/_returnCode state machine and the same socket. beginPacket() repoints the shared _txBuffer at the caller's stack array (MqttClient.cpp:1062, subscribe's packetBuffer at :333). This means a preempting ping() from poll, or a publish from logAndPublish on the API task (same priority, time-sliced), can make the next write land in another task's stack. Note on (b): subscribe's poll loop throws away any PUBLISH payload that arrives while it waits for a SUBACK (DISCARD state, MqttClient.cpp:600-613). That happens even single-threaded, so the minimal 'wrap in mutex' fix does not stop retained messages from being lost during the subscribe burst. Only the 'single owner' fix, or subscribing to a wildcard or fewer topics, avoids that. The single-owner redesign is the right fix.

### A2. getLocalTime() default 5 s timeout freezes the UI loop while NTP is unsynced

**Severity:** high · **Verdict:** CONFIRMED · **Area:** display-board  
**Location:** [`src/display.cpp:551`](../src/display.cpp#L551)

**What:** In Arduino-ESP32 v2, getLocalTime() is declared `getLocalTime(struct tm*, uint32_t ms = 5000)`. It loops with delay(10) until tm_year > 2016 or 5 s pass (esp32-hal-time.c:83-96). updatePeriodicStatus() calls it once a second from loop(), and onTimeTouched() calls it too (line 596). Until NTP sync, every call blocks the loop task for 5 s. During that time lv_timer_handler() (touch, rendering), webServer.handleClient() and display_update() all stop.

**Failure scenario:** After a power cut, the ESP32 boots faster than the router or ISP comes back, or WiFi/NTP is otherwise unreachable. loop() then completes about once every 5 s: the screen shows "Syncing" only after 5 s, taps on the clock are ignored or delayed, the web UI times out, and panel updates lag. This lasts until NTP succeeds, which could be minutes or indefinitely.

**Suggested fix:** Don't block. Use `time_t now = time(nullptr); if (now < TIME_SYNC_THRESHOLD) { show "Syncing" } else { struct tm t; localtime_r(&now, &t); strftime(...); }`, or call getLocalTime(&t, 0). Use a local struct tm instead of the global timeinfo. connections.cpp:75 also writes timeinfo from another task, so the global is an unsynchronised shared struct.

**Verifier notes:** Arduino.h:222 declares getLocalTime(struct tm*, uint32_t ms = 5000). esp32-hal-time.c:83-96 loops with delay(10) until tm_year > 116. display.cpp:551 calls it from loop() through updatePeriodicStatus, and display.cpp:596 calls it from the touch event. After a cold power-on, before NTP sync, time() is close to the epoch, so every 1 s status pass blocks the loop task for 5 s. During that time lv_timer_handler, webServer.handleClient and display_update all stall. The loop still resets the WDT, so there is no reboot, only an unresponsive UI. The global timeinfo is also written from connections.cpp:75 on another task. The suggested fix (time(nullptr) checked against TIME_SYNC_THRESHOLD plus localtime_r into a local struct tm, or getLocalTime(&t, 0)) is correct.

### A3. mqtt_connect() calls connect() and subscribe() without mqttMutex, racing the receive task and logAndPublish on shared MqttClient buffers

**Severity:** high · **Verdict:** CONFIRMED · **Area:** memory  
**Location:** [`src/connections.cpp:48`](../src/connections.cpp#L48)

**What:** Every other user of mqttClient takes mqttMutex: receive_mqtt_messages_t (mqtt.cpp:60) for parseMessage/read, and publishMessageInternal (logging.cpp:25) for beginMessage/print/endMessage. mqtt_connect() calls mqttClient.connect() and then 20 subscribe() calls with no lock. In ArduinoMqttClient, subscribe() writes a packet and then loops on poll() (MqttClient.cpp:349-357) until the SUBACK arrives. poll() changes _rxState, _rxLength and the _rxMessageTopic String. write() checks bounds, lazily mallocs _txPayloadBuffer, then memcpy's and updates _txPayloadBufferIndex in separate steps (MqttClient.cpp:675-684).

**Failure scenario:** After a broker reconnect, the connectivity task (priority 1) is inside subscribe() waiting in poll(). As soon as connected() is true, the receive task (priority 2) preempts it every 100 ms and calls parseMessage(), which runs poll() on the same object. Retained messages such as solar/forecast arrive at this moment, so both tasks parse the same RX stream and reassign the _rxMessageTopic String at the same time. The result is lost or garbled messages, or a String realloc race that corrupts the heap. The API Manager runs at the same priority 1 and time-slices with the connectivity task, so its logAndPublish() can also interleave its beginMessage/print/endMessage with a subscribe packet. That corrupts the outgoing MQTT stream (the broker drops the connection) and can race the bounds check in write().

**Suggested fix:** Take mqttMutex (portMAX_DELAY or a generous timeout) around the whole connect + subscribe sequence in mqtt_connect(). Its internal logAndPublish() calls use MUTEX_NOWAIT, so they will simply skip the MQTT publish (the mutex isn't recursive, but there's no deadlock). Alternatively, switch to xSemaphoreCreateRecursiveMutex. Also take the mutex for the mqttClient.connected() check at connections.cpp:128, or accept that check as advisory.

**Verifier notes:** connections.cpp:48-71 calls mqttClient.connect() and about 20 subscribe() calls without mqttMutex. Every other user takes the mutex (mqtt.cpp:60, logging.cpp:25). Priorities in main.cpp:307-311: receive task 2, connectivity 1, API Manager 1, all on core 1. Once connect() succeeds, connected() returns true, so the receive task (mqtt.cpp:55-61) takes the mutex and calls parseMessage()/poll() while the connectivity task is inside subscribe()'s poll loop on the same object. Both tasks then consume one RX stream and change _rxMessageTopic and the rx state. The API Manager's logAndPublish can also interleave its beginMessage/print/endMessage with subscribe packets. The fix is right: hold mqttMutex across connect and subscribe. The internal logAndPublish calls use MUTEX_NOWAIT and will just skip MQTT.

### A4. mqtt_connect() calls connect()/subscribe() without mqttMutex while the receive task is reading the same client

**Severity:** high · **Verdict:** CONFIRMED · **Area:** structure  
**Location:** [`src/connections.cpp:48`](../src/connections.cpp#L48)

**What:** The receive task takes mqttMutex around mqttClient.parseMessage() and read() (mqtt.cpp:60-84). The connectivity task calls mqttClient.connect() (connections.cpp:48) and then mqttClient.subscribe() 20 times (lines 56 and 67) without taking that mutex. In ArduinoMqttClient, subscribe() runs its own poll() loop while it waits for SUBACK (MqttClient.cpp:349-357), and poll() is the same stream parser that parseMessage() drives. As soon as connected() goes true, the receive task (priority 2, which preempts connectivity at priority 1) starts calling parseMessage() on the same socket. Meanwhile the connectivity task is still inside subscribe()'s poll loop.

**Failure scenario:** The broker restarts or WiFi drops, and the device reconnects. mqtt_connect() sends SUBSCRIBE for cave/tempset-ambient/set and the broker immediately sends that topic's retained PUBLISH. Both tasks now read the socket: subscribe()'s poll() takes some of the PUBLISH bytes and parseMessage() in the other task takes the rest, so the parser state gets out of sync. Retained readings are lost (rooms show stale or '--' until the next sensor message), SUBACKs are missed and log 'MQTT subscribe failed', or the client is stopped by a malformed-packet error. The cycle can repeat on every reconnect.

**Suggested fix:** In mqtt_connect(), take mqttMutex (portMAX_DELAY or a generous timeout) around the whole connect plus subscribe sequence, and keep logAndPublish/errorPublish calls outside the locked region. logAndPublish's non-blocking try-lock will simply skip MQTT, so nothing deadlocks. A cleaner option is to move connect and subscribe into mqtt.cpp and run them from the receive task itself, so a single task ever touches mqttClient; the connectivity task then only manages WiFi and sets a 'reconnect MQTT' flag.

**Verifier notes:** Verified connections.cpp:47-71: mqttClient.connect() and 20 subscribe() calls are made with no mqttMutex. In the pinned ArduinoMqttClient (.pio/libdeps/.../MqttClient.cpp:349-357), subscribe() loops on poll() until the SUBACK arrives and calls stop() on timeout. The receive task (priority 2, created in main.cpp:307) wakes every 100 ms. Once connected() is true it takes mqttMutex and calls parseMessage()/read() (mqtt.cpp:55-84) on the same client, preempting the connectivity task (priority 1) while it is still inside the subscribe poll loop. logAndPublish (logging.cpp:25-31) also publishes under the try-lock while the connectivity task holds no lock. Two tasks driving one parser is a real race. Retained messages arriving right after each SUBSCRIBE make it likely on every reconnect. The suggested fix (lock around connect plus subscribe, or let a single task own the client) is correct.

### A5. Log rotation deletes the whole log if the temp file cannot be created

**Severity:** medium · **Verdict:** CONFIRMED · **Area:** concurrency  
**Location:** [`src/SDCard.cpp:265`](../src/SDCard.cpp#L265)

**What:** In addLogToSDCard the rotation opens /temp_log.txt and copies into it only `if (tempFile)`. It then runs SD_MMC.remove(logFilename) and SD_MMC.rename("/temp_log.txt", logFilename) without checking whether the temp file was created or fully written. tempFile.write() return values are also ignored, so a short write (card full) still replaces the original.

**Failure scenario:** The card is full or the FAT has a transient error once a log goes past 1 MB. SD_MMC.open("/temp_log.txt", FILE_WRITE) fails, the original 1 MB log is removed, the rename fails, and the whole normal or error history is lost. *lineCountPtr is set to linesToKeep even though the file is now empty or missing, so later cached counts are wrong.

**Suggested fix:** Only remove and rename when tempFile opened and every write returned bytesRead. Otherwise close it, delete the partial temp file and leave the original log in place, perhaps truncating by another method later. Set *lineCountPtr only after a successful rename, and set it to the number of lines actually copied.

**Verifier notes:** Confirmed at SDCard.cpp:252-268. The copy into the temp file only runs `if (tempFile)`, but SD_MMC.remove(logFilename) and the rename at lines 265-266 run either way, and tempFile.write() return values are never checked. If the temp file fails to open, or the card fills up during the copy, the original log is deleted and either nothing or a truncated file takes its place. *lineCountPtr is still set to linesToKeep (line 269), which does not match what is on the card. The fix is to remove and rename only after every write has succeeded, delete the partial temp file otherwise, and set the line count from the lines actually copied.

### A6. First /api/logs request counts lines byte-by-byte over up to 1 MB on the loop (LVGL) task while holding sdMutex

**Severity:** medium · **Verdict:** CONFIRMED · **Area:** concurrency  
**Location:** [`src/SDCard.cpp:343`](../src/SDCard.cpp#L343)

**What:** getLogsFromSDCard runs inside webServer.handleClient() on the loop task, the same task that runs lv_timer_handler and is subscribed to the watchdog. If the cached count is <= 0, it calls countLinesInFile(), which does f.available() + f.read() one byte at a time over the whole file (MAX_LOG_FILE_SIZE = 1 MB) while holding sdMutex. errorLogLineCount stays -1 until the first errorPublish of this boot, so this slow path is common for /api/logs/error. The same per-byte count also runs in addLogToSDCard (line 202) on the first log write of each file.

**Failure scenario:** The device boots, no error has been logged yet, and the user opens the log viewer, which fetches /api/logs/error. The loop task spends several seconds reading about 1M single bytes through VFS (one ftell and one fread each). The UI and touch are frozen for that time. Meanwhile the MQTT and API tasks that call saveDataBlock() wait on sdMutex, and if the count takes longer than MUTEX_TIMEOUT_SD_MS (5 s) their saves are silently dropped. Reading 500 lines one char at a time afterwards (line 400) adds more time.

**Suggested fix:** Count lines with 512-byte block reads and memchr, or cache the count at boot in sdcard_init so the web handler never has to count. Read the tail lines with block reads as well. Keep the time spent holding sdMutex on the loop task short, or move log JSON generation off the loop task.

**Verifier notes:** Confirmed. countLinesInFile (SDCard.cpp:27-34) reads the file one byte at a time with available()+read(). In Arduino v2 VFS each available() call is an ftell/size lookup and each read() is a 1-byte fread, run over files up to MAX_LOG_FILE_SIZE (1 MB). getLogsFromSDCard runs on the loop task through webServer.handleClient() (main.cpp:326) and counts while holding sdMutex if errorLogLineCount is still -1 (line 343). That stays true until the first error is logged in this boot. The same slow count runs on the SD logger task at line 202 for the first write after boot, also under sdMutex, which can make saveDataBlock time out (MUTEX_TIMEOUT_SD_MS = 5 s). How long it actually takes depends on the card, but block reads with memchr are clearly better.

### A7. GT911 read overflows TAMC_GT911::points[5] (heap corruption) on any I2C read failure

**Severity:** medium · **Verdict:** CONFIRMED · **Area:** display-board  
**Location:** [`src/board.cpp:150`](../src/board.cpp#L150)

**What:** touchRead() calls ts->read() from the TAMC_GT911 library about 30 times a second. In that library, read() does `pointInfo = readByteData(0x814E)`. When the controller NACKs, Wire.read() returns -1, so pointInfo becomes 0xFF. That sets bufferStatus=1 and touches = 0xFF & 0xF = 15. The loop `for (i < touches) points[i] = readPoint(data)` then writes 15 entries into `TP_Point points[5]` (TAMC_GT911.h:165), which is about 80 bytes past the array. The object was allocated with `new` in display_hardware_init, so this corrupts the heap. The touchAvailable check only covers a missing chip at boot, not a later bus error. Your own memory note about the i2cRead Error -1 flood describes exactly this condition: every one of those polls was overflowing the array.

**Failure scenario:** The device runs normally. The GT911 then misses one transaction: an ESD event or a brown-out reset of the touch chip, a glitch on the I2C bus that the Waveshare board shares with the CH32V003 expander, or the address re-latching. read() sees 0xFF and writes 10 extra TP_Point entries into adjacent heap memory. The result is a later crash (heap assertion or LoadProhibited) somewhere unrelated, or a phantom press at x=65535.

**Suggested fix:** Stop calling ts->read(). Write a small read in touchRead(): read register 0x814E and check that Wire.requestFrom(addr,1)==1. Accept the data only if (status & 0x80) is set and n = status & 0x0F is between 1 and 5. Then read 8 bytes from 0x814F, check the requestFrom return again, and write 0 back to 0x814E. Alternatively, vendor a patched copy of TAMC_GT911 that clamps touches to 5 and checks requestFrom. Count consecutive failures and set touchAvailable=false (or re-probe) after N of them, so a dead controller doesn't spin the bus.

**Verifier notes:** The overflow is real, but it is not heap corruption. In TAMC_GT911.cpp:69-86, if requestFrom fails then Wire.read() returns -1, so pointInfo becomes 0xFF, bufferStatus is 1 and touches is 15. The loop then writes points[0..14]. In TAMC_GT911.h:165-185, points[5] (5x8 bytes) is followed inside the same object by rotation, addr, pinSda/Scl/Int/Rst, width, height and configBuf[185]. The 80 extra bytes therefore land on those fields and the start of configBuf, and never leave the object. The real effect: one transient NACK clobbers `addr` (with padding or garbage) and `rotation`/`width`/`height`. Every later poll then goes to the wrong address and NACKs again, so touch stays dead and the log floods until reboot. The same poll also reports a phantom press at about 65535. The fix as proposed (own read with requestFrom checks, clamp touches to 1..5, failure counter) is correct. Downgraded from high because it needs a bus glitch to trigger and it does not crash.

### A8. On Waveshare, the SD card is mounted before the expander drives SD CS (IO4)

**Severity:** medium · **Verdict:** PLAUSIBLE · **Area:** display-board  
**Location:** [`src/main.cpp:196`](../src/main.cpp#L196)

**What:** sdcard_mount() runs at main.cpp:196. The Waveshare expander, which owns SD CS/D3 on IO4, is only configured later: display_hardware_init() → pinInit() → waveshare_expander_init() (main.cpp:262), and that is the first place outShadow drives IO4 low. Until then, the level on IO4 is the CH32V003's power-on default. Your notes say the SD card only works once IO4 is driven low by the expander. On a warm reset, the expander keeps the state from the previous run, which hides the problem.

**Failure scenario:** After a cold power-on of a Waveshare board, the expander's default IO4 level differs from what SD_MMC 1-bit needs. sdcard_mount() fails, so no SD state is restored (solar, weather, readings and forecast all show "--" until refetched) and SD logging is disabled for the whole run. After a software or OTA reboot the same board works, so the fault looks intermittent.

**Suggested fix:** Initialise the expander right after board detection and before sdcard_mount(). For example, call waveshare_expander_init() (or a smaller waveshare_expander_early_init() that sets IO modes and drives IO4) in setup() straight after `board = ...`, and remove the call from pinInit(). Keep the backlight off until display init, as it is now.

**Verifier notes:** Confirmed ordering: sdcard_mount() at main.cpp:196 runs before display_hardware_init() → pinInit() → waveshare_expander_init() at main.cpp:262. Only waveshare_expander_init (board_waveshare.cpp:35-39) sets all IOs to output and drives IO4 low. The project notes say SD works 'once IO4 (SD CS) is driven low by the expander', so on a cold boot SD depends on the CH32V003's power-on default for IO4. The expander keeps its state across a warm reset, which would hide the problem. This ordering has existed since 95318f7. I could not verify the CH32V003 default IO state, so this stays plausible rather than confirmed. Moving the expander init (IO modes and IO4 low, backlight off) to straight after board detection is low-risk.

### A9. Freshness indicators show green when data has never arrived or the clock isn't synced

**Severity:** medium · **Verdict:** CONFIRMED · **Area:** display-board  
**Location:** [`src/display.cpp:263`](../src/display.cpp#L263)

**What:** setStatusColor computes `time(nullptr) - updateTime > maxAgeSec`. When updateTime == 0 (never fetched, or cleared by invalidateStaleApiData) and the clock is unsynced, `now` is the seconds since boot, so the age is small and the indicator is green. With restored SD timestamps and an unsynced clock, the age is hugely negative, which also gives green. display_init() sets these red, but the first updatePeriodicStatus() pass a second later overwrites that.

**Failure scenario:** The device boots with WiFi or NTP down. One second later ui_WeatherStatus turns green and stays green for 600 s (2×300), and ui_SolarStatus for 120 s, with no data ever fetched. After an SD restore, all indicators stay green until NTP syncs, even though the data may be days old.

**Suggested fix:** Treat `updateTime == 0` or `now < TIME_SYNC_THRESHOLD` as stale (or as a neutral grey 'unknown'): `bool stale = updateTime == 0 || now < TIME_SYNC_THRESHOLD || now - updateTime > maxAgeSec;`.

**Verifier notes:** setStatusColor (display.cpp:263-266) computes time(nullptr) - updateTime > maxAgeSec with no check on updateTime == 0 or on clock sync. After a cold boot with no NTP, now is seconds since boot, so with updateTime == 0 the indicators turn green within 1 s (display_init's red is overwritten at display.cpp:528-531) and stay green for 120/600/7200/3600 s. Timestamps restored from SD give a negative age, so the indicators show green until NTP syncs, possibly indefinitely. The suggested guard (updateTime == 0 or now < TIME_SYNC_THRESHOLD means stale) is correct.

### A10. loadDataBlock reads straight into the live global, so a failed load leaves it partly or fully corrupted

**Severity:** medium · **Verdict:** CONFIRMED · **Area:** memory  
**Location:** [`src/SDCard.cpp:162`](../src/SDCard.cpp#L162)

**What:** loadDataBlock() calls readBytes() directly into dataPtr, which is the real global (solarToken, weather, uv, solar, airQuality, insideAirQuality, solarForecast in main.cpp:210-248). The checksum is checked only after the bytes are already in place (line 176). If the checksum fails or the read is short (line 166), the function returns false but leaves the struct overwritten. Only restoreReadings() (main.cpp:70) protects itself with a temp buffer. The other seven callers don't.

**Failure scenario:** /solar_token.bin has the right size but a bit error or a stale sector, so the checksum fails. setup() logs 'Solar token restore failed', but solarToken.token now holds 2048 arbitrary bytes with no NUL guaranteed. APIs.cpp:622 strlen(solarToken.token) reads past the struct into neighbouring globals, and if the result is non-zero the garbage is sent as the Authorization header. In the same way, a corrupted weather_data.bin leaves weather.description, windDir and sunrise without a terminator and updateTime as a random non-zero value. updateWeatherDisplay() then shows it, and lv_label_set_text(ui_FCConditions, weather.description) reads past the field. A short file (power lost mid-write) overwrites only the first N bytes, giving a mix of old and default fields.

**Suggested fix:** Make loadDataBlock always read into a scratch buffer (heap_caps_malloc in PSRAM, as saveDataBlock does). Copy into dataPtr only after the size, length and checksum checks pass, and do that copy under dataMutex. Afterwards, force NUL-termination of the char[] fields in each struct (e.g. weather.description[sizeof-1] = 0), or add a small per-struct sanitize() step, so a file that passes the checksum but has a different layout still can't produce unterminated strings.

**Verifier notes:** SDCard.cpp:162 readBytes goes straight into dataPtr, and the size/checksum checks at 166/176 only return false afterwards. main.cpp:210-248 pass the live globals for seven structs; only restoreReadings uses a temp buffer. A short or bad-checksum file therefore leaves the struct partly or fully overwritten, and string fields such as solarToken.token or weather.description may have no terminator (APIs.cpp:622 does strlen on it). Lowered to medium because it needs SD corruption or an interrupted save, all loads run once in setup() before any task starts, and the next successful fetch overwrites the bad data (an invalid token is also cleared at APIs.cpp:396). The fix does not need dataMutex for the copy, since loads only happen before the tasks are created.

### A11. Persisted state is truncated before rewrite, so an interrupted save loses the previous good copy

**Severity:** medium · **Verdict:** CONFIRMED · **Area:** memory  
**Location:** [`src/SDCard.cpp:74`](../src/SDCard.cpp#L74)

**What:** writeDataBlock opens the real file with FILE_WRITE, which truncates it, then writes the header and the data. There's no temp file and rename, and no flush check. If power is lost or the card fails between the truncate and close(), you get an empty or short file. Saves happen often: solar every 60 s (3 calls per pass), weather every 300 s, readings and inside AQ every 300 s. So the risk window comes up again and again on a mains device that sees power cuts.

**Failure scenario:** Mains drops (load-shedding) while fetchCurrentSolar() is saving /solar_data.bin. On the next boot loadDataBlock reads a short header or short data and fails. Combined with the finding above, the live struct is half-overwritten, and the last good copy is gone even though it was valid a second earlier. The same applies to /solar_token.bin, which forces a token re-fetch, and /readings_data.bin, which loses all room history.

**Suggested fix:** Write to "<name>.tmp", call dataFile.flush(), check both write lengths, close, then SD_MMC.remove(name) and SD_MMC.rename(tmp, name). FAT rename isn't fully atomic, but this narrows the window to a directory update. On load, if the main file is missing or invalid, fall back to a valid .tmp file.

**Verifier notes:** SDCard.cpp:74 opens the real file with FILE_WRITE, which truncates it, then writes the header and data straight in. There is no temp file and rename. Saves happen often (APIs.cpp:469/533/595 for solar, and readings/AQ/forecast from mqtt.cpp), so a power cut during a save loses the last good copy. Medium is reasonable for a mains device.

### A12. Log rotation deletes the log when the temp file can't be created, and leaves the line-count cache wrong

**Severity:** medium · **Verdict:** CONFIRMED · **Area:** memory  
**Location:** [`src/SDCard.cpp:265`](../src/SDCard.cpp#L265)

**What:** If SD_MMC.open("/temp_log.txt", FILE_WRITE) fails, the code still runs SD_MMC.remove(logFilename) and rename() (lines 265-266), and neither return value is checked. Partial copies aren't detected either: tempFile.write()'s return value is ignored. After rotation *lineCountPtr is set to linesToKeep, which is at least 100, whatever the new file actually contains.

**Failure scenario:** The card is nearly full or the FAT has an error, so creating the temp file fails during rotation. The original 1 MB normal_log.txt is deleted, rename fails, and all history is lost. The cache now claims, say, 7000 lines for an empty file. getLogsFromSDCard() (line 339-352) trusts the cache, computes linesToSkip = 7000 + n - 500, skips through the whole file, and /api/logs/normal returns an empty list until reboot.

**Suggested fix:** Only remove and rename if tempFile opened and every write returned bytesRead. Otherwise leave the original in place, for example by truncating it in place as a fallback. Check the remove() and rename() return values. After a successful rotation, set the cache from the lines actually copied (count '\n' during the copy) or set it to -1 to force a recount.

**Verifier notes:** SDCard.cpp:252-266: if opening /temp_log.txt fails, the code still runs SD_MMC.remove(logFilename) and rename, so the whole log is deleted. Neither return value is checked, and tempFile.write's result is ignored. Line 269 sets *lineCountPtr = linesToKeep whatever was actually copied. getLogsFromSDCard (339-352) trusts any cache value above 0 and skips that many lines, so the small new file is skipped entirely and the viewer shows nothing until the real line count catches up or the device reboots.

### A13. Line counting and rotation skipping read the log one byte at a time while holding sdMutex

**Severity:** medium · **Verdict:** CONFIRMED · **Area:** memory  
**Location:** [`src/SDCard.cpp:27`](../src/SDCard.cpp#L27)

**What:** countLinesInFile() and the rotation skip loop (lines 246-249) call File::read() once per byte, which is a VFS fread for each byte, across up to 1 MB. Both run while holding sdMutex. The first addLogToSDCard after boot always triggers countLinesInFile (cache = -1), and each rotation skips about 25% of the file this way. getLogsFromSDCard (line 397-406) also reads lines byte by byte on the loop task, which drives LVGL.

**Failure scenario:** At boot normal_log.txt is close to 1 MB. The SD Logger task (priority 0) spends seconds counting lines while holding sdMutex. saveDataBlock() calls from the API Manager and MQTT tasks wait MUTEX_TIMEOUT_SD_MS (5 s) and then return false with nothing logged, so that state silently never gets persisted. If someone opens /logs during a rotation, the loop task blocks for up to 5 s on sdMutex and the UI freezes.

**Suggested fix:** Use the same 512-byte block read that getLogsFromSDCard already uses for skipping: read(buf, 512) and memchr/count '\n' in the buffer. Do this for countLinesInFile, for the rotation skip (seek to the found offset), and for reading lines in getLogsFromSDCard. Consider logging when saveDataBlock fails because of a mutex timeout.

**Verifier notes:** countLinesInFile (27-34) and the rotation skip (246-249) call File::available() and File::read() once per byte. On Arduino-ESP32 v2.x that is an fread plus an ftell/size call per byte, over files up to MAX_LOG_FILE_SIZE (1 MB), all while holding sdMutex. The first addLogToSDCard after boot always counts (cache -1). writeDataBlock gives up after MUTEX_TIMEOUT_SD_MS (5000 ms) and returns false without logging anything, and the API code ignores saveDataBlock's return value. The multi-second duration is plausible but I have not measured it. getLogsFromSDCard also reads lines byte by byte on the loop task (397-406). The buffered fix matches what getLogsFromSDCard already does for skipping.

### A14. Waveshare: SD card is mounted and state restored before the expander drives SD CS (IO4)

**Severity:** medium · **Verdict:** PLAUSIBLE · **Area:** memory  
**Location:** [`src/main.cpp:196`](../src/main.cpp#L196)

**What:** setup() calls sdcard_mount() and all the loadDataBlock restores (lines 196-253) before display_hardware_init() (line 262). On Waveshare, display_hardware_init() is what calls waveshare_expander_init(), and that function drives the expander's IO4 (SD CS) low. The project notes say SD only works on this board once IO4 is driven low.

**Failure scenario:** On a cold power-up the expander MCU starts with its IOs in their default state (IO4 not driven low). SD_MMC.begin fails, 'SD Card initialization failed!' is logged, and no persisted state is restored: no solar token, weather or readings. The first saveDataBlock then fails to open, remounts and succeeds, so the card looks healthy afterwards. A warm esp_restart() works because the expander keeps its state, which hides the problem.

**Suggested fix:** On Waveshare, call waveshare_expander_init() (or a small helper that just sets the mode register and drives IO4 low) right after detectWaveshare(), before sdcard_mount(). Check on hardware by cold-booting with the serial log attached.

**Verifier notes:** main.cpp:196-253 mounts SD and restores state before display_hardware_init() (line 262). That function reaches pinInit() and waveshare_expander_init() (board.cpp:84), which drives every expander output, including IO4/SD CS, low (board_waveshare.cpp:35-39). The project notes say SD only works once IO4 is driven low. On a cold power-up the expander would be at its defaults during mount, while a warm restart keeps its state. The order is the same in the previous commit (c7df130), so this is not a regression from the split. Whether the expander's power-on default actually stops SD from mounting needs a cold-boot check on hardware.

### A15. mqtt_connect() runs connect/subscribe without mqttMutex, racing the receive task, and subscribe() drops retained messages

**Severity:** medium · **Verdict:** CONFIRMED · **Area:** networking  
**Location:** [`src/connections.cpp:47`](../src/connections.cpp#L47)

**What:** mqtt_connect() calls mqttClient.connect() and then about 9 mqttClient.subscribe() calls without taking mqttMutex. In ArduinoMqttClient 0.1.8, subscribe() waits for SUBACK by calling poll() in a loop (MqttClient.cpp:349-358). Two problems follow. (1) The receive task runs at priority 2 and the connectivity task at priority 1. As soon as connected() is true, the receive task can preempt in the middle of subscribe()'s poll(), take mqttMutex (nobody else holds it) and call parseMessage()/poll() on the same object. Both tasks then change _rxState, _rxLength and the _rxMessageTopic String at the same time. Publishes from logAndPublish() in other tasks take the mutex, but subscribe() writes to the socket without it, so packets can interleave on the wire. (2) The broker sends the retained PUBLISH for topic N straight after SUBACK N. With no onMessage callback, the next subscribe(N+1) call's poll() reaches that PUBLISH while in READ_PUBLISH_PAYLOAD state and discards the payload byte by byte (MqttClient.cpp:615-627). Only the last-subscribed topic (solar/forecast) reliably keeps its retained value.

**Failure scenario:** After a broker or WiFi reconnect, each subscribe round-trip takes a few ms while the receive task only wakes every 100 ms. Retained values on readings[i].topic and the kitchen/co2|pm*/set topics are consumed and dropped inside mqtt_connect(), so those tiles stay stale or '--' until the sensor next publishes. If the receive task preempts mid-poll(), the rx state machine can desync: a malformed packet leads to stop() and another reconnect cycle, or a torn _rxMessageTopic gets read.

**Suggested fix:** Hold mqttMutex for the whole connect+subscribe sequence (xSemaphoreTake with a bounded wait; the receive task already tolerates a 500 ms miss). Also register mqttClient.onMessage(handler) and move the topic dispatch into that callback, reading the payload with mqttClient.read() inside it. PUBLISH packets that arrive during subscribe()'s internal poll() then get dispatched instead of discarded. The receive task then only needs to call mqttClient.poll() under the mutex.

**Verifier notes:** mqtt_connect() calls connect() and subscribe() without taking mqttMutex (connections.cpp:48-71). In ArduinoMqttClient 0.1.8, subscribe() waits for SUBACK in a poll() loop (MqttClient.cpp:349-358). The receive task (main.cpp:307) runs at priority 2 on the same core and the connectivity task at priority 1. Once _connected is true after CONNACK, the receive task can preempt in the middle of poll(), take the mutex (nothing else holds it) and call parseMessage()/poll() on the same object, so the race is real. The retained-message loss is also real. Without an onMessage callback, poll() in READ_PUBLISH_PAYLOAD state discards payload bytes (MqttClient.cpp:616-627). A retained PUBLISH that arrives after SUBACK N is therefore consumed by subscribe(N+1)'s poll loop, which spins with yield() and does not leave time for the receive task's 100 ms cycle. The impact depends on whether the sensors publish with the retain flag.

### A16. Solar token refresh clears a still-valid token before fetching, so a failed refresh stops all solar updates

**Severity:** medium · **Verdict:** CONFIRMED · **Area:** networking  
**Location:** [`src/APIs.cpp:624`](../src/APIs.cpp#L624)

**What:** When the token reaches SOLAR_TOKEN_REFRESH_SEC (12 h), the code sets solarToken.token[0] = '\0' before calling fetchSolarToken(). Tokens last about 24 h, so the old token is still usable. If the refresh fails (Solarman auth endpoint down, HTTP 5xx, timeout), strlen(solarToken.token)==0 and the whole solar block at line 663 is skipped until a refresh succeeds. Backoff runs 30 s, 60 s, ... up to 300 s.

**Failure scenario:** At the 12-hour mark Solarman's /account/v1.0/token endpoint returns 502 for an hour while /station/v1.0/realTime still works. Solar power, battery SoC and daily/monthly totals freeze for that hour (and go stale on screen), even though the existing token would have worked for another ~12 hours.

**Suggested fix:** Only clear the token when it is actually empty or has been rejected ('auth invalid token' already clears it at line 396). For age-based refresh, fetch into a temporary buffer and copy it over solarToken.token only on success. For example, drop line 624 and have fetchSolarToken() write the new token directly (it already does) without clearing beforehand.

**Verifier notes:** APIs.cpp:624 clears solarToken.token before attemptFetch(fetchSolarToken). fetchSolarToken() does not need the old token cleared: it only overwrites the token on success (line 331). On failure the token stays empty, so the solar block guarded by strlen()>0 (line 663) is skipped until a refresh succeeds, with backoff of up to API_MAX_BACKOFF_SEC. The fix is to delete line 624. Age-based refresh then keeps using the old token, and the 'auth invalid token' path (line 396) still clears it when needed.

### A17. OTA re-downloads the full image every 5 minutes, indefinitely, when a newer version cannot be installed

**Severity:** medium · **Verdict:** CONFIRMED · **Area:** networking  
**Location:** [`src/OTA.cpp:24`](../src/OTA.cpp#L24)

**What:** lastOTAUpdateCheck is set before updateFirmware() runs, and nothing records that the update failed. If the version file advertises a newer version but the binary fails Update.end() (image validation fails because of a wrong-chip build, a corrupt upload or a truncated file) or the download keeps stalling, the next check 300 s later (CHECK_UPDATE_INTERVAL_SEC) downloads the whole binary again. During each download api_manager_t is blocked, so weather, solar and UV fetches all stop for the length of the download.

**Failure scenario:** A bad .bin is published next to a bumped version file. Every device downloads ~2-3 MB every 5 minutes (~288 times a day, ~700 MB/day per device), flash-writes the OTA partition each time (wear), and spends a large share of every 5-minute window not refreshing any API data, until someone fixes the server.

**Suggested fix:** Remember the version that failed (a static char[] or NVS) and skip it, or apply an exponential/24-hour backoff for the same serverVersion after a failed updateFirmware(). Have updateFirmware() return bool so checkForUpdates() can record the failure.

**Verifier notes:** lastOTAUpdateCheck is set at OTA.cpp:24 before updateFirmware(), and nothing records a failed attempt. A bad magic byte fails on the first Update.write() and aborts quickly. But a chip-ID or image-validation failure is only detected in Update.end() (esp_ota_end), after the full download, and a stalled download also gives up only partway through. Both repeat every CHECK_UPDATE_INTERVAL_SEC=300 s while api_manager_t is blocked. There is a related worse case: if the version file is bumped before the new .bin is uploaded, the old image installs successfully and reboots. lastOTAUpdateCheck is 0 after boot, so the device checks immediately and repeats the cycle, which is a reboot loop. Remembering the failed or installed serverVersion fixes both.

### A18. /update and /reboot web endpoints have no authentication and can be triggered cross-site

**Severity:** medium · **Verdict:** CONFIRMED · **Area:** networking  
**Location:** [`src/web_server.cpp:81`](../src/web_server.cpp#L81)

**What:** POST /update accepts and flashes any firmware image, and POST /reboot restarts the device, with no credentials. Both are plain form POSTs, so a web page open in any browser on the LAN can submit them (CSRF): the device needs no preflight and does not check Origin. In addition, the web upload uses the same global Update singleton as updateFirmware() in the API task. A web upload that starts while an HTTP OTA is in progress calls Update.abort() on it (line 101), and their writes can interleave.

**Failure scenario:** Any device or browser tab on the home network can POST /reboot in a loop, keeping the display permanently rebooting, or flash arbitrary firmware through /update (which would also expose the WiFi/MQTT/Solarman credentials compiled into the image).

**Suggested fix:** Add HTTP basic/digest auth with webServer.authenticate(user, pass) / requestAuthentication() on /update (GET and POST) and /reboot, using credentials from config.h. Optionally also check the Origin/Referer header. Reject a web upload while Update.isRunning() is true because of an in-progress HTTP OTA, instead of aborting it.

**Verifier notes:** web_server.cpp:37-42 and 81-124 register /reboot and /update POST with no authenticate() check. Multipart and urlencoded form POSTs are CORS 'simple requests', so a cross-site page can trigger them without a preflight. The secondary point is real but rare: the web upload handler (loop task) calls Update.abort() and Update.begin() on the global Update singleton (lines 100-103), and the API task can be mid-download using the same singleton in OTA.cpp. Medium severity is appropriate for a device on a home LAN.

### A19. Log viewer request blocks the LVGL loop while scanning up to 1 MB of SD under sdMutex

**Severity:** medium · **Verdict:** CONFIRMED · **Area:** structure  
**Location:** [`src/SDCard.cpp:355`](../src/SDCard.cpp#L355)

**What:** webServer.handleClient() runs in loop() (main.cpp:326), so getLogsFromSDCard runs on the display task. To skip to the last 500 lines it reads forward through the whole file (355-373), up to the 1 MB rotation limit. It then reads the 500 lines one character at a time (397-406), all while holding sdMutex, and finally builds a buffer of up to 200 KB that is copied into a String. During this time LVGL is not serviced and the SD logger and saveDataBlock callers are blocked.

**Failure scenario:** Someone opens /logs with a nearly full normal_log.txt. The screen clock and touch freeze for the duration of a 1 MB SD read (typically one to a few seconds in 1-bit SD_MMC mode). Concurrent saveDataBlock calls from the API or MQTT task wait on sdMutex and can time out after 5 s, dropping a state save.

**Suggested fix:** Seek near the end instead of scanning from the start: `logFile.seek(max(0, size - MAX_LOG_ENTRIES_TO_READ * AVG_LINE_BYTES))`, discard the first partial line, then read in 512-byte blocks. Release sdMutex as soon as the raw bytes are in PSRAM and do the JSON escaping afterwards. Consider streaming the response with webServer.sendContent() in chunks instead of building a 200 KB buffer and then a String copy.

**Verifier notes:** webServer.handleClient() runs in loop() (main.cpp:326). getLogsFromSDCard skips forward through up to 1 MB in 512-byte reads (355-373), then reads up to 500 lines one char at a time (397-406), all while holding sdMutex, and LVGL is not serviced in that time. Viewing /logs with a near-full log freezes the UI for roughly a second and blocks SD saves. Seeking near the end of the file is a sound fix.

### A20. mqtt_connect can block far longer than the 60 s task watchdog (30 s CONNACK timeout plus 20 subscribes at 30 s each, one wdt reset)

**Severity:** low · **Verdict:** PLAUSIBLE · **Area:** concurrency  
**Location:** [`src/connections.cpp:56`](../src/connections.cpp#L56)

**What:** The watchdog is fed once at the top of mqtt_connect(). MqttClient's _connectionTimeout defaults to 30 000 ms (MqttClient.cpp:71) and is never changed (no setConnectionTimeout call anywhere). connect() waits up to 30 s for CONNACK after DNS and the TCP connect. Each of the 20 subscribe() calls (15 readings plus 5 other topics) then waits up to 30 s for its SUBACK. The Connectivity task is subscribed to the TWDT.

**Failure scenario:** The broker accepts TCP but answers slowly (for example it is overloaded, or a half-open NAT path drops some packets). connect() takes about 25 s to get CONNACK, then the first two SUBACKs take about 20 s each. More than 60 s pass without esp_task_wdt_reset(), and the task watchdog reboots the display. Even when connect() simply fails, the path is DNS (up to about 15 s) + TCP connect (about 3 s) + 30 s CONNACK + 5 s retry delay + 1 s + 5 s, roughly 59 s, which is right at the limit.

**Suggested fix:** Call mqttClient.setConnectionTimeout(5000) (or similar) once in setup. Call esp_task_wdt_reset() before connect() and inside both subscribe loops. If a subscribe fails, abort the rest of the loop: subscribe() has already called stop(), so later subscribes fail anyway after each waiting out its timeout.

**Verifier notes:** The finding overstates this. publishMessageInternal calls esp_task_wdt_reset() whenever it gets the mqttMutex (logging.cpp:26). So the logAndPublish calls at connections.cpp:46 (before connect), :49 (on failure) and :54 (after CONNACK), plus errorPublish on any subscribe failure, all feed the watchdog. The mutex is almost always free when the Connectivity task runs. The 20x30 s worst case also cannot happen: a timed-out subscribe calls stop() (MqttClient.cpp:360), and every later subscribe fails at once in endPacket(). The connect window is about DNS 15 s + TCP 3 s + CONNACK 30 s, which is under 60 s. The only remaining risk is several slow but successful SUBACKs adding up to more than 60 s with no feed. That is possible but unlikely. Calling setConnectionTimeout(5000) and feeding the watchdog in the subscribe loop are still cheap hardening, and it helps to stop relying on logAndPublish's hidden feed.

### A21. OTA path is the deepest stack use in the 8 KB API Manager task and HWM sampling never covers it

**Severity:** low · **Verdict:** PLAUSIBLE · **Area:** concurrency  
**Location:** [`src/OTA.cpp:29`](../src/OTA.cpp#L29)

**What:** checkForUpdates() keeps url[255], two logMessage[255] buffers and a String on its frame while it calls updateFirmware(). updateFirmware adds binUrl[255], buff[128] and logMessage buffers, and then runs a full mbedTLS handshake (several KB of stack) from that deeper point. Normal fetches handshake from a much shallower frame. The hourly HWM log at APIs.cpp:609-614 samples the high-water mark at the top of the loop, so it reflects normal fetches only. The OTA download path runs only when a new version exists, so it is effectively never measured before it matters.

**Failure scenario:** A new firmware is published. The API task overflows its 8192-byte stack during the second TLS handshake inside updateFirmware, which triggers a stack-canary panic and a reboot. On the next boot lastOTAUpdateCheck is 0, the check runs again, and the device boot-loops for as long as the server advertises the new version.

**Suggested fix:** Have checkForUpdates() return a flag and call updateFirmware() from api_manager_t after checkForUpdates' frame is gone. Make binUrl/url/buff static, since only one task uses them. Log uxTaskGetStackHighWaterMark() right after the OTA GET once to confirm the headroom, or raise the API Manager stack to TASK_STACK_LARGE.

**Verifier notes:** The code is as described. checkForUpdates keeps url[255], logMessage[255] and a String on its frame while it calls updateFirmware, and updateFirmware adds binUrl[255], buff[128] and logMessage buffers, then does a fresh TLS handshake. That is roughly 0.7-1 KB deeper than the version-check handshake. Since uxTaskGetStackHighWaterMark is a lifetime minimum, the hourly HWM log does include the version-check handshake made from inside checkForUpdates. Only the extra updateFirmware depth goes unmeasured. Whether 8192 bytes overflows cannot be confirmed without HWM data. Calling updateFirmware from api_manager_t after checkForUpdates returns (via a flag) is a cheap, sensible change.

### A22. Global Update object shared by the web upload handler (loop task) and the API task OTA with no coordination

**Severity:** low · **Verdict:** CONFIRMED · **Area:** concurrency  
**Location:** [`src/web_server.cpp:100`](../src/web_server.cpp#L100)

**What:** The /update POST upload handler runs on the loop task and calls Update.abort() if Update.isRunning(), then Update.begin(). updateFirmware() on the API task calls Update.begin/write/end/abort on the same singleton (OTA.cpp:59, 80, 105, 108). Neither side checks for or blocks the other.

**Failure scenario:** A user uploads firmware through the web page while the API task's periodic OTA download is running (or the other way round). The web handler's UPLOAD_FILE_START aborts the API session. The API task's next Update.write fails and it calls Update.abort() (OTA.cpp:105), which now aborts the web session halfway through flashing. Both updates fail and the user gets "FAIL" for no visible reason. Interleaved writes from the two tasks into the same session can also produce a corrupt image that fails verification.

**Suggested fix:** Guard OTA with a std::atomic<bool> otaInProgress (or a mutex) checked with compare_exchange in both paths. Reject the web upload, or skip the API check, while the other is active.

**Verifier notes:** Confirmed. web_server.cpp:100-104 aborts any running Update session and calls begin() on the loop task, while OTA.cpp:59-108 uses the same global Update singleton from the API task. There is no flag or lock between them. The trigger needs a manual web upload at the same moment a server-advertised OTA is downloading, which is rare, so low severity is right. An atomic otaInProgress guard is cheap.

### A23. Global struct tm timeinfo written by both the Connectivity task and the loop task

**Severity:** low · **Verdict:** CONFIRMED · **Area:** concurrency  
**Location:** [`src/connections.cpp:75`](../src/connections.cpp#L75)

**What:** time_init() (Connectivity task) calls getLocalTime(&timeinfo) and strftime on the global timeinfo. updatePeriodicStatus() (display.cpp:551-555) and onTimeTouched() (display.cpp:596-598) write and read the same global from the loop task. No lock protects it.

**Failure scenario:** WiFi is restored at the moment the loop is formatting the clock. The loop task is preempted between getLocalTime() filling timeinfo and strftime() reading it, and the Connectivity task overwrites the struct. The clock (or the logged sync time) shows a mixed value for one tick. Harmless today, but the code invites future misuse; APIs.cpp:417 already avoids it for this reason.

**Suggested fix:** Use a local struct tm in time_init() and in the display functions, and remove the global timeinfo (it is only used as scratch space).

**Verifier notes:** Confirmed. The global timeinfo (main.cpp:37) is written by time_init on the Connectivity task (connections.cpp:75-82) and by updatePeriodicStatus/onTimeTouched on the loop task (display.cpp:551-555, 596-598), with no lock. It is only used as scratch space, so the worst case is one wrong clock frame. Replacing it with a local struct tm in each function and removing the global is trivial cleanup.

### A24. esp_log vprintf hook puts a 256-byte buffer on the stack of whatever task logs, including small system tasks

**Severity:** low · **Verdict:** PLAUSIBLE · **Area:** concurrency  
**Location:** [`src/main.cpp:133`](../src/main.cpp#L133)

**What:** On Waveshare boards, esp_log_set_vprintf installs a lambda with char buf[256] on the stack that calls Serial.write (HWCDC). ESP-IDF calls this hook from any task that uses ESP_LOGx, including WiFi, lwIP/tcpip, esp_timer and event-loop tasks, which have small fixed stacks. HWCDC::write may also block on its TX semaphore or timeout when the USB host is not reading.

**Failure scenario:** A WiFi or TLS warning is logged from a system task that is already deep in its stack (for example esp_timer at 3.5 KB, or the sys_evt task during a disconnect storm). The extra 256 bytes plus the HWCDC write path overflow that task's stack, which causes a stack-overflow panic in a task the application does not own. Alternatively, a disconnected USB host makes each log call block for the HWCDC TX timeout inside the WiFi or lwIP task, which stalls networking.

**Suggested fix:** Use a smaller buffer, or skip the buffer and write via vfprintf to the CDC stream. Set a short HWCDC TX timeout (Serial.setTxTimeoutMs(0)) so system tasks never block on USB.

**Verifier notes:** The code is as described (main.cpp:133-140): a 256-byte stack buffer plus Serial.write (HWCDC on Waveshare) inside the global esp_log vprintf hook, so it runs in whatever task logs. The default vprintf path already uses a lot of stack for formatting, so 256 extra bytes is a modest increase, and the stack-overflow claim is speculative. The more realistic problem is HWCDC write blocking for its TX timeout in WiFi or lwIP tasks when no USB host is reading. Only Waveshare boards are affected, and only warning/error levels are logged. A smaller buffer or setTxTimeoutMs(0) is low-cost hardening.

### A25. logAndPublish performs blocking MQTT socket I/O on the caller's task, including while sdMutex is held

**Severity:** low · **Verdict:** CONFIRMED · **Area:** concurrency  
**Location:** [`src/logging.cpp:25`](../src/logging.cpp#L25)

**What:** Every logAndPublish/errorPublish call does mqttClient.beginMessage/print/endMessage inline, which is a blocking WiFiClient write that can retry with select timeouts when the TCP send window is full. It is called from every task. Some calls hold other locks: writeDataBlock calls logAndPublish at SDCard.cpp:79-85 while holding sdMutex, and the web /reboot and OTA handlers call it on the loop/LVGL task.

**Failure scenario:** The broker stalls (TCP window full but still connected) and an SD open fails in writeDataBlock. The three logAndPublish calls there block in WiFiClient::write while sdMutex is held, so the SD logger and other saveDataBlock callers hit their 5 s MUTEX_TIMEOUT_SD_MS and silently drop log lines and state saves. On the loop task the same stall freezes the UI.

**Suggested fix:** Treat logging as fire-and-forget. Push to a dedicated MQTT TX queue drained by the MQTT-owning task (see the MQTT ownership finding), just as SD logging already goes through sdLogQueue. Also avoid calling logAndPublish while holding sdMutex: record the error text and log it after xSemaphoreGive.

**Verifier notes:** Confirmed. publishMessageInternal does a blocking mqttClient beginMessage/print/endMessage inline (logging.cpp:25-33). writeDataBlock calls logAndPublish while holding sdMutex (SDCard.cpp:79-85), including around an SD remount. On Arduino v2, WiFiClient::write retries with select timeouts of several seconds when the send window is full. The finding is valid as a design point: a single MQTT owner with a TX queue, and logging after releasing sdMutex. The trigger needs a stalled broker and an SD failure at the same time, so low severity.

### A26. SD saves are dropped silently on mutex timeout, and the throttle timestamp is advanced before the save succeeds

**Severity:** low · **Verdict:** CONFIRMED · **Area:** concurrency  
**Location:** [`src/mqtt.cpp:120`](../src/mqtt.cpp#L120)

**What:** writeDataBlock returns false without logging when sdMutex is not available within 5 s (SDCard.cpp:66-68). addLogToSDCard drops the log line the same way (SDCard.cpp:190-191). saveDataBlock's return value is ignored at every call site, and in the throttled saves lastReadingsSave/lastInsideAQSave are set before the save is attempted (mqtt.cpp:120-121, 357-358).

**Failure scenario:** A log rotation or a slow first-time line count holds sdMutex for more than 5 s. The readings snapshot save fails silently and the next attempt is not made for another READINGS_SAVE_INTERVAL_SEC (300 s). A reboot in that window restores data up to 10 minutes older than intended, and nothing in the logs shows why.

**Suggested fix:** Update lastReadingsSave/lastInsideAQSave only when saveDataBlock returns true. Count or log SD mutex timeouts, for example with a rate-limited errorPublish or a counter exposed on the web status page.

**Verifier notes:** Confirmed. writeDataBlock returns false on mutex timeout without logging (SDCard.cpp:66-68), and addLogToSDCard drops lines the same way (190-191). In mqtt.cpp:120-121 and 357-358, lastReadingsSave/lastInsideAQSave are set before saveDataBlock and its bool result is ignored, so one dropped save postpones the next attempt by a full READINGS_SAVE_INTERVAL_SEC. Fix: advance the timestamp only when the save succeeds.

### A27. Unlocked reads of packed, unaligned shared fields in the API task and status row

**Severity:** low · **Verdict:** CONFIRMED · **Area:** concurrency  
**Location:** [`src/APIs.cpp:634`](../src/APIs.cpp#L634)

**What:** api_manager_t reads weather.updateTime, weather.isDay, uv.updateTime, airQuality.updateTime and the solar.*UpdateTime fields without dataMutex (lines 634, 647, 653, 658, 665-675). The loop task's invalidateStaleApiData writes these under dataMutex (staleness.cpp:110-141). The same holds for updatePeriodicStatus (display.cpp:528-531). The structs are __attribute__((packed)), so updateTime (for example at offset 21 in Weather) is unaligned and GCC emits byte-wise loads and stores, which makes even a 32-bit time_t access non-atomic.

**Failure scenario:** The loop task's invalidateStaleApiData zeroes weather.updateTime (byte by byte) at the moment the API task, at the same priority and time-sliced, reads it for fetchDue(). The API task sees a value with some bytes old and some new, computes a wrong age, and skips or forces a fetch. The impact is a mistimed fetch or a wrong status colour for one second, not corruption.

**Suggested fix:** Take a snapshot under dataMutex at the top of each api_manager_t pass (the updateTime values plus isDay into locals) and use the snapshot. Do the same in updatePeriodicStatus, or keep these scheduling fields in non-packed atomics outside the persisted structs.

**Verifier notes:** The code is as described. All structs are __attribute__((packed)) (types.h:35-92). weather.updateTime and isDay, and uv/airQuality/solar updateTime, are read without dataMutex in api_manager_t (APIs.cpp:634-675) and updatePeriodicStatus (display.cpp:528-535). invalidateStaleApiData (staleness.cpp:110-141) and the fetch functions write them under dataMutex. Unaligned packed fields are accessed byte by byte on Xtensa, so torn reads are possible. Writes from invalidateStaleApiData are rare (only once data passes MAX_API_DATA_AGE_SEC), and the effect is at most one mistimed fetch decision or one wrong status colour, so low severity is right.

### A28. Staleness checks take dataMutex and scan all readings about 50 times a second

**Severity:** low · **Verdict:** CONFIRMED · **Area:** concurrency  
**Location:** [`src/main.cpp:330`](../src/main.cpp#L330)

**What:** loop() calls invalidateOldReadings(), invalidateInsideAirQuality() and invalidateStaleApiData() on every pass (LOOP_DELAY_MS = 20). Each takes dataMutex and calls time(). The thresholds are minutes to hours, so per-pass checking only adds mutex traffic and priority-inheritance boosts against the MQTT and API producers.

**Failure scenario:** There is no functional failure. About 150 mutex take/give pairs per second contend with the receive task's updateReadings and the API task's struct updates, and each contention briefly boosts the loop task. This is wasted CPU on the core that also runs LVGL.

**Suggested fix:** Run the three invalidate* functions from the existing once-per-second updatePeriodicStatus tick (or behind a millis() gate of about 1 s) instead of on every loop pass.

**Verifier notes:** Confirmed. loop() calls all three invalidate* functions on every pass (main.cpp:330-332), and LOOP_DELAY_MS is 20 (constants.h:213). Each call takes dataMutex with portMAX_DELAY and checks against thresholds measured in minutes or hours (staleness.cpp:13-141), and invalidateStaleApiData can call localtime_r on each pass. There is no functional bug. Gating these to once per second (for example from the existing periodic status tick) is a cheap way to cut CPU use and lock contention.

### A29. Board detection is a single I2C probe with no retry; a miss boots the Waveshare with the Matouch pin map

**Severity:** low · **Verdict:** PLAUSIBLE · **Area:** display-board  
**Location:** [`src/board.cpp:40`](../src/board.cpp#L40)

**What:** detectWaveshare() probes 0x24 once, immediately after Serial.begin() with no settle delay (main.cpp:124). The 0x24 'expander' is a CH32V003 microcontroller with its own boot time. If it doesn't ACK that one probe, isWaveshare=false and board=&BOARD_CFG_MATOUCH for the whole run. There is no positive confirmation that the board is a Matouch. The result is never cross-checked or persisted.

**Failure scenario:** On a cold power-on of a Waveshare board, the CH32V003 is still starting when the probe runs. The NACK selects the Matouch config. LEDC PWM is attached to GPIO10 (Waveshare LCD B4), Wire is started on GPIO17/18 (Waveshare LCD B3/B2), and the RGB panel is built with the wrong DE/VSYNC/HSYNC/PCLK pins. The result is a garbled or blank screen and dead touch until someone power-cycles the board again.

**Suggested fix:** Retry the probe several times (for example 5 attempts, 20-50 ms apart) before concluding Matouch. Better still, positively identify both boards: 0x24 on 8/9 means Waveshare, a GT911 at 0x5D/0x14 on 17/18 means Matouch, and if neither responds, retry. Optionally cache the confirmed board type in NVS (Preferences) and use it when the probe is inconclusive.

**Verifier notes:** The code is as described. board.cpp:40-46 makes a single probe with no retry, main.cpp:124 calls it right after Serial.begin, and nothing positively confirms Matouch. A miss is unlikely in practice, though. The ESP32-S3 ROM, the second-stage bootloader and PSRAM init take hundreds of ms before setup() runs, while a CH32V003 boots in a few ms from the same rail. A few retries, or positively identifying the GT911 on 17/18, is cheap hardening, but the failure is speculative.

### A30. LVGL draw buffer is put in PSRAM first, and sized with the 3-byte lv_color_t

**Severity:** low · **Verdict:** PLAUSIBLE · **Area:** display-board  
**Location:** [`src/board.cpp:196`](../src/board.cpp#L196)

**What:** The partial-render draw buffer is allocated with MALLOC_CAP_SPIRAM first and internal RAM only as a fallback. LVGL's software renderer is much slower writing to PSRAM. In this setup PSRAM is also being read continuously by the RGB panel's framebuffer scan-out, and dispFlush copies into the PSRAM framebuffer, so all three compete for the same bus. Separately, in LVGL 9 lv_color_t is always 3 bytes (RGB888), so `sizeof(lv_color_t) * width * DISPLAY_BUFFER_LINES` is 30 KB rather than the 20 KB that RGB565 needs. LVGL quietly renders 15 lines instead of the 10 the constant suggests. The `lv_color_t*` cast in dispFlush is misleading for the same reason.

**Failure scenario:** Every redraw, for example the per-second status updates or a day/night switch that recolours every label, renders slowly through PSRAM and adds PSRAM traffic that competes with panel scan-out. That contention is a plausible contributor to the 'jumping at bottom of screen' seen at 16/30 MHz pclk, which is why pclk is pinned at 12 MHz.

**Suggested fix:** Allocate from MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA first and fall back to SPIRAM. Compute the size as `screenWidth * DISPLAY_BUFFER_LINES * lv_color_format_get_size(LV_COLOR_FORMAT_RGB565)` (or `LV_COLOR_DEPTH/8`). Treat px_map as uint16_t* directly. With the buffer in internal RAM, try raising preferSpeed again.

**Verifier notes:** The facts check out. In LVGL 9.4, lv_color_t is the 3-byte {blue, green, red} struct (lv_color.h:109-113). With LV_COLOR_DEPTH 16, bufferSize = 3*1024*10 = 30720 bytes, so LVGL renders about 15 lines rather than 10. board.cpp:198 tries SPIRAM first. Software rendering into PSRAM is slower than into internal RAM, but the size of the cost here was not measured. The link to the pclk 'jumping' is speculative. Valid as a perf and clarity cleanup (size the buffer with lv_color_format_get_size / LV_COLOR_DEPTH/8, try internal DMA RAM first), but not shown to cause a problem.

### A31. Status row resets identical text and styles every second, forcing relayout and redraw

**Severity:** low · **Verdict:** CONFIRMED · **Area:** display-board  
**Location:** [`src/display.cpp:528`](../src/display.cpp#L528)

**What:** Each second, updatePeriodicStatus() unconditionally calls lv_label_set_text() on ui_WiFiIcon and ui_Version, and lv_obj_set_style_text_color() on 4 status indicators plus ui_WiFiStatus, ui_ServerStatus and ui_WiFiIcon. In LVGL 9.4, set_text_internal() never compares contents: it does lv_free + lv_malloc + copy and marks the label for relayout and invalidation. lv_obj_set_local_style_prop() always calls lv_obj_refresh_style(), which invalidates the object. ui_Version is a long, full-width label. Only ui_Time actually changes each second. You already apply this optimisation to ui_StatusMessage (lines 721-730).

**Failure scenario:** In steady state, about 9 widgets are re-rendered every second through the slow PSRAM draw buffer, with LVGL heap churn from two free/malloc pairs per second. The cost is wasted CPU on core 1, which is shared with the MQTT and API tasks, and extra PSRAM bandwidth contention for no visible change.

**Suggested fix:** Cache the last value per widget: last RSSI icon pointer, last stale/fresh bool per indicator, last wifi/mqtt connected state, last IP string. Only call lv_label_set_text / lv_obj_set_style_text_color when the value changes. A small helper such as `setLabelIfChanged(lv_obj_t*, const char*)` that compares against lv_label_get_text() covers most cases.

**Verifier notes:** display.cpp:528-564 unconditionally sets text on ui_WiFiIcon and ui_Version and the text colour on about 7 widgets every second. In LVGL 9.4, lv_label_set_text → set_text_internal does not compare content, and lv_obj_set_local_style_prop (lv_obj_style.c:353-379) always calls lv_obj_refresh_style, which invalidates the widget. This is real wasted redraw and allocator churn, but it is small (about 9 widgets per second), so low rather than medium. A set-if-changed helper is a sensible fix.

### A32. Touch mapping uses hard-coded raw maxima, an identity X map and no clamping

**Severity:** low · **Verdict:** PLAUSIBLE · **Area:** display-board  
**Location:** [`src/board.cpp:152`](../src/board.cpp#L152)

**What:** `map(x, 0, 1024, 0, LCD_WIDTH)` is an identity mapping (LCD_WIDTH is 1024). Y uses a per-board magic touchRawMaxY (750 or 600). Nothing clamps the result to [0, W-1] / [0, H-1]. LVGL 9.4's indev_pointer_proc only logs a warning for out-of-range points; it does not clamp them. The magic numbers exist because TAMC's setResolution() never actually writes the resolution to the chip: reflashConfig() writes only a checksum, computed with an uninitialised accumulator, plus the config-fresh flag. So the GT911 keeps its factory X/Y output range, which can differ between panel batches.

**Failure scenario:** A replacement panel's GT911 has a factory range of, say, 800x480 or 1024x768. Taps land in the wrong place, and edge touches produce x/y >= resolution, so presses near the right or bottom edge hit nothing. Each begin() call (up to 3 at boot) also writes a random checksum with config_fresh=1 to the GT911.

**Suggested fix:** At touchInit(), read the GT911's real output range from registers 0x8048-0x804B (X max LSB/MSB, Y max LSB/MSB). Map from those values and clamp with constrain(v, 0, LCD_WIDTH-1) and constrain(v, 0, LCD_HEIGHT-1). This removes touchRawMaxY from BoardConfig. If you replace ts->read() as suggested in the first finding, you can drop the TAMC begin()/setResolution() config write entirely.

**Verifier notes:** Confirmed in the code. board.cpp:152 is an identity map (LCD_WIDTH is 1024), Y uses touchRawMaxY, and there is no clamping. LVGL lv_indev.c:746-756 only logs a warning (and LV_USE_LOG is 0) and does not clamp. The TAMC calculateChecksum uses an uninitialised `uint8_t checksum` (TAMC_GT911.cpp:34), and reflashConfig writes only the checksum and the fresh flag on every begin(). The failure needs a different panel or controller range and is hypothetical on current hardware. Clamping and reading 0x8048-0x804B are cheap hardening.

### A33. Clock seconds visibly skip because the 1 s cadence drifts against wall-clock seconds

**Severity:** low · **Verdict:** CONFIRMED · **Area:** display-board  
**Location:** [`src/display.cpp:522`](../src/display.cpp#L522)

**What:** updatePeriodicStatus fires when `currentMillis - lastPeriodicMs >= 1000` and then sets lastPeriodicMs = currentMillis. The loop period is 20 ms plus render and handleClient time, so each update lands 0-30+ ms late and the phase drifts relative to the RTC second boundary. When the phase crosses a boundary, two consecutive updates fall in different seconds two apart.

**Failure scenario:** The "%H:%M:%S" clock jumps, for example 12:00:41 to 12:00:43, every few tens of seconds. It is also up to about 1 s behind real time.

**Suggested fix:** Drive the clock from the time itself. Keep `static time_t lastSec`, and when `time(nullptr) != lastSec`, update the clock label. Keep the 1 s millis gate only for the other status checks, or run the whole status block on the second-change event.

**Verifier notes:** display.cpp:522-524 resets lastPeriodicMs = currentMillis after a check of `>= 1000`. Each period is therefore 1000 ms plus loop jitter (LOOP_DELAY_MS is 20, plus render and handleClient time), so the phase drifts against wall-clock seconds and the HH:MM:SS display periodically skips a second. The fix is cosmetic: update when time(nullptr) changes.

### A34. Grid % shows 100% when today's usage is 0, and truncation biases it upward

**Severity:** low · **Verdict:** CONFIRMED · **Area:** display-board  
**Location:** [`src/display.cpp:109`](../src/display.cpp#L109)

**What:** The early return only triggers when both todayUse and monthUse are <= 0. If todayUse == 0 but monthUse > 0, which is normal just after midnight when the daily counters reset, todaySelfSufficiency is forced to 0, so todayGridPercentage is 100. Separately, `(int)` truncates self-sufficiency (99.6% becomes 99%), so the grid share is overstated by up to 1 point.

**Failure scenario:** Just after midnight, with daily totals reset to 0, the display shows "Today: 0 kWh, R0, 100%": 100% from the grid for zero usage.

**Suggested fix:** When todayUse <= 0, show "--" or "0%" for today's percentage instead of 100%. Compute the grid percentage directly as lroundf(todayBuy / todayUse * 100) and clamp it to 0..100, rather than 100 minus a truncated value.

**Verifier notes:** display.cpp:99 returns early only when both todayUse and monthUse are <= 0. If todayUse == 0 and monthUse > 0, line 109 gives a self-sufficiency of 0, so line 111 sets the grid percentage to 100 and the label shows '100%' for zero usage. Truncating with (int) before the subtraction also biases the figure upward by up to 1 point. Cosmetic.

### A35. Inside-AQ threshold colouring is gone (dead constants), and stale is red where rooms use grey

**Severity:** low · **Verdict:** CONFIRMED · **Area:** display-board  
**Location:** [`src/display.cpp:433`](../src/display.cpp#L433)

**What:** CO2_THRESHOLD_YELLOW/RED and PM25_THRESHOLD_YELLOW/RED (constants.h:145-148) are no longer referenced anywhere. The green/amber/red colouring was removed in commit 95318f7 ('Add runtime board auto-detection'), an unrelated change, so the removal may have been accidental. The project notes still describe threshold colouring with grey for stale. The current code colours stale CO2/PM2.5 COLOR_RED, while stale room readings use COLOR_STALE grey. Red now means 'sensor offline' on this panel but 'bad value' elsewhere.

**Failure scenario:** CO2 rises to 2000 ppm and the label stays black or white, with no warning. A sensor that went offline shows in red, which the user reads as 'air is bad'.

**Suggested fix:** If the removal was unintentional, restore the threshold colouring (amber >= YELLOW, red >= RED) and use COLOR_STALE for stale, matching the room temperatures. If it was intentional, delete the four unused constants.

**Verifier notes:** grep shows CO2_THRESHOLD_* and PM25_THRESHOLD_* (constants.h:145-148) are defined but never used. `git show 95318f7` shows the board auto-detection commit removed the COLOR_STALE/amber/red threshold logic from the inside-AQ display. display.cpp:433 and :442 now use COLOR_RED for stale, while rooms use COLOR_STALE (display.cpp:281). The comment at display.cpp:414 suggests the current behaviour may have been accepted since, so either restore the thresholds or delete the dead constants.

### A36. Staleness checks run every 20 ms loop pass, taking dataMutex and parsing strings each time

**Severity:** low · **Verdict:** CONFIRMED · **Area:** display-board  
**Location:** [`src/main.cpp:329`](../src/main.cpp#L329)

**What:** invalidateOldReadings(), invalidateInsideAirQuality() and invalidateStaleApiData() run on every loop() iteration, about 50 times a second. Each one takes dataMutex. While weather is stale, invalidateStaleApiData also runs two sscanf calls and a localtime_r each pass. Their thresholds are measured in minutes or hours.

**Failure scenario:** There is constant dataMutex contention with the MQTT receive task (priority 2) and the API task. In the worst case MQTT parsing blocks behind the loop's lock, or the loop blocks behind MQTT, adding render jitter. It is also wasted CPU on the core that renders the UI.

**Suggested fix:** Gate all three behind a once-per-second (or once-per-10 s) check, for example inside the same 1 s tick as updatePeriodicStatus, or on time(nullptr) changing.

**Verifier notes:** main.cpp:329-331 calls all three staleness functions on every loop pass (about every 20 ms or more). Each one takes dataMutex (staleness.cpp:16, 37, 109), and invalidateStaleApiData runs two sscanf calls plus localtime_r while weather.updateTime == 0. The thresholds are 1800 s or more. The cost per call is small, so this is minor wasted work and lock traffic. Gating them on a 1 s tick is a trivial improvement.

### A37. Clock-derived day/night flip doesn't refresh the UV panel

**Severity:** low · **Verdict:** CONFIRMED · **Area:** display-board  
**Location:** [`src/staleness.cpp:125`](../src/staleness.cpp#L125)

**What:** When weather is stale, invalidateStaleApiData recomputes weather.isDay from sunrise/sunset but doesn't set dirtyUv. updateUVDisplay only shows ui_UVUpdateTime when isDay is true, and only re-runs when dirtyUv is set. fetchWeather sets dirtyUv because it knows the UV panel depends on isDay; this path doesn't.

**Failure scenario:** With the weather API down, sunset passes and the screen switches to night mode, but the UV update-time label keeps showing the daytime timestamp all night. In the morning it stays blank until the next UV fetch.

**Suggested fix:** Track the previous isDay. If the recomputed value differs, set dirtyUv = true (and dirtyWeather if needed). Or have adjustDayNightMode() set dirtyUv on any day/night transition, alongside dirtyRooms/dirtyInsideAQ.

**Verifier notes:** staleness.cpp:118-126 recomputes weather.isDay without setting dirtyUv, and updateUVDisplay (display.cpp:312-317) shows the timestamp only when isDay is true and only re-runs on dirtyUv. adjustDayNightMode (display.cpp:573-575) also does not set dirtyUv. The scenario is overstated, though. While weather is stale, the API manager's night handling (APIs.cpp:634) is skipped and UV fetching stops (isDay is false), so uv.updateTime ages out within MAX_API_DATA_AGE_SEC (7200 s), which sets dirtyUv and blanks the panel. The stale daytime timestamp therefore lasts at most about 1-2 hours, not all night. In the morning the zeroed updateTime makes a fetch due straight away, so the blank gap is short. Setting dirtyUv on any isDay transition is still a correct small fix.

### A38. Solarman token silently truncated by snprintf, causing an endless token-refresh loop

**Severity:** low · **Verdict:** CONFIRMED · **Area:** memory  
**Location:** [`src/APIs.cpp:331`](../src/APIs.cpp#L331)

**What:** snprintf(solarToken.token, 2048, "bearer %s", recToken) ignores the return value. The project has already been hit by silent truncation at 512 and 1024. If Solarman's JWT grows past 2040 characters, the truncated token is stored, saved to SD and reported as obtained.

**Failure scenario:** The token response gets longer (more claims). Each pass, fetchSolarToken stores a truncated token and returns true, which resets the backoff. fetchCurrentSolar gets 'auth invalid token', clears the token and returns true, so no backoff there either. The token endpoint is then hit every API_FAIL_DELAY_SEC (30 s) indefinitely, there's no solar data, and the only symptom is repeated 'Solar token rejected' log lines.

**Suggested fix:** Check the result: int n = snprintf(...); if (n < 0 || n >= (int)sizeof(solarToken.token)) { errorPublish("Solar token too long (n bytes)"); solarToken.token[0] = 0; return false; }, so that backoff applies and the cause is logged.

**Verifier notes:** APIs.cpp:331 ignores snprintf's return value and sets obtained=true, so a truncated token is stored and saved. fetchCurrentSolar on 'auth invalid token' clears the token and returns true (APIs.cpp:392-402), so neither the token nor the solar backoff grows, and the token is refetched roughly every API_FAIL_DELAY_SEC. This only happens if the JWT grows past 2040 characters, so low is right. Checking the return value is cheap protection.

### A39. Address of a packed struct member passed to localtime_r

**Severity:** low · **Verdict:** PLAUSIBLE · **Area:** memory  
**Location:** [`src/display.cpp:485`](../src/display.cpp#L485)

**What:** SolarForecast is __attribute__((packed)), so it has alignment 1. `SolarForecast fc` is a stack local, and &fc.ts is passed as a const time_t* to localtime_r, which does a normal aligned 32-bit load. GCC gives no guarantee that a packed local lands 4-byte aligned. Its neighbours here are char line1[64], line2[64] and text[255]. This is the only place in user code that takes the address of a packed member (confirmed by grep).

**Failure scenario:** A compiler or optimisation change places fc at an unaligned stack offset. localtime_r's load then raises a LoadStoreAlignment exception on Xtensa, and the loop task panics and reboots each time the forecast panel refreshes (once a minute).

**Suggested fix:** Copy the value first: `time_t ts = fc.ts; localtime_r(&ts, &tsTm);`. Consider adding -Waddress-of-packed-member to build_flags so any future cases are caught.

**Verifier notes:** types.h:81 declares SolarForecast packed, and display.cpp:485 passes &fc.ts, from a stack-local copy, to localtime_r. ts is at offset 0, and in practice GCC aligns the stack slot, so this is unlikely to fault today. Alignment is not guaranteed, though, and the v2.x toolchain (GCC 8.4) does not warn by default. Copying into a local time_t is trivial and correct.

### A40. UV index truncated instead of rounded when stored as int

**Severity:** low · **Verdict:** CONFIRMED · **Area:** memory  
**Location:** [`src/APIs.cpp:143`](../src/APIs.cpp#L143)

**What:** `uv.index = UV;` converts the float from weatherbit to int by truncating. UV index is conventionally rounded to the nearest integer, and the colour scale (uvColor) and the arc both use the stored int.

**Failure scenario:** Weatherbit returns uv = 5.9. The display shows 5 with the yellow 'moderate' colour instead of 6 'high' orange. 0.8 shows as 0 with the dark-green 'low' band. The UV arc max also uses std::max(lroundf(uvMax), uv.index), so the arc and label disagree with the forecast's rounding.

**Suggested fix:** Use `uv.index = (int)lroundf(UV);`. Or store a float in UV and round only for display, noting that changing the struct size invalidates the saved uv_data.bin once, which loadDataBlock handles safely.

**Verifier notes:** types.h:52 declares UV::index as int, and APIs.cpp:143 assigns the float with truncation. display.cpp:318/325 show uv.index and compare it with lroundf(weather.uvMax), so the rounding is inconsistent. 5.9 shows as 5. Minor display correctness, so low.

### A41. XOR checksum misses common corruption patterns

**Severity:** low · **Verdict:** CONFIRMED · **Area:** memory  
**Location:** [`src/utils.cpp:201`](../src/utils.cpp#L201)

**What:** calculateChecksum is an 8-bit XOR. It ignores byte order, so swapped or misplaced sectors aren't detected. Any even number of identical bit flips in the same bit position cancel out, and random corruption has a 1/256 chance of passing. DataHeader has no magic or version field either, so a different struct of the same size would load without complaint.

**Failure scenario:** A FAT cluster from an older save, or from a different .bin of the same length, is cross-linked into the file. The XOR happens to match (about 0.4% chance, or certain if bytes are only reordered), so stale or foreign data is loaded as valid state, including string fields with no terminator.

**Suggested fix:** Use CRC32: esp_rom_crc32_le(0, data, size) is in ROM and costs nothing. Add a uint32_t magic and a per-struct version to DataHeader. Changing the header invalidates existing files once, which the size check already handles.

**Verifier notes:** utils.cpp:201-208 is an 8-bit XOR checksum, and DataHeader (types.h:119-122) has only size and checksum, with no magic or version. XOR does not detect reordered bytes and lets random corruption through 1 time in 256. CRC32 from ROM is a cheap upgrade, and the size check already rejects old-format files after a header change.

### A42. LVGL draw buffer allocated in PSRAM first, slowing every render

**Severity:** low · **Verdict:** CONFIRMED · **Area:** memory  
**Location:** [`src/board.cpp:196`](../src/board.cpp#L196)

**What:** The partial-render buffer goes to PSRAM first and only falls back to internal RAM. LVGL draws into this buffer pixel by pixel (fills, anti-aliased arcs, text blending), and on the ESP32-S3 random access to PSRAM through the cache is several times slower than SRAM. Also, in LVGL 9 lv_color_t is 3 bytes, so sizeof(lv_color_t)*1024*10 gives 30 KB for an RGB565 display. The buffer is 50% larger than the 10 lines intended (LVGL renders 15).

**Failure scenario:** Full-screen redraws, such as the day/night switch or the panel updates that the dirty flags trigger, take noticeably longer while loop() is busy. That delays webServer.handleClient() and touch polling, and stretches the time dataMutex is held in set_solar_values().

**Suggested fix:** Size the buffer as LCD_WIDTH * DISPLAY_BUFFER_LINES * lv_color_format_get_size(LV_COLOR_FORMAT_RGB565), which is 2 bytes per pixel. Allocate it from MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA first (20 KB, affordable at about 36% RAM used) and fall back to PSRAM.

**Verifier notes:** board.cpp:196 sizes the buffer as sizeof(lv_color_t)*width*DISPLAY_BUFFER_LINES. In LVGL 9, lv_color_t is the 3-byte RGB888 struct, while LV_COLOR_DEPTH is 16 (lv_conf.h:30). The buffer is therefore 30 KB and LVGL renders 15 lines, not the 10 intended. Lines 198-200 allocate from PSRAM first. Rendering into internal SRAM is generally faster on the S3. The speed gain is plausible but unmeasured; the sizing error is certain.

### A43. Log JSON is copied into a second ~200 KB String before sending

**Severity:** low · **Verdict:** CONFIRMED · **Area:** memory  
**Location:** [`src/SDCard.cpp:472`](../src/SDCard.cpp#L472)

**What:** getLogsFromSDCard builds the JSON in a 200 KB malloc'd buffer (JSON_BUFFER_SIZE = 500*400+32), plus a 130 KB LogLine array. It then does `jsonOutput = jsonBuffer`, which allocates and copies a second String of the same size before freeing the original. Peak usage is roughly 2x the response plus the line array, about 530 KB, and it relies on CONFIG_SPIRAM_USE_MALLOC to keep the String out of internal RAM.

**Failure scenario:** /api/logs/normal is requested while PSRAM is fragmented, for example during an OTA download or with several browser tabs open. The String allocation fails silently (Arduino String leaves itself invalid/empty), so the viewer gets an empty 200 response. If the fallback malloc path ever lands in internal RAM, other tasks run out of heap.

**Suggested fix:** Send straight from the buffer: change the API to fill a caller-provided buffer, then call webServer.setContentLength(len); webServer.send(200, "application/json", ""); webServer.sendContent(jsonBuffer, len); free(jsonBuffer). That removes the String copy entirely. Optionally stream entries with sendContent per chunk and drop the 200 KB buffer.

**Verifier notes:** SDCard.cpp:472 `jsonOutput = jsonBuffer` copies into a String, then frees the buffer, and web_server.cpp:15-17 sends the String. The extra copy is real, but the finding overstates it. The String is sized to the actual JSON length, not the full 200 KB buffer. The 130 KB lines array has already been freed (line 469) before the copy, so peak memory is roughly 200 KB plus the actual JSON length, not ~530 KB. Sending straight from the buffer with sendContent is still a valid cleanup. Low is right.

### A44. Custom ESP_LOG vprintf hook puts a 256-byte buffer and a blocking USB-CDC write on system task stacks

**Severity:** low · **Verdict:** PLAUSIBLE · **Area:** memory  
**Location:** [`src/main.cpp:133`](../src/main.cpp#L133)

**What:** On Waveshare, esp_log_set_vprintf installs a lambda that puts char buf[256] on the stack and calls Serial.write (HWCDC). It runs in whichever task emits an ESP_LOG: the WiFi task, the lwIP tcpip task, esp_timer, the SSL client and so on. Those tasks have small fixed stacks, and HWCDC::write can block for its TX timeout when the host isn't draining the port.

**Failure scenario:** During a WiFi reconnect storm the WiFi or lwIP task logs warnings. The extra 256 B frame plus vsnprintf's own usage pushes a tight system task stack over its limit, causing a stack-overflow panic. Or, with the USB cable attached but no terminal open, each log line stalls the WiFi task for the CDC timeout.

**Suggested fix:** Format into a smaller static buffer protected by a spinlock or mutex, or write with a zero timeout (Serial.setTxTimeoutMs(0) on HWCDC). Alternatively, drop the hook and send ESP-IDF logs to USB-Serial-JTAG with CONFIG_ESP_CONSOLE_SECONDARY instead.

**Verifier notes:** main.cpp:133-140 installs a vprintf hook that puts char buf[256] on the stack and calls Serial.write in whichever task emits an ESP_LOG, including system tasks such as WiFi and lwIP, which have small fixed stacks. Two things lower the real risk. The precompiled v2.x IDF log level is low, so system tasks emit few ESP_LOG lines. Arduino's log_e goes through ets_printf, not this hook. A stack overflow or USB-CDC blocking is still possible, so the suggestions (a smaller static buffer, or a zero TX timeout) are reasonable hardening.

### A45. Zero-length MQTT payload desyncs the library parser and forces a disconnect; the 'Empty MQTT message' branch can never run

**Severity:** low · **Verdict:** CONFIRMED · **Area:** networking  
**Location:** [`src/mqtt.cpp:62`](../src/mqtt.cpp#L62)

**What:** For a QoS0 PUBLISH with an empty payload, parseMessage() returns 0 (_rxLength==0) but leaves _rxState at READ_PUBLISH_PAYLOAD. The task treats 0 as 'no message' (else branch, line 131). On the next parseMessage(), the library moves to DISCARD_PUBLISH_PAYLOAD, and poll() then consumes one byte as payload even though _rxLength is already 0 (MqttClient.cpp:615-624). That byte is the fixed-header byte of the next packet, so the stream is misaligned. The following byte is read as a packet type, judged malformed, and stop() is called. The empty-message check at line 94 is unreachable because messageSize==0 never enters the if at line 62.

**Failure scenario:** Someone clears a retained topic (mosquitto_pub -r -n -t kitchen/co2/set), or a sensor publishes an empty string after a failed read. The broker forwards the zero-length PUBLISH, the next packet (e.g. a PINGRESP or another reading) is misparsed, and the client calls stop(). MQTT drops and reconnects, and the readings in flight are lost. A sensor that keeps publishing empty strings causes repeated disconnects.

**Suggested fix:** Use the onMessage callback (see the subscribe finding). With a callback set, the library resets _rxState to READ_TYPE for zero-length messages (MqttClient.cpp:580-583), and the callback can log 'Empty MQTT message on topic'. Then remove the dead check at line 94.

**Verifier notes:** Verified against the library. With a zero-length QoS0 payload, poll() leaves _rxState=READ_PUBLISH_PAYLOAD and _rxLength=0, and parseMessage() returns 0 (MqttClient.cpp:126-131). mqtt.cpp:62 treats that as no message. The next parseMessage() switches to DISCARD, and poll() then consumes one byte in the DISCARD case without decrementing (since _rxLength is 0) before returning to READ_TYPE (lines 616-627). That byte is the next packet's fixed header. The byte after it (e.g. 0x00 from a PINGRESP) is read as type 0, which is an unexpected type, so the parser treats it as malformed and calls stop(). The messageSize==0 half of the check at line 94 is dead. The recMessage[0]=='\0' half can still fire for a payload that starts with NUL.

### A46. No watchdog feed between the version check and the firmware GET; worst-case blocking exceeds 60 s

**Severity:** low · **Verdict:** PLAUSIBLE · **Area:** networking  
**Location:** [`src/OTA.cpp:48`](../src/OTA.cpp#L48)

**What:** api_manager_t feeds the watchdog once (APIs.cpp:682) and then calls checkForUpdates(). That function does an HTTPS GET (handshake up to HTTPS_HANDSHAKE_TIMEOUT_SEC=20 s, plus a response timeout of 10 s) and getString() (up to 10 s). It then calls updateFirmware(), which does a second begin()/GET (another up to 20 s handshake + 10 s) before the download loop's first esp_task_wdt_reset(). Neither function feeds the watchdog in between.

**Failure scenario:** The OTA server is slow or congested: version fetch about 35 s plus binary GET headers about 30 s is over the 60 s task watchdog. The device panics and reboots in the middle of an update check, and then repeats this every 5 minutes while the server stays slow.

**Suggested fix:** Call esp_task_wdt_reset() at the start of updateFirmware() and right after http.GET() returns in both functions. Optionally bound getString() by reading at most ~32 bytes for the version file.

**Verifier notes:** The code is as described. APIs.cpp:682 feeds the watchdog once, then checkForUpdates() does a TLS handshake (up to 20 s), header wait (10 s) and getString(). It then calls http.end() and updateFirmware() opens a new TLS connection (another handshake plus header wait) before the first esp_task_wdt_reset() in the download loop (OTA.cpp:74). logAndPublish (logging.cpp:26) feeds the watchdog when it gets the mutex, and one such call ('New firmware version available') sits between the two requests. That partly mitigates the problem but is not guaranteed, because it uses a non-blocking take. Exceeding 60 s also requires a very slow server on both requests, so this is reachable only in edge conditions.

### A47. MQTT connect can block for >60 s with a single watchdog feed and busy-spins the connectivity task

**Severity:** low · **Verdict:** PLAUSIBLE · **Area:** networking  
**Location:** [`src/connections.cpp:48`](../src/connections.cpp#L48)

**What:** mqtt_connect() feeds the watchdog once, then calls mqttClient.connect(), which waits up to _connectionTimeout (library default 30 s) for CONNACK. The first subscribe() can then wait up to another 30 s for SUBACK. setConnectionTimeout() is never called. Both waits spin in a poll()/yield() loop at priority 1, and on ESP32 yield() does not let lower-priority tasks (the SD logger) run.

**Failure scenario:** Broker overload: CONNACK arrives after ~29 s, then the broker stalls before SUBACK. The total is more than 60 s without a watchdog feed, so a task-WDT panic reboots the device. Even short of a reboot, the SD logger is starved for the duration.

**Suggested fix:** Call mqttClient.setConnectionTimeout(10000) once in setup and esp_task_wdt_reset() before each subscribe() call.

**Verifier notes:** The watchdog-panic scenario is mostly refuted. logAndPublish('Connected to the MQTT broker') at connections.cpp:54 runs between connect() and the subscribes, and publishMessageInternal feeds the watchdog (logging.cpp:26) whenever it gets mqttMutex. After the first subscribe times out, the library calls stop(), and later subscribes fail immediately at endPacket(); each failure goes through errorPublish, which also feeds. Each blocking segment is therefore about 30 s or less, not more than 60 s. The busy-spin is real: the connect and subscribe loops call poll()+yield() for up to 30 s at priority 1, which starves the priority-0 SD logger so its queue fills and log lines are dropped. Calling setConnectionTimeout(~10000) is still a sensible hardening.

### A48. Daily battery min/max reset only happens if a solar fetch succeeds during hour 0

**Severity:** low · **Verdict:** CONFIRMED · **Area:** networking  
**Location:** [`src/APIs.cpp:436`](../src/APIs.cpp#L436)

**What:** The reset happens only inside fetchCurrentSolar() when nowTm.tm_hour == 0. If no successful fetch happens between 00:00 and 00:59 (WiFi outage, Solarman outage, token refresh failing, 'success:false' responses), the reset is skipped. At 01:00, isMinMaxReset goes back to false, but tm_hour is no longer 0, so the reset never happens that day.

**Failure scenario:** Solarman is down from 23:30 to 01:15. All of the next day's display shows yesterday's battery min/max (e.g. a min of 12% from the previous night) because the midnight window was missed.

**Suggested fix:** Store the day the values belong to (tm_yday or YYYYMMDD in NVS next to solarmin/solarmax) and reset whenever the current local date differs, instead of keying on hour == 0.

**Verifier notes:** APIs.cpp:436-444 resets min/max only when a successful fetchCurrentSolar() runs while tm_hour==0. If no fetch succeeds during 00:00-00:59, isMinMaxReset is set back to false at 01:00 without any reset, so yesterday's min and max carry over for the whole day. This needs an outage of about an hour around midnight, hence low severity. Keying the reset on a stored date (tm_yday) is the correct fix.

### A49. success:true responses with missing fields write 0 into displayed values

**Severity:** low · **Verdict:** PLAUSIBLE · **Area:** networking  
**Location:** [`src/APIs.cpp:405`](../src/APIs.cpp#L405)

**What:** fetchCurrentSolar() only checks root["success"]. batterySoc, usePower, wirePower, batteryPower, generationPower and lastUpdateTime are read with implicit conversion, which gives 0 for null or missing values. fetchWeather() validates only current.temperature_2m, so daily max/min/uv_index_max (lines 219-221) become 0 when the daily block is missing or null. Daily and monthly solar have the same problem with buyValue, useValue and generationValue.

**Failure scenario:** Solarman returns {"success":true,"batterySoc":null,...} while the inverter is offline. The display shows 0% battery and 0 kW, and fetch time 00:00:00 (from lastUpdateTime=0), with fresh-data colouring. The result is saved to SD and treated as fresh for the full interval.

**Suggested fix:** Validate the key fields with is<float>() (e.g. batterySoc, generationPower, and daily temperature_2m_max[0]) before taking dataMutex. Or use `root["x"] | previousValue` so a missing field keeps the last good value instead of 0.

**Verifier notes:** The code matches the description. Lines 405-410 read fields with implicit conversion after checking only 'success', and lines 219-221, 526-528 and 588-590 do the same. The existing guard 'solar.batteryCharge != 0' at line 445 suggests zero values have been seen before. lastUpdateTime=0 would show the time as 00:00:00 or 02:00:00 in local time. I could not confirm that Solarman actually returns success:true with null fields, and open-meteo always includes the daily block when it is requested, so this stays PLAUSIBLE.

### A50. readChunkedPayload continues after a short chunk read, misaligning the chunk framing

**Severity:** low · **Verdict:** CONFIRMED · **Area:** networking  
**Location:** [`src/APIs.cpp:726`](../src/APIs.cpp#L726)

**What:** If readBytes() times out and returns fewer than chunkSize bytes, the code ignores this (empty if-block), discards two bytes it assumes are CRLF, and parses the next 'size line' from the middle of the body data. That can pull more body bytes in as a bogus chunk size. Also, hitting the outer loop limit (totalRead == bufferSize-1) or losing the connection returns a positive length without the terminating 0-chunk having been seen.

**Failure scenario:** A slow chunked response from open-meteo stalls mid-chunk for longer than the 10 s timeout. The function returns a spliced, truncated body as success (>0). In practice deserializeJson usually rejects it, but the error is then logged as a 'JSON error' instead of a payload read error, which hides the real cause.

**Suggested fix:** Return -1 when dataRead != (size_t)chunkSize, when the CRLF after a chunk is not "\r\n", or when the loop exits without reading the terminating zero-size chunk.

**Verifier notes:** APIs.cpp:726-728 has an empty if-block on a short read. It then blindly discards two bytes and parses the next size line from inside the body. If the loop exits without seeing a zero-size chunk (connection drop, or strtol of garbage returning 0), the partial body is returned as success. The practical effect is mainly a misleading 'JSON error' log, so low severity is right.

### A51. After a failed setup_wifi() the radio is switched off, so the in-between 'autoReconnect' iterations do nothing

**Severity:** low · **Verdict:** CONFIRMED · **Area:** networking  
**Location:** [`src/connections.cpp:26`](../src/connections.cpp#L26)

**What:** When setup_wifi() runs out of retries it calls WiFi.disconnect(true), which switches STA mode off. connectivity_manager_t then waits WIFI_RECONNECT_GROUP-1 iterations (4 x 5 s) 'letting autoReconnect work in the background' (line 114), but with the radio off nothing can reconnect. Each recovery cycle is therefore about 55 s of attempts followed by 20 s of dead radio.

**Failure scenario:** The router reboots. The first setup_wifi() runs while the AP is still down and turns the radio off. The AP comes back 10 s later, but the device stays offline for up to 20 s more until wifiFailCount reaches 5. With repeated short AP blips, recovery is consistently delayed.

**Suggested fix:** On failure, call WiFi.disconnect(false) (keep STA mode and autoReconnect enabled), or simply call setup_wifi() on every iteration while disconnected and drop the WIFI_RECONNECT_GROUP logic. Either way, update the comment at line 114 to match the behaviour.

**Verifier notes:** connections.cpp:26 calls WiFi.disconnect(true), which in Arduino-ESP32 v2.x disables STA mode (enableSTA(false)). connectivity_manager_t then sleeps through wifiFailCount 2-4 and 6-9 (lines 107-115) with the radio off, so nothing can reconnect in the background as the comment at line 114 claims. Every setup_wifi() call also starts with disconnect(true) (line 11), so autoReconnect never gets a chance to help. The result is about 15-20 s of dead time per cycle.

### A52. Solar token HTTP error log drops the status code for HTTP errors

**Severity:** low · **Verdict:** CONFIRMED · **Area:** networking  
**Location:** [`src/APIs.cpp:346`](../src/APIs.cpp#L346)

**What:** http.errorToString() only maps negative transport errors and returns an empty String for positive HTTP status codes. When the token POST gets a 401, 403 or 500, the log reads '[HTTP] GET solar token failed, error: ' with no code. Every other fetch uses reportHttpError(), which prints the number. The label also says GET for what is a POST.

**Failure scenario:** Solarman rejects the credentials with 401. The error log gives no clue whether the problem is auth, rate limiting or a server outage.

**Suggested fix:** Use reportHttpError("POST solar token", httpCodeToken) for consistency. Also fix the 'GET' labels on the other POST requests (solar status, today, month).

**Verifier notes:** APIs.cpp:346 uses http.errorToString(httpCodeToken). In HTTPClient v2.x this only maps negative codes and returns an empty String for HTTP status codes such as 401 or 500, so the log loses the code. Every other fetch uses reportHttpError(), which prints the number. The 'GET' labels on POST calls (lines 346, 369, 498, 562) are also wrong.

### A53. OTA download uses a 128-byte buffer with a 1 ms delay per read

**Severity:** low · **Verdict:** CONFIRMED · **Area:** networking  
**Location:** [`src/OTA.cpp:66`](../src/OTA.cpp#L66)

**What:** OTA_BUFFER_SIZE is 128 and every loop iteration calls vTaskDelay(1 tick), even when more data is already available. A ~2-3 MB image takes about 20k iterations, and therefore at least 20k ticks plus TLS record overhead, while api_manager_t (and all API refreshes) are blocked.

**Failure scenario:** A 2.5 MB image takes at least ~20 s longer than necessary to download, and all API data stalls for that time.

**Suggested fix:** Use a 1-4 KB buffer (static or heap, so it doesn't sit on the 8 KB task stack) and only vTaskDelay() when client.available() == 0.

**Verifier notes:** OTA_BUFFER_SIZE=128 (constants.h:295) and OTA_YIELD_DELAY_MS=1 (constants.h:297). vTaskDelay(1 tick at 1 kHz) runs on every iteration, even after a successful read (OTA.cpp:97). A 2.5 MB image takes about 20k iterations, which adds at least about 20 s while api_manager_t is blocked. The fix needs a static or heap buffer, because a 4 KB buffer on the 8 KB task stack is risky, and the delay should run only when available()==0.

### A54. setReuse(true)/setReuse(false) pairs around Solarman calls keep the TLS session open and contradict their comments

**Severity:** low · **Verdict:** CONFIRMED · **Area:** structure  
**Location:** [`src/APIs.cpp:361`](../src/APIs.cpp#L361)

**What:** fetchCurrentSolar, fetchDailySolar and fetchMonthlySolar call http.setReuse(true) before begin(), call http.end(), and only then call setReuse(false) (lines 361/376, 489/506, 553/570). In the Arduino-ESP32 2.0.17 HTTPClient, end() → disconnect() keeps the socket open when `_reuse && _canReuse` (HTTPClient.cpp:392). Because reuse is still true when end() runs, the WiFiClientSecure session to Solarman (an mbedTLS context of about 40 KB) stays open after each solar call, and the later setReuse(false) does nothing to it. The connection is only dropped when the next begin() goes to a different host. The comment 'Keep the connection open for potential token refresh' is inaccurate: token refresh happens in a later pass. MEMORY.md also says setReuse(false) is required for this single-client design.

**Failure scenario:** Every 60 s fetchCurrentSolar leaves a TLS session to Solarman allocated in internal heap until the next weather, air-quality or OTA call, or until the server drops the keep-alive. That reduces heap headroom during the window in which the next handshake (OTA or weather) has to allocate a second context. Future readers are also misled about the reuse policy.

**Suggested fix:** Either drop the setReuse toggles and leave the global setReuse(false) from setup(), or make reuse deliberate: set reuse true once before the current, daily and monthly batch, then call `http.setReuse(false); http.end(); httpsClient.stop();` after the batch. Put this inside the solarmanPost helper suggested above so the policy lives in one place.

**Verifier notes:** In the 2.x framework HTTPClient.cpp:367-380, disconnect() keeps the socket open when _reuse && _canReuse. Because setReuse(false) is called only after http.end() (APIs.cpp:370-376, 500-506, 564-570), the TLS session stays open after each Solarman call. The later setReuse(false) does not close it. One nuance: the reuse does help the current, daily and monthly sequence to the same host within one pass, so 'for daily data fetches' in the comment is partly accurate. 'Token refresh' is not. The session left open after the last solar call is real but its cost is modest, so low is right.

### A55. Staleness checks run every 20 ms loop pass, taking dataMutex 3× per frame for 30-minute-granularity state

**Severity:** low · **Verdict:** CONFIRMED · **Area:** structure  
**Location:** [`src/main.cpp:330`](../src/main.cpp#L330)

**What:** loop() calls invalidateOldReadings(), invalidateInsideAirQuality() and invalidateStaleApiData() on every pass (about 50 Hz). Each one calls time() twice and takes dataMutex with portMAX_DELAY, and invalidateOldReadings walks all 15 readings. Once weather has gone stale, invalidateStaleApiData also runs localtime_r() and two sscanf() calls (hhmmToMinutes) on every pass. The thresholds involved are 30/60/120 minutes, so checking 50 times a second is about 3,000× more often than needed. The work also competes for dataMutex with the MQTT task (priority 2) and the API task.

**Failure scenario:** In steady state that is about 150 mutex take/give pairs and 300 time() calls per second on Core 1, plus sscanf/localtime on every frame during an API outage. This takes CPU from LVGL rendering in the same loop task, and every MQTT or API write has to wait for whichever staleness pass holds the mutex.

**Suggested fix:** Rate-limit the checks to once per second. Move the three calls inside the existing 1 s gate (call them from updatePeriodicStatus, or add a `static unsigned long lastStaleCheck` in loop()). Better still, put them in a staleness_tick(nowMillis) in staleness.cpp that self-throttles. Display latency is unaffected because the dirty flags still drive redraws.

**Verifier notes:** main.cpp:330-332 calls all three staleness functions on every loop pass (LOOP_DELAY_MS=20 plus render time). Each one takes dataMutex with portMAX_DELAY (staleness.cpp:16, 37, 109). invalidateStaleApiData runs sscanf and localtime_r on every pass whenever weather.updateTime == 0. The code is as described, but uncontended FreeRTOS mutex take/give and time() cost microseconds on a 240 MHz core, so the real CPU and latency impact is small. Moving the checks behind a 1 s gate is still a sensible cleanup. Downgraded to low.

### A56. updatePeriodicStatus() re-sets unchanged label text and styles every second, forcing redraws

**Severity:** low · **Verdict:** CONFIRMED · **Area:** structure  
**Location:** [`src/display.cpp:528`](../src/display.cpp#L528)

**What:** Once a second, updatePeriodicStatus sets 4 status colours (528-531), the WiFi icon text and colour (536-538), the server status colour (545-549) and ui_Version text (559-564). The ui_Version update also builds a heap String through WiFi.localIP().toString(). In LVGL 9, lv_label_set_text() always re-lays out and invalidates even when the text is identical, and lv_obj_set_style_text_color() always refreshes style and invalidates. Only the clock actually changes each second. display_update() already avoids this for the status message (lines 721-730), but the pattern isn't applied here.

**Failure scenario:** Every second, about 9 widgets are invalidated and re-rendered through the 10-line PSRAM draw buffer to the 12 MHz RGB panel, even when nothing has changed. That costs CPU in the loop task and allocates and frees a String each time.

**Suggested fix:** Cache the last applied state in function-scope statics: last WiFi connected flag, last icon pointer, last stale state per status label, last mqtt connected flag and last IP. Call LVGL setters only when a value changes. Make setStatusColor take a `bool* lastStale` (or return early if unchanged). Rebuild ui_Version only when the WiFi state or IP changes.

**Verifier notes:** Checked the LVGL 9.4 source. lv_label_set_text -> set_text_internal does not compare against the old text, and lv_obj_set_local_style_prop always calls lv_obj_refresh_style. updatePeriodicStatus (display.cpp:528-564) therefore invalidates about 9 widgets a second, and builds a String via WiFi.localIP().toString(), even when nothing has changed. display_update already uses the compare-first pattern for the status label (721-730). Minor cost, so low.

### A57. Inside-AQ threshold constants are dead; stale colour is red here but grey everywhere else

**Severity:** low · **Verdict:** CONFIRMED · **Area:** structure  
**Location:** [`src/constants.h:145`](../src/constants.h#L145)

**What:** CO2_THRESHOLD_YELLOW/RED and PM25_THRESHOLD_YELLOW/RED are defined with the comment 'colour coding on labels' but nothing uses them. The colouring was removed in commit 95318f7 (runtime board detection), probably unintentionally since that commit was about boards. updateInsideAQDisplay (display.cpp:433, 442) now colours only stale readings, and uses COLOR_RED, while rooms use COLOR_STALE grey (display.cpp:281) and ReadingState::STALE is documented as 'coloured grey' (constants.h:113). MEMORY.md still describes the inside AQ display as green/amber/red by threshold with grey for stale.

**Failure scenario:** CO2 rises to 2000 ppm in the kitchen. The label shows 'CO2: 2,000' in plain black or white with no warning, although the constants and project notes say it should be red. A stale CO2 sensor shows red, which reads as 'dangerous air' rather than 'sensor offline'.

**Suggested fix:** Decide which behaviour is intended. Either restore the colouring with a shared helper, e.g. `static lv_color_t aqColor(float v, float yellow, float red, ReadingState s)` that returns COLOR_STALE for STALE, COLOR_RED at or above red, COLOR_AMBER at or above yellow, and otherwise the theme default. Or delete the four constants and switch stale to COLOR_STALE for consistency. Update the updateInsideAQDisplay comment either way.

**Verifier notes:** CO2_THRESHOLD_* and PM25_THRESHOLD_* (constants.h:145-148) have no references. git shows commit 95318f7 removed the threshold-based amber/red colouring from display code. updateInsideAQDisplay now colours only stale readings, and uses COLOR_RED (433, 442), while rooms use COLOR_STALE (281). The ReadingState::STALE comment says grey. The project memory still describes green/amber/red colouring. This is a real behaviour regression or inconsistency, worth a decision either way.

### A58. Magic numbers duplicated instead of using existing constants (I2C pins/address, PWM cap, timeouts, delays)

**Severity:** low · **Verdict:** CONFIRMED · **Area:** structure  
**Location:** [`src/board.cpp:41`](../src/board.cpp#L41)

**What:** detectWaveshare() hard-codes `Wire.begin(8, 9, 400000)` and `0x24` (lines 41-42), although board.cpp already includes board_waveshare.h, which defines WS_I2C_SDA, WS_I2C_SCL and WS_EXPANDER_I2C_ADDR. The 97 PWM cap is literal three times in waveshare_backlight_set (board_waveshare.cpp:65,74), also as WS_BACKLIGHT_DAY_PERCENT. touchRead maps raw X from a literal 1024 (board.cpp:152) while Y uses board->touchRawMaxY. main.cpp:31-32 defines the HTTP timeouts locally and main.cpp:299 hard-codes the 60 s watchdog. mqtt.cpp:136 uses vTaskDelay(100), SDCard.cpp:482 uses a 60000 ms timeout (a duplicate of STATUS_MESSAGE_QUEUE_TIMEOUT_MS), display.cpp:469 refreshes the forecast every literal 60 s, and SDCard.cpp:237-239 hard-codes the 75% and 100-line rotation policy.

**Failure scenario:** The expander address changes (MEMORY.md records it was once wrongly thought to be 0x20) and someone updates WS_EXPANDER_I2C_ADDR. detectWaveshare() still probes 0x24, so the board is misdetected as Matouch and the wrong pinout drives the panel.

**Suggested fix:** Use WS_I2C_SDA, WS_I2C_SCL and WS_EXPANDER_I2C_ADDR in detectWaveshare(). Add WS_PWM_MAX = 97, touchRawMaxX in BoardConfig, and HTTP_TIMEOUT_MS, HTTPS_HANDSHAKE_TIMEOUT_SEC, TASK_WDT_TIMEOUT_SEC, MQTT_POLL_DELAY_MS, SD_LOG_QUEUE_TIMEOUT_MS, SOLAR_FORECAST_REFRESH_SEC, LOG_ROTATE_KEEP_PERCENT and LOG_ROTATE_MIN_LINES in constants.h.

**Verifier notes:** board.cpp:41-42 hard-codes 8, 9 and 0x24 although board_waveshare.h defines WS_I2C_SDA, WS_I2C_SCL and WS_EXPANDER_I2C_ADDR, and board_waveshare.cpp:32 already uses them. The literal 97 appears at board_waveshare.cpp:65 and 74. The touch X map uses a literal 1024 (board.cpp:152). SDCard.cpp:482 uses 60000 while STATUS_MESSAGE_QUEUE_TIMEOUT_MS exists. main.cpp:299 hard-codes 60. All accurate.

### A59. Dead code: unused constants, struct, extern, generated table, parameter and a broken v3 branch

**Severity:** low · **Verdict:** CONFIRMED · **Area:** structure  
**Location:** [`src/constants.h:162`](../src/constants.h#L162)

**What:** These items have no references outside their definition: DATA_SETTING, DATA_ONOFF (161-163), TASK_STACK_LARGE (280), SD_CS_PIN (261), LOG_TOPIC, ERROR_TOPIC (58-59; per-chip topics are built in main.cpp instead), SOLAR_TOKEN_WAIT_SEC, API_SEMAPHORE_WAIT_SEC (191-192; leftovers from the 12-task design), TIME_RETRIES (184), WIFI_SLASH (85), MS_NONE8 and mountain_offset_min[366] (mountain_sunset.h). Also unused: struct LogEntry (types.h:134), `extern Solar solar` in SDCard.cpp:2, and the readingIndex parameter of getBatteryStatus (display.cpp:603). The `#if ESP_ARDUINO_VERSION_MAJOR >= 3` branch at main.cpp:296 uses `esp_task_wdtConfig_t`, which doesn't exist (the real type is esp_task_wdt_config_t), so that branch could never compile. The file header comment at main.cpp:1-13 describes Arduino IDE settings and 'esp32 3.3', which contradicts the deliberate PlatformIO and v2.x setup.

**Failure scenario:** A reader assumes DATA_SETTING/DATA_ONOFF readings are supported, or that API_SEMAPHORE_WAIT_SEC controls something, and tunes a constant that does nothing. If anyone ever tries the documented v3 upgrade path, main.cpp fails to compile on the misspelled type.

**Suggested fix:** Delete the unused constants, LogEntry, the stray extern and the unused parameter. Have mountain_sunset.py stop emitting mountain_offset_min and MS_NONE8, or mark them [[maybe_unused]]. Fix the type name to esp_task_wdt_config_t, or since v3 is deliberately unsupported, remove all the `ESP_ARDUINO_VERSION_MAJOR >= 3` branches in main.cpp and board.cpp and keep the upgrade notes in README. Replace the header comment with a short pointer to platformio.ini and README.

**Verifier notes:** Grep confirms DATA_SETTING, DATA_ONOFF, TASK_STACK_LARGE, SD_CS_PIN, LOG_TOPIC, ERROR_TOPIC, SOLAR_TOKEN_WAIT_SEC, API_SEMAPHORE_WAIT_SEC, TIME_RETRIES, WIFI_SLASH, MS_NONE8, mountain_offset_min and LogEntry are each referenced only at their definition. `extern Solar solar` in SDCard.cpp:2 is unused. main.cpp:296 uses the non-existent type esp_task_wdtConfig_t. The header comment (main.cpp:1-13) says 'esp32 3.3' and gives Arduino IDE settings. getBatteryStatus' readingIndex is unused. DATA_SETTING is at line 162 rather than 161.

### A60. Global `struct tm timeinfo` written concurrently by the loop task and the connectivity task

**Severity:** low · **Verdict:** CONFIRMED · **Area:** structure  
**Location:** [`src/main.cpp:37`](../src/main.cpp#L37)

**What:** timeinfo is a shared global that getLocalTime() fills from display.cpp:551 and 596 (loop task) and from connections.cpp:75 (connectivity task), without any lock. APIs.cpp:417-418 already says to avoid it because of this race. Every user only needs a local struct tm.

**Failure scenario:** WiFi is restored at the moment the 1 s clock update runs. time_init() in the connectivity task overwrites timeinfo while the loop task is midway through strftime() on it, and the clock label shows a torn time (for example hour from one call and minute from the other) for a second. The 'Time synchronized' log line can be torn the same way.

**Suggested fix:** Delete the global and its three externs. Use a local `struct tm t; if (getLocalTime(&t)) ...` in updatePeriodicStatus, onTimeTouched and time_init. Factor the duplicated strftime(showDate ? ... : ...) in display.cpp:555 and 598 into one `static void renderClock()`.

**Verifier notes:** The global timeinfo (main.cpp:37) is written by getLocalTime in the loop task (display.cpp:551, 596) and in the connectivity task (connections.cpp:75), with no lock. Both tasks run at priority 1 on core 1, so a time-slice preemption mid-write or mid-strftime is possible. The effect is a rare, transient torn clock string, so low. Local struct tm variables are the right fix.

### A61. Readings array layout and MAX_READINGS bound are assumed but never checked

**Severity:** low · **Verdict:** CONFIRMED · **Area:** structure  
**Location:** [`src/mqtt.cpp:12`](../src/mqtt.cpp#L12)

**What:** mqtt.cpp sizes lastLoggedValue and hasLoggedBefore by MAX_READINGS (20) and indexes them by reading index (lines 184 and 275-276). display.cpp assumes READINGS_ARRAY is exactly [temps][humidities][batteries] in blocks of ROOM_COUNT (`readings[i + ROOM_COUNT]` at 292 and 642, `readings[i + 2*ROOM_COUNT]` at 295). The only guard is a comment ('currently 15'). numberOfReadings is a runtime extern const, so other TUs cannot check it.

**Failure scenario:** Two more rooms are added, giving 7×3 = 21 readings. The first message for reading index 20 writes lastLoggedValue[20] and hasLoggedBefore[20] past the end of their arrays, corrupting adjacent statics such as lastLoggedInsideCO2. Or a CO2 entry is appended to READINGS_ARRAY, and display.cpp then shows the wrong sensor in a room slot.

**Suggested fix:** In the TU that defines readings, add `static_assert(sizeof(readings)/sizeof(readings[0]) == 3 * ROOM_COUNT, "READINGS_ARRAY layout"); static_assert(sizeof(readings)/sizeof(readings[0]) <= MAX_READINGS, "raise MAX_READINGS");`. Better still, make numberOfReadings a constexpr in state.h (move READINGS_ARRAY into state.cpp) and size the mqtt.cpp arrays with it directly.

**Verifier notes:** lastLoggedValue and hasLoggedBefore are sized by MAX_READINGS=20 (mqtt.cpp:12-13) and indexed by reading index (184, 275-276) with no bound check. display.cpp assumes the [temps][humidities][batteries] layout (292, 295, 642). Adding two rooms (21 readings) would write out of bounds. Nothing currently overflows, so this is a latent bug. The static_assert suggestion is cheap and correct.

### A62. SD log append does three file opens per line; first-time line count reads 1 MB byte-by-byte

**Severity:** low · **Verdict:** PLAUSIBLE · **Area:** structure  
**Location:** [`src/SDCard.cpp:212`](../src/SDCard.cpp#L212)

**What:** For every log line, addLogToSDCard calls SD_MMC.exists(), opens the file in FILE_READ just to get size() (212-223), and then opens it again in FILE_APPEND (274). When the cache is cold (each boot, for both files), it first counts lines with countLinesInFile, which calls File::read() once per byte (27-34) over up to 1 MB, while holding sdMutex. Logging is frequent: every MQTT change, every API update, and 'Solar status updated' every 60 s.

**Failure scenario:** On the first error and first normal log after boot, the SD logger task holds sdMutex while it reads up to 2 MB in one-byte calls through VFS. API and MQTT saveDataBlock calls wait up to MUTEX_TIMEOUT_SD_MS (5 s) and then fail silently ('return false'), so state saves made just after boot can be dropped. In steady state each line costs 3 FAT directory lookups.

**Suggested fix:** Open once with FILE_APPEND and use logFile.size() to decide on rotation (close and rotate only when it exceeds the limit). Count lines in 512-byte chunks, as getLogsFromSDCard already does, or avoid exact counts by rotating on size and having the reader scan backwards from the end.

**Verifier notes:** The code is as described: exists, then FILE_READ open for size(), then FILE_APPEND open per line (SDCard.cpp:212-223, 274), and countLinesInFile reads one byte per call (27-34) under sdMutex. File::read() on SD_MMC goes through a stdio FILE*, so it is buffered, which makes the per-byte cost smaller than 'through VFS each byte' suggests. A >5 s hold that makes saveDataBlock time out is possible on a slow card but unproven. The single-open and chunked-count improvements are valid.

### A63. LV_USE_OS=FREERTOS adds a hidden, unpinned priority-3 render task that bypasses the core-1 design

**Severity:** structure · **Verdict:** CONFIRMED · **Area:** display-board  
**Location:** [`src/board.cpp:191`](../src/board.cpp#L191)

**What:** lv_conf.h sets LV_USE_OS LV_OS_FREERTOS with LV_DRAW_SW_DRAW_UNIT_CNT 1. lv_init() → lv_draw_sw_init() then creates an 'swdraw' thread through lv_thread_init → xTaskCreate. That thread has no core affinity, priority tskIDLE+LV_THREAD_PRIO_HIGH (3, above every application task), an 8 KB internal-RAM stack, and isn't covered by the task watchdog. With a single draw unit there is no parallelism to gain: the loop task hands each draw task to the thread and waits on a semaphore, which only adds context switches. Only the loop task ever touches LVGL, so the OS locking isn't needed either.

**Failure scenario:** The render thread can run on core 0 next to WiFi/LwIP, or pre-empt the MQTT and API tasks on core 1. The stack HWM logging and the 'all tasks on core 1' assumption don't account for it. The extra task-switch overhead slows every frame.

**Suggested fix:** Set LV_USE_OS to LV_OS_NONE: rendering then happens inline in loop(), and the hidden task and 8 KB stack disappear. If you want parallel rendering instead, keep FREERTOS, set LV_DRAW_SW_DRAW_UNIT_CNT 2, and document the extra tasks.

**Verifier notes:** lv_conf.h:86 sets LV_USE_OS to LV_OS_FREERTOS, and :115 sets LV_DRAW_SW_DRAW_UNIT_CNT to 1. lv_draw_sw.c:98 creates a 'swdraw' thread through lv_thread_init, which in lv_freertos.c:93-99 is an unpinned xTaskCreate at tskIDLE_PRIORITY + LV_THREAD_PRIO_HIGH with an 8 KB stack (lv_conf_internal.h:388, 403). The thread is invisible to the 'all tasks on core 1' design and to the HWM logging, as the finding says. One caveat to note: that unpinned thread may currently be offloading rendering to core 0, so switching to LV_OS_NONE moves all rendering onto the core 1 loop. The choice should be made deliberately and documented, but the suggestion is not wrong.

### A64. Display code mutates shared model data and holds dataMutex across LVGL work

**Severity:** structure · **Verdict:** CONFIRMED · **Area:** display-board  
**Location:** [`src/display.cpp:369`](../src/display.cpp#L369)

**What:** updateWeatherDisplay() widens weather.minTemp/maxTemp, so the view writes the model, and those changes are later overwritten by the next fetch. set_solar_values(), updateRoomDisplay(), updateUVDisplay() and updateWeatherDisplay() hold dataMutex for the whole sequence of lv_label_set_text / arc / style calls, each of which allocates and lays out. updateSolarForecastDisplay() already uses the better pattern: lock, copy the struct, unlock, then render. Several functions also read weather.isDay outside the lock.

**Failure scenario:** The MQTT receive task (priority 2) and the API task block on dataMutex for the full duration of an LVGL relayout of 30+ widgets. Any future change that calls back into code taking dataMutex from inside an LVGL event would deadlock.

**Suggested fix:** Snapshot each struct under the mutex (Solar s = solar; Weather w = weather; ...), release it, then render from the copies. Move the min/max widening into fetchWeather(), or compute it into local variables in the display code without writing back.

**Verifier notes:** display.cpp:369-374 writes weather.minTemp and weather.maxTemp from the view, under the mutex. updateRoomDisplay (276), updateUVDisplay (309), updateWeatherDisplay (352), set_solar_values (143) and updateInsideAQDisplay (422) all hold dataMutex across many lv_* calls, and updateInsideAQDisplay reads weather.isDay before taking the lock (421). updateSolarForecastDisplay (display.cpp:475) already uses the copy-then-render pattern. Snapshotting is a valid structural improvement. The deadlock is only a future risk.

### A65. No compile-time check that the readings table matches the fixed-size arrays and display indexing

**Severity:** structure · **Verdict:** CONFIRMED · **Area:** memory  
**Location:** [`src/main.cpp:47`](../src/main.cpp#L47)

**What:** mqtt.cpp:12-13 sizes lastLoggedValue and hasLoggedBefore at MAX_READINGS (20) and indexes them with the readings index. display.cpp:292/295 hard-codes the layout readings[i], readings[i+ROOM_COUNT] and readings[i+2*ROOM_COUNT]. Nothing ties READINGS_ARRAY to either assumption.

**Failure scenario:** A future edit adds six more sensors (21 entries), so updateReadings writes lastLoggedValue[20] past the end and corrupts adjacent statics. Or it reorders or removes rows, so the humidity and battery labels show the wrong sensor, or index past readings[] if there are fewer than 3*ROOM_COUNT entries.

**Suggested fix:** In main.cpp after the readings definition, add: `static_assert(sizeof(readings)/sizeof(readings[0]) <= MAX_READINGS, "..."); static_assert(sizeof(readings)/sizeof(readings[0]) == 3*ROOM_COUNT, "display expects temp/humidity/battery blocks");`. Better still, look entries up by (room, dataType) instead of by positional offset.

**Verifier notes:** mqtt.cpp:12-13 size the arrays at MAX_READINGS (20), and constants.h:8 says 'currently 15'. display.cpp:292-295 hard-codes the readings[i+ROOM_COUNT] and readings[i+2*ROOM_COUNT] layout. Nothing checks at compile time that READINGS_ARRAY (constants.h:10-25) matches either assumption. The static_asserts are correct and cost nothing.

### A66. Seven fetch functions duplicate the same HTTP/JSON boilerplate; factor out a single request helper

**Severity:** structure · **Verdict:** CONFIRMED · **Area:** networking  
**Location:** [`src/APIs.cpp:114`](../src/APIs.cpp#L114)

**What:** fetchUV, fetchWeather, fetchAirQuality, fetchSolarToken and the three solar fetches each repeat begin / addHeader / GET-or-POST / code check / readHttpPayload / end / setReuse / deserialize / error logging, with small inconsistencies: the order of logAndPublish versus http.end(), setReuse(true) only on some calls, errorToString versus a numeric code, and comment headers that say 'returns true on non-HTTP error' when JSON errors actually return false. OTA.cpp also reaches into the same global `http`/`httpsClient` through extern.

**Failure scenario:** Any change to transport handling (timeouts, keep-alive, a CA cert instead of setInsecure, payload reading) has to be made in 8 places. The current drift, such as the mismatched return-value comments and the missing code in the token log, shows this is already happening.

**Suggested fix:** Add a static helper such as `int apiRequest(const char* label, const char* url, const char* postBody /*nullable*/, const char* auth /*nullable*/, JsonDocument& doc)`. It would always call http.end(), log uniformly, and return 0 on success or the error code. Each fetch then shrinks to building the URL and mapping fields. Move `http`/`httpsClient` ownership into APIs.cpp (or a small http module) and expose them to OTA.cpp through a function instead of extern globals. Make readChunkedPayload/readFixedLengthPayload static, since only APIs.cpp uses them.

**Verifier notes:** The duplication is accurate. Seven fetch functions repeat the begin/headers/request/check/read/end/deserialize pattern, with drift between them: setReuse is used only in the solar fetches, token errors use errorToString while the others use reportHttpError, and the header comment on fetchAirQuality (line 253-254) says it returns true on non-HTTP error, while the JSON-error path at line 285 returns false. OTA.cpp reaches into the globals through extern http/httpsClient (OTA.cpp:6-7). The suggested helper would be an improvement.

### A67. main.cpp still owns every module's state; 30+ hand-written extern declarations duplicated across 8 .cpp files

**Severity:** structure · **Verdict:** CONFIRMED · **Area:** structure  
**Location:** [`src/main.cpp:27`](../src/main.cpp#L27)

**What:** After the split, main.cpp still defines all the shared state: the network objects (espClient, mqttClient, http, httpsClient, plus the HTTP timeouts at lines 31-32), mqttMutex/dataMutex, every data struct (weather, uv, solar, solarToken, airQuality, insideAirQuality, solarForecast, readings), storage, statusMessageQueue, logTopic/errorTopic/chipId/macAddress and statusMessageValue. Each consumer then re-declares what it needs with its own `extern` (display.cpp:9-26, APIs.cpp:7-15, mqtt.cpp:4-9, staleness.cpp:3-8, connections.cpp:4-7, logging.cpp:5-9, web_server.cpp:8-9, SDCard.cpp:2, OTA.cpp:6-7). Nothing checks these declarations against the definitions. display.cpp even interleaves its externs with a function (setArcColor at line 19 sits between two extern blocks), a sign of a mechanical move.

**Failure scenario:** Someone changes `char chipId[CHAR_LEN]` in main.cpp to `char chipId[8]`, or changes the type of numberOfReadings. Every TU that declares `extern char chipId[CHAR_LEN]` still compiles and links, but now uses a mismatched declaration: undefined behaviour with no diagnostic. Separately, any new task has to know to add the right externs by hand.

**Suggested fix:** Give each piece of state one owner and declare it once in that owner's header. (1) Add state.h/state.cpp holding the data structs, readings[], numberOfReadings, dataMutex and the dirty flags (move the externs out of types.h), plus restorePersistedState() (see the setup() restore finding). (2) Move mqttClient, espClient and mqttMutex into mqtt.cpp, or into connections.cpp if that module keeps the connect logic, and expose `bool mqttIsConnected()` so display.cpp no longer needs <ArduinoMqttClient.h>. (3) Move http, httpsClient and their setTimeout/setInsecure/handshake setup into an `http_client.cpp` exposing `void httpInit()`, used by APIs.cpp and OTA.cpp. (4) Move logTopic, errorTopic and statusMessageQueue into logging.cpp with `void logging_init(const char* chipId)`. (5) Move chipId and macAddress into a small device-info module. main.cpp then only holds setup()/loop() and task creation.

**Verifier notes:** main.cpp:27-63 defines all shared state. Hand-written externs are repeated in display.cpp:9-26, with setArcColor sitting between the two extern blocks, and also in APIs.cpp:7-15, mqtt.cpp:4-9, staleness.cpp:3-8, connections.cpp:4-7, logging.cpp:5-9, web_server.cpp:8-9, SDCard.cpp:2 and OTA.cpp:6-7. C++ does not check extern declarations against the definitions across TUs, so the mismatch scenario holds. The suggestion is sound.

### A68. setup() repeats load+log boilerplate 8 times; persisted blocks have no single registry

**Severity:** structure · **Verdict:** CONFIRMED · **Area:** structure  
**Location:** [`src/main.cpp:210`](../src/main.cpp#L210)

**What:** Lines 210-253 contain eight near-identical `if (loadDataBlock(FILE, &x, sizeof(x))) logAndPublish("X restored OK") else logAndPublish("X restore failed")` blocks, some followed by an ad-hoc action (invalidateOldReadings, invalidateInsideAirQuality and dirtyInsideAQ, dirtySolarForecast). The save calls for the same blocks are scattered across APIs.cpp and mqtt.cpp. Nothing ties one block's filename, pointer, size and post-restore action together. MEMORY.md already records a bug that came from this: dirtyInsideAQ was not set after restore.

**Failure scenario:** A developer adds a new persisted struct and its saveDataBlock() call but forgets the restore block, or forgets to set its dirty flag after restore. The display then shows '--' until the next update after every reboot, which is the exact bug class already seen with dirtyInsideAQ.

**Suggested fix:** Add a table in state.cpp: `struct PersistedBlock { const char* name; const char* file; void* ptr; size_t size; void (*onRestored)(); };` with one entry each for solar, solarToken, weather, uv, airQuality, insideAirQuality and solarForecast (readings keeps its custom restoreReadings as the onRestored hook or a special entry). restorePersistedState() loops over the table and logs `"%s state restored OK/failed"`. Set every dirty flag to true after restore so none has to be remembered.

**Verifier notes:** main.cpp:210-253 has the eight near-identical load/log blocks with ad-hoc post-restore actions: invalidateOldReadings, dirtyInsideAQ, dirtySolarForecast. The dirtyInsideAQ init at main.cpp:62 (false, while the others are true) shows exactly the bug class described. A table-driven restore is a reasonable suggestion.

### A69. Seven API fetchers duplicate the HTTP→read→parse→error flow; daily and monthly solar are near copies

**Severity:** structure · **Verdict:** CONFIRMED · **Area:** structure  
**Location:** [`src/APIs.cpp:475`](../src/APIs.cpp#L475)

**What:** fetchUV (114), fetchWeather (180) and fetchAirQuality (255) repeat the same sequence: WiFi check, snprintf URL, begin, GET, reportHttpError plus a second logAndPublish, readHttpPayload, end, payload-length check, deserializeJson, missing-field check and errorPublish. fetchCurrentSolar (356), fetchDailySolar (475) and fetchMonthlySolar (539) repeat the Solarman POST with Content-Type and Authorization headers and the setReuse toggles. fetchDailySolar and fetchMonthlySolar differ only in timeType (2/3), strftime format and destination fields. Their success:false handling has already drifted: daily logs 'No success' (line 522) and monthly returns silently (line 584-585). The two files also use different patterns for building log messages.

**Failure scenario:** A fix applied to one copy is missed in the others. For example, the token-rejected handling exists only in fetchCurrentSolar (392-396). If the daily or monthly call is the first to see 'auth invalid token', it logs nothing useful, returns true (no backoff), and keeps failing until the next current-solar call happens to clear the token.

**Suggested fix:** Extract `static bool httpGetJson(const char* label, JsonDocument& doc)`, which handles WiFi, begin, GET, error reporting, payload reading, end and deserialize. Extract `static bool solarmanPost(const char* path, const char* body, const char* label, JsonDocument& doc)`, which also checks `success` and handles 'auth invalid token' in one place. Collapse daily and monthly into `static bool fetchSolarHistory(int timeType, const char* dateFmt, float* buy, float* use, float* gen, time_t* updated, const char* label)`. Each fetchX then shrinks to building the URL/body and copying fields under dataMutex, roughly 20 lines each.

**Verifier notes:** Verified the duplication across fetchUV, fetchWeather, fetchAirQuality and the three Solarman POSTs. fetchDailySolar (475-535) and fetchMonthlySolar (539-597) differ only in timeType, date format and destination fields. Their handling has drifted: daily logs 'No success' at 522, monthly returns silently at 584-585. Token-rejected handling exists only in fetchCurrentSolar (392-396). The failure scenario is slightly overstated: current solar runs first in each pass, so it normally sees the bad token first. However, the token check at line 663 is evaluated once, so after current clears the token, daily and monthly still run in the same pass with an empty Authorization header and log a generic failure. The refactor is valid.

### A70. Display update functions hold dataMutex across dozens of LVGL calls instead of snapshotting

**Severity:** structure · **Verdict:** CONFIRMED · **Area:** structure  
**Location:** [`src/display.cpp:276`](../src/display.cpp#L276)

**What:** updateRoomDisplay (276-300, about 40 LVGL calls), set_solar_values (143-172, including updateChargingStatus and updateGridMetrics), updateUVDisplay (309-343), updateWeatherDisplay (352-411) and updateInsideAQDisplay (422-443) all hold dataMutex while calling LVGL. The MQTT receive task (priority 2) and the API task block on portMAX_DELAY for the whole render. updateSolarForecastDisplay (474-476) already uses a better pattern: copy the struct under the mutex, then render from the copy. set_solar_values also checks solar.currentUpdateTime (line 140) before taking the mutex.

**Failure scenario:** A burst of retained MQTT messages arrives while the loop task is rendering rooms or solar. Each updateReadings() call blocks on dataMutex for the full LVGL pass, which delays message handling and lengthens the time the MQTT task spends away from the socket. If a future LVGL call inside these sections ever calls back into code that takes dataMutex (for example logAndPublish → status message → display), it will deadlock.

**Suggested fix:** Apply the snapshot pattern consistently: `Weather w; AirQuality aq; xSemaphoreTake(...); w = weather; aq = airQuality; xSemaphoreGive(...);`, then render from the locals. For readings, copy only the fields used (output, currentValue, readingState) into a small local array. This keeps critical sections to microseconds and makes the render functions pure functions of a snapshot.

**Verifier notes:** updateRoomDisplay (276-300), set_solar_values (143-172), updateUVDisplay (309-343), updateWeatherDisplay (352-411) and updateInsideAQDisplay (422-443) hold dataMutex across many LVGL calls. updateSolarForecastDisplay (474-476) uses the snapshot pattern. set_solar_values reads solar.currentUpdateTime before taking the mutex (140). The hypothetical deadlock path is not currently reachable, because logAndPublish does not take dataMutex. The structural suggestion to snapshot and then render is still valid.

### A71. Display layer mutates the data model (weather min/max) instead of the producer

**Severity:** structure · **Verdict:** CONFIRMED · **Area:** structure  
**Location:** [`src/display.cpp:369`](../src/display.cpp#L369)

**What:** updateWeatherDisplay() widens weather.minTemp and weather.maxTemp to include the current temperature (lines 369-374). That is data normalisation done in the renderer, so the persisted weather struct depends on whether the display has run. Day/night is handled similarly: invalidateStaleApiData (staleness.cpp:118-127) rewrites weather.isDay. Ownership of `weather` is therefore split between APIs.cpp, display.cpp and staleness.cpp.

**Failure scenario:** fetchWeather saves to SD (APIs.cpp:249) before the display pass has clamped min/max. After a reboot the restored file can have temperature outside [minTemp, maxTemp] until the first render. Any future consumer of weather.minTemp (for example a web endpoint) sees different values depending on render timing.

**Suggested fix:** Do the clamp in fetchWeather just before the mutex-protected write (`weatherMinTemp = std::min(weatherMinTemp, weatherTemperature)` and the same for max), and make updateWeatherDisplay read-only. Document in state.h which module writes each field.

**Verifier notes:** display.cpp:369-374 writes weather.minTemp/maxTemp from the renderer, and staleness.cpp:118-126 rewrites weather.isDay, so ownership of `weather` is split. The stated failure is weak, because the first render after a restore re-applies the clamp. The point that the renderer should be read-only and the producer should normalise the data is factually correct and a sound suggestion.

### A72. Day/night theme code duplicated between display_init() and adjustDayNightMode(); repeated isDay colour ternary

**Severity:** structure · **Verdict:** CONFIRMED · **Area:** structure  
**Location:** [`src/display.cpp:576`](../src/display.cpp#L576)

**What:** The night-theme block (setBacklight, set_basic_text_color, set_arc_night_mode, screen bg and two container borders) appears in adjustDayNightMode (576-590, day and night variants) and again in display_init (683-689). The expression `weather.isDay ? lv_color_hex(COLOR_BLACK) : lv_color_hex(COLOR_WHITE)` is repeated at lines 284, 421, 478 and 535. set_arc_night_mode (196-211) repeats the same two-call pair for 5 arcs instead of looping over an array as it already does for room arcs. adjustDayNightMode's `static bool lastIsDay = false` only works because display_init happens to apply the night theme, an implicit coupling between the two.

**Failure scenario:** A developer adds a new container or arc to the theme in adjustDayNightMode but not in display_init, or the reverse. The first frame after boot then shows the wrong colour on that element until the first day/night transition, which can be hours later.

**Suggested fix:** Add `static void applyTheme(bool isDay)` containing the backlight, text, arc, background and border logic, and call it from both places, with display_init calling applyTheme(false) and setting lastIsDay (make it file-static) to false. Add `static lv_color_t themeTextColor()`. In set_arc_night_mode, build `lv_obj_t** arcs[] = {&ui_TempArcFC, &ui_BatteryArc, &ui_SolarArc, &ui_UsingArc, &ui_UVArc}` and loop over it.

**Verifier notes:** The night theme block is duplicated in adjustDayNightMode (576-590) and display_init (684-689). The isDay colour ternary is repeated at 284, 421, 478 and 535. set_arc_night_mode (195-211) repeats the call pair for 5 arcs although it already loops for the room arcs. lastIsDay=false (569) only works because display_init applies the night theme. All accurate. An applyTheme(bool) helper is a sound fix.

### A73. updateInsideAirQuality() is four copy-pasted branches plus eight parallel statics

**Severity:** structure · **Verdict:** CONFIRMED · **Area:** structure  
**Location:** [`src/mqtt.cpp:295`](../src/mqtt.cpp#L295)

**What:** Lines 295-346 repeat the same block for CO2, PM1, PM2.5 and PM10: range check, log, mutex, set value, state and timestamp, then updateInsideTracking. Only the topic, label, unit, range, field pointers and threshold differ. Eight file-scope statics (16-23) back it. The caller also strcmp's the same four topics (123-126) before dispatching, and updateInsideAirQuality then strcmp's them again. updateReadings likewise has three near-identical range-check blocks (162-179) and two switch statements on dataType (187-200, 209-226).

**Failure scenario:** Adding a fifth sensor (for example VOC) means editing the dispatch list, a new branch, two new statics, the subscribe list in connections.cpp:61-65 and constants.h, and it is easy to miss one. For example, the topic might be subscribed but not dispatched, so messages are silently dropped.

**Suggested fix:** Make it table-driven: `struct InsideSensor { const char* topic; const char* label; const char* unit; float min, max, logThreshold; float* value; ReadingState* state; time_t* lastTime; float lastLogged; bool hasLogged; };` with a static array of four entries. The receive loop looks up the topic once and calls `updateInsideSensor(sensor, msg)`, and mqtt_connect iterates the same table to subscribe. Do the same for readings: a per-dataType table {min, max, logThreshold, format, suffix} replaces both switches and the three if-blocks.

**Verifier notes:** mqtt.cpp:295-346 is four copy-pasted branches backed by eight statics (16-23). The receive loop strcmp's the four topics (123-126), and updateInsideAirQuality strcmp's them again. Subscriptions live separately in connections.cpp:61-65. updateReadings has three range-check blocks (162-179) and two dataType switches (187-200, 209-226). A table-driven design is a sound suggestion.

### A74. Header hygiene: headers pull in heavy libraries they don't need, types.h leaks secrets everywhere, constants.h depends on UI symbols

**Severity:** structure · **Verdict:** CONFIRMED · **Area:** structure  
**Location:** [`src/types.h:5`](../src/types.h#L5)

**What:** types.h, the universal header, includes config.h (WiFi, MQTT and API secrets) and logging.h, so every TU, including board_waveshare-adjacent code and utils users, sees all credentials and macros. connections.h:5-7 includes ArduinoMqttClient, Preferences and WiFi although its four prototypes need none of them. mqtt.h:5-7 includes ArduinoJson, MqttClient and WiFi for prototypes taking char*. OTA.h:5-7 includes HTTPClient, Update and WiFi for two void() functions. APIs.h includes Preferences, ArduinoJson and utils.h. main.cpp compiles only because of these transitive includes (it uses HTTPClient, WiFiClientSecure, MqttClient and Preferences via APIs.h/connections.h). constants.h:29-55 defines ROOM_NAME_LABELS, TEXT_COLOR_LABELS and similar macros referencing `ui_*` objects, so the 'constants' header depends on SquareLine output. connections.cpp:2 includes OTA.h but uses nothing from it. board_config.h defines two `static const BoardConfig` tables in a header, which are copied into every TU including board.h (main.cpp, board.cpp, display.cpp).

**Failure scenario:** Trimming any header, for example removing <HTTPClient.h> from OTA.h, unexpectedly breaks main.cpp. Every edit to config.h or a library header rebuilds all TUs. Unit-testing constants.h natively (test/test_utils) cannot include the UI macros without SquareLine. Secrets end up visible to code that has no reason to see them.

**Suggested fix:** Make each header include only what its declarations need, and have .cpp files include their own dependencies. Remove config.h from types.h and include it only in connections.cpp, APIs.cpp, OTA.cpp, display.cpp (FIRMWARE_VERSION) and web_server.cpp. Move the ui_* label-array macros from constants.h into display.cpp as static arrays. Remove `#include "OTA.h"` from connections.cpp. Move the BoardConfig tables into board.cpp and expose `void board_select()` that performs detection and sets `board` (see the board finding).

**Verifier notes:** Verified: types.h:5-7 includes config.h and logging.h. connections.h, mqtt.h, OTA.h and APIs.h include heavy libraries their prototypes do not need. constants.h:29-30 and the other macros reference ui_* symbols. connections.cpp:2 includes OTA.h but uses nothing from it. main.cpp relies on transitive includes for HTTPClient, WiFiClientSecure and Preferences. One minor inaccuracy: the board_config.h `static const BoardConfig` tables are only emitted in TUs that odr-use them (main.cpp), so they are not really 'copied into every TU'. Moving them to board.cpp is still cleaner.

### A75. Internal helpers and buffers have external linkage; headers export module-internal functions

**Severity:** structure · **Verdict:** CONFIRMED · **Area:** structure  
**Location:** [`src/APIs.cpp:96`](../src/APIs.cpp#L96)

**What:** payloadBuffer (4 KB), urlBuffer and postBuffer (APIs.cpp:95-100) are non-static globals. In board.cpp, rgbpanel, gfx, ts, touchLastX/Y (21-26) and dispFlush, pinInit, touchInit, touchRead (63, 79, 112, 145) all have external linkage without being declared in any header. Headers also export functions that only their own .cpp calls: display.h exports set_solar_values, set_basic_text_color and set_arc_night_mode; connections.h exports setup_wifi, mqtt_connect and time_init; mqtt.h exports updateReadings, updateInsideAirQuality and updateSolarForecast; OTA.h exports updateFirmware; APIs.h exports readChunkedPayload and readFixedLengthPayload. The commit message says helpers used in one file are now static, but these were missed.

**Failure scenario:** A library or a future module defines its own global `gfx`, `ts` or `payloadBuffer` (common names in Arduino GFX examples and TLS code). The result is a multiple-definition link error or, with weak symbols, silent aliasing. Exported internals also invite other modules to call them out of context, for example calling setup_wifi() from another task while connectivity_manager_t is mid-retry.

**Suggested fix:** Mark these buffers, objects and functions `static` in their .cpp files and remove the internal prototypes from the headers. If readChunkedPayload/readFixedLengthPayload should be unit-tested, move them to an http_util.cpp with its own header rather than APIs.h.

**Verifier notes:** payloadBuffer, urlBuffer and postBuffer (APIs.cpp:95-100) are non-static. In board.cpp, rgbpanel, gfx, ts and touchLastX/Y (21-27) and dispFlush, pinInit, touchInit and touchRead have external linkage, and board.h does not declare them. Grep shows the header-exported functions listed are only called within their own .cpp. Making them static and trimming the headers is sound.

### A76. Direct SD writes and log-line formatting duplicated outside SDCard.cpp; shutdown/boot writes bypass sdMutex

**Severity:** structure · **Verdict:** CONFIRMED · **Area:** structure  
**Location:** [`src/main.cpp:100`](../src/main.cpp#L100)

**What:** The '<epoch>|message\n' log-line format is implemented three times: shutdownHandler (main.cpp:100-114), the boot reset-reason write (main.cpp:202-208) and addLogToSDCard (SDCard.cpp:274-287, the only one that sanitises control characters and updates the cached line count). The two copies in main.cpp open ERROR_LOG_FILENAME directly without sdMutex and without updating errorLogLineCount. main.cpp also has to include SD_MMC indirectly for this.

**Failure scenario:** The web /reboot handler (loop task) triggers esp_restart() while the SD logger task is preempted mid-append with sdMutex held. shutdownHandler then opens and appends to the same file without the lock. The cached line count is also off by one per boot or reboot entry, so later rotation keeps slightly fewer lines than intended. If the line format changes (for example adding a level field), main.cpp's copies are easily missed and the log viewer parser breaks on those lines.

**Suggested fix:** Add `void sdcard_append_line_now(const char* filename, const char* message)` to SDCard.cpp. It formats the line (one implementation), takes sdMutex with a short timeout (or skips the lock only in the shutdown path, documented), appends, and bumps the line-count cache. Call it from shutdownHandler and the boot reason path, and move shutdownHandler itself into SDCard.cpp or logging.cpp.

**Verifier notes:** The line format is implemented three times: main.cpp:102-108, main.cpp:202-207 and SDCard.cpp:274-287 (only the last one sanitises control characters). The main.cpp copies bypass sdMutex. The /reboot handler (web_server.cpp:37-42) queues a log message, delays and restarts, so the SD logger task could be mid-append when shutdownHandler writes. Correction: the 'cached line count off by one' claim is wrong. The boot write happens while errorLogLineCount is still -1, so it is counted on first use, and the shutdown write is followed by a reboot that resets the cache. The structural consolidation suggestion is still valid.

### A77. api_manager_t scheduling is ad-hoc: token and UV special-cases inline, OTA timing via an exported global

**Severity:** structure · **Verdict:** CONFIRMED · **Area:** structure  
**Location:** [`src/APIs.cpp:622`](../src/APIs.cpp#L622)

**What:** The scheduler mixes four patterns. The solar token re-implements fetchDue inline (622-627). UV has 20 lines of night-mode state machine with a function-static flag that writes uv and saves to SD inside the scheduler (633-650). Weather, air quality and the three solar calls use fetchDue/attemptFetch. OTA uses `lastOTAUpdateCheck`, an `unsigned long` global exported from OTA.h:12 and compared with a time_t, with no backoff. The HWM logging here uses time() and HWM_LOG_INTERVAL_SEC, while the other four tasks copy-paste the same block with millis() and HWM_LOG_INTERVAL_MS (connections.cpp:97-102, mqtt.cpp:47-52, display.cpp:230-235, SDCard.cpp:485-490).

**Failure scenario:** The OTA server goes down. checkForUpdates sets lastOTAUpdateCheck on failure, so it retries every 300 s with a full TLS handshake and no backoff. With the internet down, every pass makes up to eight 10-20 s HTTPS timeouts, which the comment at 616-619 acknowledges is close to the watchdog limit. Adding a new API means copying one of the three patterns.

**Suggested fix:** Make the scheduler a table: `struct ApiJob { const char* name; ApiBackoff backoff; bool (*fetch)(); time_t (*lastUpdate)(); int intervalSec; bool (*enabled)(); };` covering token (enabled = token empty or expired), UV (enabled = isDay), weather, AQ, the three solar jobs (enabled = token present) and OTA (lastUpdate = an OTA-owned static, which removes the extern). Move the UV night-zeroing into a small `applyUvNight()` called when the day/night state changes. Add `void logStackHwmIfDue(const char* task, unsigned long* lastMs)` in logging.cpp and use it in all five tasks.

**Verifier notes:** Verified the mixed scheduling patterns: inline token check (622-627), the UV night state machine with a function-static flag and an SD save (633-650), fetchDue/attemptFetch for the rest, and the OTA exported global `unsigned long lastOTAUpdateCheck` (OTA.h:12) compared with time_t. The HWM block is copy-pasted in all five tasks. The 'no OTA backoff' failure is mild: on failure the check simply keeps its normal 300 s cadence. The table-driven suggestion is reasonable.

### A78. Battery min/max persisted twice (NVS and the SD Solar block) with NVS silently overriding SD

**Severity:** structure · **Verdict:** CONFIRMED · **Area:** structure  
**Location:** [`src/APIs.cpp:457`](../src/APIs.cpp#L457)

**What:** todayBatteryMin and todayBatteryMax are fields of the Solar struct, which is saved to SD on every solar fetch (APIs.cpp:469, 533, 595). They are also written to NVS via the global `Preferences storage` (457-466) whenever they change. At boot, setup() restores Solar from SD and then overwrites both fields from NVS (main.cpp:266-276). There are two sources of truth, and the Preferences object is a global shared between main.cpp and APIs.cpp purely for this.

**Failure scenario:** The battery charges 1% per fetch all morning. Each new max triggers an NVS write (about 40-60 writes a day) on top of the SD write that already records the same value. If NVS and SD ever disagree (for example the NVS write is skipped on a crash between the two), NVS wins at boot even when the SD block is newer.

**Suggested fix:** Choose one store. Since Solar is already checksummed and saved on every fetch, drop the NVS path along with the global `storage`, and initialise min to 100 and max to 0 when the SD restore fails. If NVS is kept as a fallback for a missing SD card, read it only when loadDataBlock(SOLAR_DATA_FILENAME) fails, and make `storage` a static local in APIs.cpp.

**Verifier notes:** todayBatteryMin and todayBatteryMax are stored both in the SD Solar block (APIs.cpp:469, 533, 595) and in NVS (457-466), and setup() overwrites the SD-restored values from NVS (main.cpp:266-276) through a global Preferences shared only for this. The stated failure, NVS winning over a newer SD value, is unlikely: NVS is written before the SD save in the same function, so NVS is never older. The two-sources-of-truth point and the suggestion to read NVS only as a fallback are still valid.

### A79. utils is designed for native unit tests but platformio.ini has no native test environment

**Severity:** structure · **Verdict:** CONFIRMED · **Area:** structure  
**Location:** [`platformio.ini:12`](../platformio.ini#L12)

**What:** utils.h says 'fully unit-testable on native builds' and test/test_utils/test_utils.cpp exists, but platformio.ini defines only [env:klaussometer] (ESP32-S3), so `pio test` would flash and run on hardware. Pure parsing and format logic (hhmmToMinutes in staleness.cpp, parseApiTime/mountainSetTime in APIs.cpp, formatForecastLine and the charging-time rounding in display.cpp, copyFullTime in mqtt.cpp) lives as statics inside hardware-bound modules, so it cannot be tested at all.

**Failure scenario:** A regression in formatForecastLine's rounding, or in the 'hours/mins' derivation that comments at display.cpp:41-42 say was already wrong once ('1h50m previously displayed as "2 hour 50 mins"'), can only be caught by eye on the device.

**Suggested fix:** Add `[env:native]` with `platform = native`, `test_build_src = yes` and `build_src_filter = -<*> +<utils.cpp>`, and run `pio test -e native` in CI. Move the pure helpers listed above into utils.cpp (or a new time_format.cpp with no Arduino dependencies) and add tests for them.

**Verifier notes:** platformio.ini defines only [env:klaussometer] (line 12), and utils.h:4 claims the helpers are native unit-testable. The tests have also gone stale: test/test_utils/test_utils.cpp calls uv_color(), but utils.h/utils.cpp now define uvColor(), so the test file would not even compile. That strengthens the case for a native env that CI runs. Moving the pure helpers (hhmmToMinutes, parseApiTime, formatForecastLine, charging-time rounding) into utils is sound.

## Appendix B: Refuted findings

- **Second HTTPS handshake in the OTA path has no watchdog feed** ([`src/OTA.cpp:48`](../src/OTA.cpp#L48)): OTA.cpp:28 calls logAndPublish("New firmware version available...") just before updateFirmware(). publishMessageInternal feeds the watchdog when it gets the mqttMutex (logging.cpp:25-26), and that mutex is almost always free when the lower-priority API task is running. So the watchdog is reset between the two HTTPS requests. Each window is then at most about 20 s handshake + 10 s response (+ getString), well under 60 s. A reset can only happen if the Receive task is blocked inside a socket write while holding mqttMutex at that exact moment, which is very unlikely. An explicit esp_task_wdt_reset() at the top of updateFirmware would still be clearer than relying on the hidden feed in logAndPublish, but the boot-loop scenario described is not realistic.
