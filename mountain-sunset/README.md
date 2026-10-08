# Mountain sunset generator

Works out, for each day of the year, when the sun drops behind the Table Mountain /
Lion's Head ridge as seen from the house, and writes it as a C lookup table for the
firmware (`src/mountain_sunset.h`).

You only need this to regenerate the table, for example after changing the observer
location, eye height or calibration. The firmware build uses the committed
`src/mountain_sunset.h` and does not need any of this.

## Setup

```sh
cd mountain-sunset
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

## Terrain data (DEM)

The generator needs one tile of the Copernicus GLO-30 surface model, a 30 m height grid
covering 34°S–33°S, 18°E–19°E. It is about 32 MB, so it is not committed.

**Automatic:** the generator downloads it next to `mountain_sunset.py` on first run.

**Manual:** if the automatic download fails, or you want it beforehand:

```sh
curl -o Copernicus_DSM_COG_10_S34_00_E018_00_DEM.tif \
  https://copernicus-dem-30m.s3.amazonaws.com/Copernicus_DSM_COG_10_S34_00_E018_00_DEM/Copernicus_DSM_COG_10_S34_00_E018_00_DEM.tif
```

The file should be 33,743,971 bytes. It comes from the Copernicus DEM open-data bucket
on AWS (`s3://copernicus-dem-30m/`, no account needed). If that URL ever moves, the same
tile is available from the Copernicus Data Space Ecosystem; set `DEM_URL` or `DEM_PATH`
at the top of `mountain_sunset.py` to match.

Attribution: produced using Copernicus WorldDEM-30 © DLR e.V. 2010-2014 and © Airbus
Defence and Space GmbH 2014-2018, provided under COPERNICUS by the European Union and ESA;
all rights reserved.

## Run

```sh
python mountain_sunset.py
cp mountain_sunset.h ../src/mountain_sunset.h
```

This writes `mountain_sunset.h`, `mountain_sunset.csv` (per-day detail) and `horizon.png`
(the skyline with the sunset path) into the current directory. Check `horizon.png` looks
right before copying the header into the firmware.

All settings (location, eye height, calibration) are constants at the top of
`mountain_sunset.py`.

## Test

```sh
pytest -q
```

Runs offline on a synthetic ridge, so it does not need the DEM.
