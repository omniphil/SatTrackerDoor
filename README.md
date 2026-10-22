# TERMinator Satellites

A satellite tracker door for BBSes: where the ISS and friends are right now, and when they pass over your town.

- **TRACE** callers get a live map of the Earth, NASA's Blue Marble by day and Black Marble city lights by night,
  with the day/night line where it really is. Satellites in the chosen group move across it; the one you follow shows
  its track, its footprint and its name. Below: its altitude, speed and position, its next passes over your town
  (with the visible ones marked) and a sky chart of where to look. Mouse and keyboard.
- **JPEG XL** callers (CTerm terminals that draw JPEG XL) get the TRACE map and panels themselves: the module is
  compiled into the door (`door/pix_sats.c`), draws on the BBS at 640x480 (its 1x size) with `earth.bin` read from beside the door,
  and the changed parts of each frame go out as JPEG XL pictures shrunk to the caller's screen (640x400 at 80x25),
  paced and sharpened like the News and DOOM doors' JPEG XL modes. A tile is resent only once it has drifted a
  visible amount from what the caller has, so slowly moving satellite dots don't resend every frame, and after 3 s
  without a key or the mouse the map is drawn 4 times a second. Keys and the mouse (xterm SGR reporting) work as in
  TRACE. Needs libjxl on the box (loaded at run time). On the BBS box (Intel N150) about 15% of a core left alone
  and ~45% while the mouse moves; ~10-50 KB/s left alone (more satellites in the group, more bytes), ~25-110 KB/s
  while the mouse moves. Old terminals' X10 mouse reports work too. `SATDOOR_LOG=<file>` or `saves/<user>/jxl.log` has the frame rate and bandwidth;
  `tools/fake_cterm.py` plays a fake CTerm terminal against the door and saves the screen as PNGs.
- **ANSI** callers get the same live map in 24-bit half blocks at 79x24, a satellite list and the same passes.

Groups: space stations, the brightest, weather, ham radio, GPS and science satellites (CelesTrak's lists).

## How it works

| Piece | Where | What |
|---|---|---|
| `door/satdoor` | the BBS | Start menu, orbit fetch + cache (CelesTrak), place search (Open-Meteo), the ANSI view, the JPEG XL view (the module compiled in), the pipe to the module |
| `door/sats.wasm` | the caller's TERMinator | Draws the map and panels; runs the orbit maths every frame |
| `door/earth.bin` | sent once, cached by TERMinator | The day and night Earth pictures (123 KB, from `tools/make_earth.py`) |

- **One orbit engine, two users.** `module/src/orbit.h` is compiled into both the door and the module: SGP4 for
  near-Earth orbits (checked against the reference `sgp4` library to within 4 metres) and Kepler plus J2 drift for
  high orbits (about 35 km off for GPS and geostationary satellites, under a pixel on the map). Pass predictions
  match Skyfield to the second (`tools/check_orbit.py`).
- **The door sends orbit elements, not positions**, so hundreds of satellites move smoothly without streaming.
- **CelesTrak** asks for no more than a fetch every two hours; each group is cached for `cache_hours` (default 8).
- **Home place:** the caller picks it (city, "City, ST" or ZIP), or, with `weather_saves` set, the tracker borrows the
  place the caller gave the Weather door. Pass times are shown in the home place's time zone.
- **TRACE needs TERMinator 1.1.3+** (mouse support); older ones are told to update and get ANSI.

## Install

```
make -C module            # needs wasi-sdk 34+ in ~/tools
make -C door install      # copies sats.wasm and earth.bin beside the door, makes sats.ini, saves/, cache/
```

On the BBS the door needs `satdoor` (`chmod +x`), `sats.wasm`, `earth.bin` and `sats.ini` in one folder, write access
to it, and `curl` on the PATH. The door entry is `satdoor <dir containing door32.sys>`.

## Keys

| | TRACE | ANSI |
|---|---|---|
| Follow a satellite | click it (hover shows its name), Up/Down, wheel | Up/Down |
| Group | click a chip, 1-6 | 1-6 |
| The ISS | I | I |
| A pass on the sky chart | click the pass, P | |
| Home place | click the place, L | L |
| Quit | the X, Esc, Q | Esc, Q |

## Testing

```
python3 door/test_door.py            # fake TERMinator: asset, orbits, group change, search, pick, prefs
python3 door/test_door.py --bin      # binary frames
python3 door/test_door.py --nomouse  # pre-1.1.3 TERMinator: TRACE refused
python3 tools/ansi_shot.py out.png 2 @WAIT       # the ANSI view, rendered to a PNG
make -C door preview && (cd door && ./preview --group 2 out.ppm)   # the TRACE module, natively
```

## Licence

GPL-3.0: see `COPYING`. The module embeds the Ubuntu font, baked to bitmaps, under the Ubuntu Font Licence 1.0
(`module/src/UBUNTU-FONT-LICENCE.txt`). stb_image (`door/third_party/`) is public domain. `data/earth.bin` is built
from NASA imagery, which is public domain.

The repository holds sources and `data/earth.bin`: build `sats.wasm` with `make -C module` (wasi-sdk) and the door
with `make -C door`.

## Credits

Orbits: [CelesTrak](https://celestrak.org). Earth: NASA Blue Marble Next Generation and Black Marble 2016 (public
domain). Places: Open-Meteo (CC BY 4.0). Fonts: Ubuntu (Ubuntu Font Licence 1.0). stb_image: public domain.
BBS door by JSONBourne.
