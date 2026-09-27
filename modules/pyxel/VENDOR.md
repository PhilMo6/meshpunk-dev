# Pyxel module: vendored code and licenses

`pyxel.app.elf` runs Pyxel games (https://github.com/kitao/pyxel): an embedded
MicroPython interpreter plus a C implementation of the `pyxel` API.

| Component | Where | License | Notes |
|---|---|---|---|
| MicroPython v1.29.0 (`ports/embed` package) | `micropython/py`, `micropython/shared`, `micropython/genhdr`, `micropython/extmod` | MIT, Damien P. George and contributors | Local patches below |
| Pyxel 2.9.9 engine behaviour | `src/*.c`, `pylib/pyxel.py` | MIT, Takashi Kitao | C port of `crates/pyxel-core`: canvas (including rotate/scale and perspective blits), image, tilemap (`collide`, TMX), input, system, resource loading and saving, the built-in font data, BDF fonts, the sound command model with both MML parsers, WAV decoding, tones, channels and voices. Plus the public API surface |
| LodePNG | `../pico8/fake08-src/libs/lodepng` (compiled from there, as C) | zlib, Lode Vandevenne | PNG decoding, raw DEFLATE for `.pyxapp` / `.pyxres` ZIPs, CRC-32 for saved `.pyxres` |
| `pylib/enum.py`, `random.py`, `itertools.py` | `pylib/` | Meshpunk (MIT) | Subsets of the CPython modules the games import |
| `src/libm_extra.c` | `src/` | Meshpunk (MIT) | Float math the firmware does not export |

Games are supplied by the user; each carries its own license.

## Local MicroPython patches (tagged `MESHPUNK`)

- `py/objstr.c`: `str.ljust`, `str.rjust`, `str.zfill` (CPython padding methods).
- `py/objlist.c`: slice assignment accepts any iterable (`lst[:] = generator`).
- `py/objexcept.c`, `py/obj.h`: the OSError subclasses `FileExistsError`,
  `FileNotFoundError` and `IsADirectoryError`. `mpconfigport.h` puts them in
  the builtins through `MICROPY_PORT_BUILTINS`, and genhdr holds their three
  qstrs.
- `py/map.c`: the first probed slot is a Fibonacci multiply of the hash
  (`MAP_POS`), and the lookup-cache index is a Fibonacci multiply of the key.
- `py/mpstate.h`: lookup-cache entries are `uint16_t`, not `uint8_t`.
- `PX_IRAM` on hot functions in `py/argcheck.c`, `bc.c`, `gc.c`, `obj.c`,
  `objfloat.c`, `objfun.c`, `objint.c`, `objmodule.c`, `objtuple.c`,
  `objtype.c`, `qstr.c`, `runtime.c`. The macro is defined in
  `mpconfigport.h`; build.ps1 compiles those files as `$iram_files`.

## Configuration and generated headers

`micropython/mpconfigport.h` is the feature set. `micropython/genhdr/` was
generated from that same feature set. These settings only affect code
generation and can change freely:
- NLR/GC register capture;
- the file reader;
- computed goto;
- the object representation (REPR_C builds against this genhdr);
- the lookup-cache size;
- the `MICROPY_WRAP_*` / `PX_IRAM` placement of hot functions in
  `.iram.text`;
- the VM hook that feeds audio.

Enabling or disabling a module, builtin or feature requires regenerating
genhdr:

1. Download MicroPython v1.29.0 and copy every file under `micropython/py/`
   that contains `MESHPUNK` over upstream's (they carry the patches).
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
