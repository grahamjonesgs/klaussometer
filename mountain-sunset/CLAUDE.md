# Mountain sunset — project brief for Claude Code

## Goal
Show on Graham's ESP32 weather display the time the sun disappears behind the
Table Mountain / Lion's Head ridge (Kloof Nek saddle) as seen from his house at
22 Watsonia Street, Devils Peak Estate, Cape Town. This is earlier than the
astronomical sunset the weather API reports.

## Architecture (decided)
- Heavy work is done offline in Python: DEM → horizon profile → per-day solar crossing.
- The output is a generated C header with 366-entry lookup tables. The ESP32 only indexes it.
- The ridge is fixed and the sun's path repeats yearly. Reusing the leap reference year's
  table every year costs about ±1 min, which is acceptable.
- On-device computation (horizon profile of ~1.4 KB plus the NOAA solar routine in C) is a
  possible later option. Do not build it unless asked.

## Files
- `mountain_sunset.py` — generator. All configuration constants are at the top.
- `tests/test_synthetic.py` — offline end-to-end test on a synthetic ridge (`pytest -q`).
- `requirements.txt`
- `README.md` — setup, DEM download (automatic or manual) and run instructions.
- Generated outputs `mountain_sunset.h`, `mountain_sunset.csv` and `horizon.png` are committed.
  The cached DEM `.tif` (32 MB) is not; it is git-ignored and downloaded on first run.

## Method
1. DEM: Copernicus GLO-30 DSM tile S34 E018 (downloaded from AWS open data on first run).
   This is a surface model, so it includes trees and buildings. `D_MIN = 150 m` skips the
   immediate surroundings.
2. Horizon: rays from 225° to 320° azimuth in 0.05° steps, out to 15 km in 10 m steps.
   Apparent terrain angle is `atan((z − z_obs − d²/2R′)/d)` with `R′ = R/(1−0.13)`
   (curvature plus terrestrial refraction).
3. Sun: pvlib NREL SPA apparent elevation and azimuth, sampled every 20 s between 14:00 and
   20:45 SAST. The crossing is where the upper limb meets the horizon,
   `elev + 0.267° = H(az)`, linearly interpolated between samples.
4. Multiple crossings (the sun reappearing in a gap) are flagged. The header uses the last one.
5. Times are SAST (UTC+2). South Africa has no DST.

## Header contract (keep stable — firmware depends on it)
- `uint16_t mountain_sunset_min[366]` — minutes after midnight SAST; `0xFFFF` means none.
- `uint8_t mountain_offset_min[366]` — minutes before astronomical sunset; `0xFF` means none.
- `ms_index(month 1..12, mday)` uses the leap-year layout, so Feb 29 has its own slot.

## Status
- Run on the real Copernicus DEM with the dropped-pin location (`OBS_LAT` / `OBS_LON`,
  `EYE_HEIGHT_M`). The output is committed: `horizon.png`, `mountain_sunset.csv` and the
  header, copied to `src/mountain_sunset.h`.
- `PEAK_FIXES` raises Lion's Head to its surveyed 669 m; the 30 m DSM flattens it to about 600 m.
- The synthetic test passes. Astronomical sunsets match Cape Town (about 20:01 on 1 Jan and
  17:45 on 21 Jun, SAST).
- Integrated in the firmware: `APIs.cpp` looks up the day's time and shows it in `FCMountainSet`.
- Checked against one observation: on 7 Oct 2026 Graham saw the sun disappear at exactly the
  table time, 18:27 SAST (18:26:53 in the CSV; ridge at az 266°, 5.2° high, 3.5 km away).
  `CALIBRATION_MIN` stays 0.

## Next tasks
1. Optional: a further check at another time of year, where the sun sets behind a different
   part of the ridge. If it is off, set `CALIBRATION_MIN` or tune `D_MIN`.
2. Optional: a script target or Makefile that regenerates the header straight into the
   firmware tree.

## Working preferences
- Prioritise accuracy over overclaiming. State clearly what is verified and what is assumed.
- Keep communication concise. Integrate with the existing firmware structure rather than
  adding new frameworks.
