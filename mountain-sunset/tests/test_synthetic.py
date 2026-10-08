"""Offline end-to-end test: synthetic ridge ~4 km west of the observer, no download needed."""
import os
import sys

import numpy as np
import pandas as pd
import rasterio
from rasterio.transform import from_origin

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import mountain_sunset as ms  # noqa: E402


def make_dem(path):
    res, n = 1 / 3600, 3600
    lon0, lat0 = 18.0, -33.0
    x = np.arange(n) * res + lon0 + res / 2
    y = lat0 - np.arange(n) * res - res / 2
    lon, lat = np.meshgrid(x, y)
    dx = (lon - ms.OBS_LON) * 92500
    dy = (lat - ms.OBS_LAT) * 111000
    z = 100 + 600 * np.exp(-((dx + 4000) / 800) ** 2) * (1 - 0.5 * np.exp(-(dy / 400) ** 2))
    with rasterio.open(path, "w", driver="GTiff", height=n, width=n, count=1,
                       dtype="float32", crs="EPSG:4326",
                       transform=from_origin(lon0, lat0, res, res)) as d:
        d.write(z.astype("float32"), 1)


def test_end_to_end(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    # DEM_PATH points at the real tile beside the script; never overwrite it with the synthetic one
    monkeypatch.setattr(ms, "DEM_PATH", str(tmp_path / "synthetic_dem.tif"))
    make_dem(ms.DEM_PATH)
    ms.main()
    df = pd.read_csv("mountain_sunset.csv")
    assert len(df) == 366
    # Cape Town astronomical sunset sanity (SAST)
    assert "19:58" <= df.loc[0, "sunset"] <= "20:04"            # 1 Jan
    assert "17:42" <= df.loc[ms.ms_index_py(6, 21), "sunset"] <= "17:48"  # 21 Jun
    assert (df["offset_min"] > 0).all()                         # ridge always sets the sun early
    hdr = open("mountain_sunset.h").read()
    assert "mountain_sunset_min[366]" in hdr and "ms_index" in hdr
