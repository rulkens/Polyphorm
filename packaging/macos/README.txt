Polyphorm for macOS (@VERSION@)
===============================

A native macOS (Apple Silicon) build of Polyphorm, the Monte Carlo
Physarum Machine for reconstructing the cosmic web from galaxy catalogs.
Source: https://github.com/rulkens/Polyphorm/tree/@TAG@
Upstream: https://github.com/CreativeCodingLab/Polyphorm

Requirements
------------
- A Mac with Apple Silicon (M1 or newer). Intel Macs are not supported.
- macOS 13 (Ventura) or newer.
- 16 GB of RAM recommended; large runs use roughly 8-10 GB.

First launch (important)
------------------------
This app is not signed with an Apple Developer ID, so macOS blocks it the
first time. To allow it:

  1. Move Polyphorm.app to your Applications folder (or anywhere you like).
  2. Double-click it. macOS says it "cannot be opened" / "could not verify".
     Click Done (or Cancel).
  3. Open System Settings > Privacy & Security, scroll down, and click
     "Open Anyway" next to the Polyphorm message. Confirm with your password.

Alternatively, in Terminal:

  xattr -dr com.apple.quarantine /Applications/Polyphorm.app

After that it opens normally.

Using it
--------
Double-click Polyphorm.app and pick a dataset:

  - 2MRS: 34,974 galaxies, all-sky
  - SDSS full: 324,901 galaxies (the catalog behind the published
    Cosmic Slime value-added catalog)
  - SDSS quick slice: 37,655 galaxies, loads fast
  - Open my own dataset: a .bin / _metadata.txt pair (see below)

The simulation starts with 1M agents; the AGENT COUNT dropdown goes up to
10M (slower, higher quality). Controls are listed in the SHORTCUTS panel.
The important ones: F1 toggles the UI, F2 resets, F3 pauses, F6 exports,
F8 clears the trace; left-drag orbits, right-drag pans, scroll zooms.

Output
------
Everything the app writes goes to ~/Polyphorm:

  - export/YYYY-MM-DD_HH-MM-SS/  (F6) trace.bin + deposit.bin (headerless
    float16 volumes, index = z*W*H + y*W + x), export_metadata.txt with
    the dimensions and domain, and halos_measurements.csv.
  - polyphorm.log                 console output, useful when reporting issues.

tools/export_vdb.py in the source repository turns an export folder into
an OpenVDB file for Blender.

Your own data
-------------
Polyphorm reads a packed pair: <name>.bin (float32 x,y,z,weight rows) and
<name>_metadata.txt. pack_catalog.py (included next to this README) makes
one from a CSV with four columns x,y,z,weight and no header row:

  python3 -m pip install numpy
  python3 pack_catalog.py --csv mygalaxies.csv --out mygalaxies

Then choose "Open my own dataset" and pick mygalaxies.bin. If the file is
in Documents, Desktop or Downloads, macOS asks whether Polyphorm may
access that folder; allow it. Keeping datasets in ~/Polyphorm avoids the
question entirely.

Command line
------------
The app can also be run from Terminal, with the same flags as the source
build:

  /Applications/Polyphorm.app/Contents/MacOS/Polyphorm --dataset mygalaxies --agents 2000000
  /Applications/Polyphorm.app/Contents/MacOS/Polyphorm --headless 1000 --export --dataset mygalaxies

--headless N runs N iterations without a window; --export writes an
export at the end (to ~/Polyphorm/export).
