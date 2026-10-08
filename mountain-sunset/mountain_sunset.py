#!/usr/bin/env python3
"""
mountain_sunset.py - when does the sun go behind the Table Mountain / Lion's Head ridge?

1. Builds a terrain horizon profile H(azimuth) for the observer from a DEM.
2. For every day of a leap reference year, finds when the sun's upper limb
   disappears behind that horizon (pvlib / NREL SPA solar position).
3. Writes:
     mountain_sunset.h    C header for the ESP32 (366-entry tables, index by month/day)
     mountain_sunset.csv  human-readable table for checking
     horizon.png          horizon profile + the year's disappearance points (if matplotlib present)

Install:  pip install numpy scipy pandas rasterio pvlib matplotlib
Run:      python mountain_sunset.py
"""
import math
import os
import urllib.request

import numpy as np
import pandas as pd
import pvlib
import rasterio
from rasterio.windows import from_bounds
from scipy.ndimage import map_coordinates

# ---------------------------------------------------------------- configuration
OBS_LAT = -33.9366529       # dropped pin, 22 Watsonia St (from Graham)
OBS_LON = 18.4282683        # dropped pin, 22 Watsonia St (from Graham)
EYE_HEIGHT_M = 2.0          # eye height above ground (from Graham)
OBS_GROUND_M = None         # ground elevation override (m); None = read from DEM
TZ = "Africa/Johannesburg"  # SAST = UTC+2, no DST

# Copernicus GLO-30 surface model (includes buildings/trees). Tile S34 E018 covers -34..-33, 18..19.
# Not committed (32 MB): downloaded next to this script on first run. See README.md.
DEM_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "Copernicus_DSM_COG_10_S34_00_E018_00_DEM.tif")
DEM_URL = ("https://copernicus-dem-30m.s3.amazonaws.com/"
           "Copernicus_DSM_COG_10_S34_00_E018_00_DEM/"
           "Copernicus_DSM_COG_10_S34_00_E018_00_DEM.tif")
# A GeoTIFF built from City of Cape Town contours (EPSG:4326) can be dropped in via DEM_PATH.

AZ_MIN, AZ_MAX, AZ_STEP = 225.0, 320.0, 0.05   # deg; sea-level sunsets span ~241..299, wider for a high ridge
D_MIN, D_MAX, D_STEP = 150.0, 15000.0, 10.0    # m along each ray; D_MIN skips your own roof
K_REFRACTION = 0.13          # terrestrial refraction coefficient
SUN_SEMI_DIAM = 0.267        # deg
REF_YEAR = 2028              # leap year -> 366 entries
SEARCH_START_H, SEARCH_END_H = 14.0, 20.75     # local-time search window (hours)
STEP_S = 20                  # solar sampling step (s); crossings are interpolated
CALIBRATION_MIN = 0.0        # add after one real observation (+ = later)
R_EARTH = 6371000.0
# Narrow summits the 30 m DSM flattens: (lat, lon, true elevation m, radius m).
# The DEM is raised by a cosine bump so its highest cell within `radius` reaches the true
# elevation, tapering to no change at `radius`. Lion's Head: DSM max 600 m vs surveyed 669 m.
PEAK_FIXES = [(-33.9351, 18.3887, 669.0, 150.0)]


# ---------------------------------------------------------------- DEM
def download_dem():
    """Fetch the DEM tile. Downloads to a .part file first so an interrupted
    download is never mistaken for a complete tile on the next run."""
    print(f"Downloading DEM tile (~32 MB) to {DEM_PATH} ...")
    tmp = DEM_PATH + ".part"
    urllib.request.urlretrieve(DEM_URL, tmp)
    os.replace(tmp, DEM_PATH)


def load_dem():
    if not os.path.exists(DEM_PATH):
        download_dem()
    pad = 0.2  # degrees around the observer (~20 km)
    with rasterio.open(DEM_PATH) as src:
        win = from_bounds(OBS_LON - pad, OBS_LAT - pad, OBS_LON + pad, OBS_LAT + pad,
                          src.transform).round_offsets().round_lengths()
        z = src.read(1, window=win, boundless=True, fill_value=-32767).astype(np.float64)
        tf = src.window_transform(win)
    z[z < -1000] = np.nan  # outside the tile / nodata
    fix_peaks(z, tf)
    return z, tf


def fix_peaks(z, tf):
    """Raise summits listed in PEAK_FIXES (in place) to their surveyed height."""
    rows, cols = np.indices(z.shape)
    lat = tf.f + (rows + 0.5) * tf.e
    lon = tf.c + (cols + 0.5) * tf.a
    for plat, plon, elev, radius in PEAK_FIXES:
        dy = (lat - plat) * math.pi / 180 * R_EARTH
        dx = (lon - plon) * math.pi / 180 * R_EARTH * math.cos(math.radians(plat))
        r = np.hypot(dx, dy)
        near = r < radius
        if not near.any():
            continue
        # Centre the bump on the DSM's own summit cell, which may sit a cell or two off the survey point.
        k = np.nanargmax(np.where(near, z, -np.inf))
        r = np.hypot(dx - dx.flat[k], dy - dy.flat[k])
        lift = elev - z.flat[k]
        if lift <= 0:
            continue
        bump = np.where(r < radius, 0.5 * (1 + np.cos(np.pi * r / radius)), 0.0)
        z += lift * bump
        print(f"Peak fix ({plat:.4f}, {plon:.4f}): DSM {elev - lift:.0f} m -> {elev:.0f} m")


def sample(z, tf, lat, lon):
    """Bilinear DEM sample at lat/lon arrays (north-up EPSG:4326 raster)."""
    col = (np.asarray(lon) - tf.c) / tf.a - 0.5   # -0.5: pixel centres
    row = (np.asarray(lat) - tf.f) / tf.e - 0.5
    return map_coordinates(z, [row, col], order=1, mode="constant", cval=np.nan)


# ---------------------------------------------------------------- horizon
def horizon_profile(z, tf):
    if OBS_GROUND_M is not None:
        z_ground = OBS_GROUND_M
    else:
        z_ground = float(sample(z, tf, np.array([OBS_LAT]), np.array([OBS_LON]))[0])
    z_obs = z_ground + EYE_HEIGHT_M

    az = np.arange(AZ_MIN, AZ_MAX + 1e-9, AZ_STEP)
    d = np.arange(D_MIN, D_MAX + 1e-9, D_STEP)
    A, D = np.meshgrid(np.radians(az), d, indexing="ij")
    lat = OBS_LAT + np.degrees(D * np.cos(A) / R_EARTH)
    lon = OBS_LON + np.degrees(D * np.sin(A) / (R_EARTH * math.cos(math.radians(OBS_LAT))))
    zt = sample(z, tf, lat.ravel(), lon.ravel()).reshape(lat.shape)

    r_eff = R_EARTH / (1.0 - K_REFRACTION)          # curvature + terrestrial refraction
    ang = np.degrees(np.arctan2(zt - z_obs - D ** 2 / (2.0 * r_eff), D))
    ang = np.where(np.isnan(ang), -90.0, ang)
    imax = ang.argmax(axis=1)
    H = ang[np.arange(len(az)), imax]
    return az, H, d[imax], z_ground


# ---------------------------------------------------------------- sun
def solve_year(az, H, dist, z_ground):
    days = pd.date_range(f"{REF_YEAR}-01-01", f"{REF_YEAR}-12-31", freq="D", tz=TZ)
    sunsets = pvlib.solarposition.sun_rise_set_transit_spa(days, OBS_LAT, OBS_LON)["sunset"]
    rows = []
    for day in days:
        t = pd.date_range(day + pd.Timedelta(hours=SEARCH_START_H),
                          day + pd.Timedelta(hours=SEARCH_END_H), freq=f"{STEP_S}s")
        sp = pvlib.solarposition.get_solarposition(t, OBS_LAT, OBS_LON,
                                                   altitude=z_ground + EYE_HEIGHT_M)
        sun_az = sp["azimuth"].to_numpy()
        f = (sp["apparent_elevation"].to_numpy() + SUN_SEMI_DIAM
             - np.interp(sun_az, az, H, left=-90.0, right=-90.0))
        tsec = (t - day).total_seconds().to_numpy()
        idx = np.where((f[:-1] > 0) & (f[1:] <= 0))[0]           # upper limb disappears
        cross = tsec[idx] + f[idx] / (f[idx] - f[idx + 1]) * STEP_S

        sunset_min = (sunsets[day] - day).total_seconds() / 60.0
        if len(cross):
            first = cross[0] / 60.0 + CALIBRATION_MIN
            last = cross[-1] / 60.0 + CALIBRATION_MIN
            a = float(np.interp(cross[-1], tsec, sun_az))
            ridge_h = float(np.interp(a, az, H))
            ridge_d = float(dist[np.abs(az - a).argmin()])
        else:
            first = last = a = ridge_h = ridge_d = np.nan
        rows.append(dict(date=day.strftime("%m-%d"), sunset=sunset_min,
                         mountain_first=first, mountain_last=last,
                         n_disappearances=len(cross), sun_az=a,
                         ridge_elev_deg=ridge_h, ridge_dist_m=ridge_d))
    return pd.DataFrame(rows)


# ---------------------------------------------------------------- output
def ms_index_py(month, mday):
    """Python mirror of the C ms_index(): leap-year day index, 0..365."""
    cum = [0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335]
    return cum[month - 1] + mday - 1


def hms(m):
    if np.isnan(m):
        return ""
    s = int(round(m * 60))
    return f"{s // 3600:02d}:{s % 3600 // 60:02d}:{s % 60:02d}"


def write_csv(df, path="mountain_sunset.csv"):
    out = df.copy()
    for c in ("sunset", "mountain_first", "mountain_last"):
        out[c] = out[c].map(hms)
    out["offset_min"] = (df["sunset"] - df["mountain_last"]).round(1)
    out.round(2).to_csv(path, index=False)


def c_array(ctype, name, values, comment):
    lines = [f"// {comment}", f"static const {ctype} {name}[366] = {{"]
    for i in range(0, 366, 12):
        lines.append("    " + ", ".join(f"{v:5d}" for v in values[i:i + 12]) + ",")
    lines.append("};")
    return "\n".join(lines)


def write_header(df, z_ground, path="mountain_sunset.h"):
    last = [0xFFFF if np.isnan(v) else int(round(v)) for v in df["mountain_last"]]
    off = []
    for s, m in zip(df["sunset"], df["mountain_last"]):
        off.append(0xFF if np.isnan(m) else max(0, min(254, int(round(s - m)))))
    hdr = f"""// Generated by mountain_sunset.py - do not edit by hand.
// Observer {OBS_LAT:.5f}, {OBS_LON:.5f}, ground {z_ground:.0f} m + eye {EYE_HEIGHT_M} m.
// Times are SAST (UTC+2). Reference year {REF_YEAR}; reuse every year (error ~ +/-1 min).
#pragma once
#include <stdint.h>

#define MS_NONE16 0xFFFF
#define MS_NONE8  0xFF

{c_array("uint16_t", "mountain_sunset_min", last,
         "Minutes after midnight SAST when the sun's upper limb finally disappears behind the ridge.")}

{c_array("uint8_t", "mountain_offset_min", off,
         "Minutes before astronomical sunset (subtract from the weather API's sunset time).")}

// month 1..12, mday 1..31 -> table index (leap-year layout, so Feb 29 has its own slot)
static inline int ms_index(int month, int mday) {{
    static const uint16_t cum[12] = {{0,31,60,91,121,152,182,213,244,274,305,335}};
    return cum[month - 1] + mday - 1;
}}
"""
    with open(path, "w") as fh:
        fh.write(hdr)


def plot(az, H, df, path="horizon.png"):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        return
    fig, ax = plt.subplots(figsize=(11, 4))
    ax.plot(az, H, "k-", lw=1, label="terrain horizon")
    ax.scatter(df["sun_az"], df["ridge_elev_deg"], c=np.arange(len(df)), cmap="twilight",
               s=8, label="sun disappears (colour = day of year)")
    ax.set_xlabel("azimuth (deg from north)")
    ax.set_ylabel("apparent elevation (deg)")
    ax.set_xlim(AZ_MIN, AZ_MAX)
    ax.grid(alpha=0.3)
    ax.legend(loc="upper right")
    fig.tight_layout()
    fig.savefig(path, dpi=130)


def main():
    z, tf = load_dem()
    az, H, dist, z_ground = horizon_profile(z, tf)
    print(f"Observer ground {z_ground:.0f} m; horizon {H.min():.2f}..{H.max():.2f} deg "
          f"over az {AZ_MIN:.0f}..{AZ_MAX:.0f}")
    df = solve_year(az, H, dist, z_ground)
    write_csv(df)
    write_header(df, z_ground)
    plot(az, H, df)
    off = df["sunset"] - df["mountain_last"]
    print(f"Mountain sunset is {off.min():.0f}-{off.max():.0f} min before astronomical sunset.")
    multi = df[df["n_disappearances"] > 1]
    if len(multi):
        print(f"{len(multi)} days where the sun reappears in a gap (e.g. Kloof Nek) "
              f"before finally setting - see mountain_sunset.csv.")
    print("Wrote mountain_sunset.h, mountain_sunset.csv, horizon.png")


if __name__ == "__main__":
    main()
