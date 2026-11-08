# data_filter.py — one data/ tree, per-board shipping filter.
#
# Every board builds from the same data/ tree, but not every file belongs on
# every board: ELF module artifacts are per CPU architecture — the Xtensa
# boards' "<x>.elf" files and the RISC-V (T-Display P4) "<x>.rv32.elf"
# files sit side by side (docs/DEVICE_EXPANSION_PLAN_2026-09-08.md 3.1).
# ships(rel_path, arch, board) is the single rule set; two consumers apply
# it:
#
#  * make_data_pack.py (the S3 release envs' embedded pack) skips the files
#    the board does not ship;
#  * this file as an extra_script (pre:) for the buildfs/uploadfs targets:
#    it stages the kept files into $BUILD_DIR/data_filtered and points the
#    filesystem-image builder (PROJECT_DATA_DIR) at that copy, so the image
#    on the device carries only the board's files. Firmware builds are not
#    touched.
#
# Arch comes from the board's MCU (board_build / board JSON "build.mcu"):
# esp32s3 -> "xtensa", esp32p4 -> "rv32". Board-specific skips go into
# BOARD_SKIP (board name = platformio.ini `board`), empty today.

import fnmatch
import os
import shutil

# Files that exist for one architecture only. A file matches the first rule
# whose pattern fits (fnmatch on the forward-slash path relative to data/,
# with a leading '/'); files that match no rule ship everywhere.
ARCH_RULES = [
    ("*.rv32.elf",      {"rv32"}),
    ("*.app.elf",       {"xtensa"}),
    ("*.loraproto.elf", {"xtensa"}),
    ("*.bleproto.elf",  {"xtensa"}),
    ("*.drv.elf",       {"xtensa"}),
]

# Per-board skips: board name -> list of patterns (same matching) that board
# never ships. Empty: nothing is board-specific beyond the architecture yet.
BOARD_SKIP = {}

MCU_ARCH = {
    "esp32":   "xtensa",
    "esp32s2": "xtensa",
    "esp32s3": "xtensa",
    "esp32p4": "rv32",
    "esp32c3": "rv32",
    "esp32c6": "rv32",
}

FS_TARGETS = {"buildfs", "uploadfs", "uploadfsota"}


def arch_of(mcu):
    """CPU architecture name for a board JSON "build.mcu" value."""
    return MCU_ARCH.get(str(mcu).lower(), "xtensa")


def ships(rel_path, arch, board=None):
    """True when the board of architecture `arch` carries data/<rel_path>."""
    rel = "/" + rel_path.replace(os.sep, "/").lstrip("/")
    name = rel.rsplit("/", 1)[-1]
    for pattern in BOARD_SKIP.get(board or "", []):
        if fnmatch.fnmatch(rel, pattern) or fnmatch.fnmatch(name, pattern):
            return False
    for pattern, archs in ARCH_RULES:
        if fnmatch.fnmatch(rel, pattern) or fnmatch.fnmatch(name, pattern):
            return arch in archs
    return True


def walk_kept(data_dir, arch, board=None):
    """Yield (rel_path, full_path) for every file the board ships, sorted."""
    for root, dirs, names in os.walk(data_dir):
        dirs.sort()
        for name in sorted(names):
            full = os.path.join(root, name)
            rel = os.path.relpath(full, data_dir).replace(os.sep, "/")
            if ships(rel, arch, board):
                yield rel, full


def stage(data_dir, out_dir, arch, board=None):
    """Rebuild out_dir as the filtered copy of data_dir; returns (kept, skipped)."""
    if os.path.isdir(out_dir):
        shutil.rmtree(out_dir)
    kept = 0
    skipped = []
    for root, dirs, names in os.walk(data_dir):
        dirs.sort()
        for name in sorted(names):
            full = os.path.join(root, name)
            rel = os.path.relpath(full, data_dir).replace(os.sep, "/")
            if not ships(rel, arch, board):
                skipped.append(rel)
                continue
            dst = os.path.join(out_dir, rel.replace("/", os.sep))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(full, dst)
            kept += 1
    return kept, skipped


# ── PlatformIO extra_script entry ───────────────────────────────────────────
# Only when run by SCons (Import exists); plain `import data_filter` from
# make_data_pack.py takes the functions above and nothing else.
try:
    Import("env")  # noqa: F821  (SCons global)
    _is_scons = True
except NameError:
    _is_scons = False

if _is_scons:
    from SCons.Script import COMMAND_LINE_TARGETS

    if FS_TARGETS & set(COMMAND_LINE_TARGETS):
        _mcu = env.BoardConfig().get("build.mcu", "")
        _arch = arch_of(_mcu)
        _board = env.get("BOARD", "")
        _src = env.subst("$PROJECT_DATA_DIR")
        _dst = os.path.join(env.subst("$BUILD_DIR"), "data_filtered")
        if os.path.isdir(_src):
            _kept, _skipped = stage(_src, _dst, _arch, _board)
            env.Replace(PROJECT_DATA_DIR=_dst)
            print("data_filter: %s/%s ships %d files from %s; skipped %d: %s"
                  % (_board, _arch, _kept, _src, len(_skipped),
                     ", ".join(_skipped) if _skipped else "-"))
        else:
            print("data_filter: no data directory at %s" % _src)
