# Pyxel module: vendored code and licenses

`pyxel.app.elf` runs Pyxel games (https://github.com/kitao/pyxel): an embedded
MicroPython interpreter plus a C implementation of the `pyxel` API.

| Component | Where | License | Notes |
|---|---|---|---|
| MicroPython v1.29.0 (`ports/embed` package) | `micropython/py`, `micropython/shared`, `micropython/genhdr`, `micropython/extmod` | MIT, Damien P. George and contributors | Local patches below |
| Pyxel 2.9.9 engine behaviour | `src/*.c`, `pylib/pyxel.py` | MIT, Takashi Kitao | C port of `crates/pyxel-core` (canvas, image, tilemap, input, system, resource loading, the built-in font data, sound sequencing and tones) and the public API surface |
| LodePNG | `../pico8/fake08-src/libs/lodepng` (compiled from there, as C) | zlib, Lode Vandevenne | PNG decoding and raw DEFLATE for `.pyxapp` / `.pyxres` ZIPs |
| `pylib/enum.py`, `random.py`, `itertools.py` | `pylib/` | Meshpunk (MIT) | Subsets of the CPython modules the games import |
| `src/libm_extra.c` | `src/` | Meshpunk (MIT) | Float math the firmware does not export |

Games are supplied by the user; each carries its own license.

## Local MicroPython patches (tagged `MESHPUNK`)

- `py/objstr.c`: `str.ljust`, `str.rjust`, `str.zfill` (CPython padding methods).
- `py/objlist.c`: slice assignment accepts any iterable (`lst[:] = generator`).

## Configuration and generated headers

`micropython/mpconfigport.h` is the feature set. `micropython/genhdr/` was
generated from that same feature set; the settings that only affect code
generation (NLR/GC register capture, file reader, computed goto, the
`MICROPY_WRAP_*` placement of hot functions in `.iram.text`, the VM hook that
feeds audio) can change freely, but
enabling or disabling a module, builtin or feature requires regenerating
genhdr:

1. Download MicroPython v1.29.0 and copy `py/objstr.c` and `py/objlist.c`
   from `micropython/py/` over upstream's (they carry the patches).
2. Make a directory next to `py/` holding `mpconfigport.h` and
   `micropython_embed.mk`:
   ```
   MICROPYTHON_TOP = ..
   SRC_QSTR += extmod/modjson.c
   include $(MICROPYTHON_TOP)/ports/embed/embed.mk
   ```
3. From that directory run `make -f micropython_embed.mk`, then copy the
   generated `micropython_embed/{py,shared,genhdr}` and
   `extmod/{modjson.c,modplatform.h}` into `micropython/`.

The `_pyxel` native module interns its names at runtime (`qstr_from_str`), so
`src/` never needs genhdr regeneration.
