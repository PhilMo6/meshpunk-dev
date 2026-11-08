local lvgl = require("lvgl")
local apps = require("lib/apps")
local nav = require("lib/nav")
local fileman = require("lib/fileman")
local keybind = require("lib/keybind")

local app_dir = ...

local W = lvgl.HOR_RES()
local H = lvgl.VER_RES()

-- ============================================================
-- SNES (snes9x) Launcher
-- ============================================================

local TITLE     = "SNES"
local ACCENT    = "#7F00FF"
local ELF_NAME  = "snes.app.elf"
local ROM_HINT  = "Place .smc/.sfc ROMs in S:/snes/ or\npick a folder in Settings"
local EXTRA_DIR = "S:/snes"
local function is_rom(name)
    return name:match("%.smc$") or name:match("%.sfc$") or name:match("%.fig$")
end

-- Derive SD-side mirror of app directory:
-- "L:/lua/apps/Games/SNES" -> "S:/lua/apps/Games/SNES"
local sd_app_dir = app_dir:gsub("^L:", "S:")

-- ROM paths are converted for the ELF module's fopen; the ELF path itself is
-- passed raw — the loader reads L:/S: prefixes natively.
local function to_vfs_path(path)
    if path:sub(1, 2) == "S:" then return "/sd" .. path:sub(3) end
    if path:sub(1, 2) == "L:" then return "/littlefs" .. path:sub(3) end
    return path
end

-- Firmware with _elf_arch_path maps an ELF name to this CPU's build
-- (snes.app.rv32.elf on the T-Display P4): that file is checked for, and the
-- name given is returned (the loader maps it again at launch).
local function find_file(name)
    local have = (_elf_arch_path and _elf_arch_path(name)) or name
    local search = { app_dir, sd_app_dir }
    for _, dir in ipairs(search) do
        local f = io.open(dir .. "/" .. have, "r")
        if f then f:close(); return dir .. "/" .. name end
    end
    return nil
end

local CFG_PATH = app_dir .. "/launcher.cfg"
local ELF_PATH = find_file(ELF_NAME)

-- ============================================================
-- Coprocessor pre-flight: carts that need a chip the emulator does not
-- implement. Reads the first 66KB and checks every header candidate
-- (LoROM/HiROM, each with and without a 512-byte copier header), keeping
-- only candidates whose checksum + complement pair validates, so garbage
-- can't false-positive. Returns the chip's name, or nil to launch.
-- memmap.c in the module refuses the same set (serial-only backstop that
-- also catches interleaved dumps). Remove a chip here AND there when its
-- emulation lands.
-- ============================================================
local function unsupported_chip(path)
    local f = io.open(path, "rb")
    if not f then return nil end
    local data = f:read(66048)   -- covers the far candidate: 0xFFB0+0x200+0x30
    f:close()
    if not data then return nil end
    -- base = file offset of the $xFB0 header block; string indices are 1-based
    local function chip_at(base, hirom)
        if base + 0x30 > #data then return nil end
        local comp = data:byte(base + 0x2D) + data:byte(base + 0x2E) * 256
        local sum  = data:byte(base + 0x2F) + data:byte(base + 0x30) * 256
        if comp + sum ~= 0xFFFF then return nil end   -- not a real header
        local speed = data:byte(base + 0x26)          -- $xFD5 map mode
        local typ   = data:byte(base + 0x27)          -- $xFD6 chip type
        local hi = math.floor(typ / 16)
        if hirom then
            if speed % 16 == 10 and hi == 15 then return "SPC7110" end
        else
            -- SA-1 is emulated (the module carries the second 65816), so
            -- hi == 3 passes through here.
            if hi == 4 then return "S-DD1" end
            if typ == 0xF6 then return "SETA ST010/011" end
            if typ == 0xF5 and speed % 16 ~= 10 then return "ST018" end
        end
        return nil
    end
    local candidates = {
        { 0x7FB0, false }, { 0x7FB0 + 0x200, false },
        { 0xFFB0, true },  { 0xFFB0 + 0x200, true },
    }
    for _, c in ipairs(candidates) do
        local chip = chip_at(c[1], c[2])
        if chip then return chip end
    end
    return nil
end

-- ============================================================
-- Keymap / controls system (host-side translation, same as GameBoy/NGPC:
-- the -keymap string maps physical keys to the codes the module reads)
-- ============================================================

-- Codes main_tdeck.c's poll_input() understands.
local SNES = {
    UP     = 0x77,  -- 'w'
    DOWN   = 0x73,  -- 's'
    LEFT   = 0x61,  -- 'a'
    RIGHT  = 0x64,  -- 'd'
    A      = 0x6D,  -- 'm'
    B      = 0x6E,  -- 'n'
    X      = 0x6B,  -- 'k'
    Y      = 0x6A,  -- 'j'
    L      = 0x71,  -- 'q'
    R      = 0x70,  -- 'p'
    START  = 0x0D,  -- Enter
    SELECT = 0x20,  -- Space
}

-- What this launcher wants bound. lib/keybind owns the key table, the Controls
-- and picker screens, storage and the -keymap/-trkball strings, and appends the
-- standard Quit action itself. Defaults name keys rather than repeating codes.
local ACTIONS = {
    { id="up",     label="Up",     out=SNES.UP,     key1="w",     key2="TrkUp"  },
    { id="down",   label="Down",   out=SNES.DOWN,   key1="s",     key2="TrkDn"  },
    { id="left",   label="Left",   out=SNES.LEFT,   key1="a",     key2="TrkLt"  },
    { id="right",  label="Right",  out=SNES.RIGHT,  key1="d",     key2="TrkRt"  },
    { id="btn_a",  label="A btn",  out=SNES.A,      key1="m",     key2="TrkClk" },
    { id="btn_b",  label="B btn",  out=SNES.B,      key1="n"                    },
    { id="btn_x",  label="X btn",  out=SNES.X,      key1="k"                    },
    { id="btn_y",  label="Y btn",  out=SNES.Y,      key1="j"                    },
    { id="btn_l",  label="L btn",  out=SNES.L,      key1="q"                    },
    { id="btn_r",  label="R btn",  out=SNES.R,      key1="p"                    },
    { id="start",  label="Start",  out=SNES.START,  key1="Enter"                },
    { id="select", label="Select", out=SNES.SELECT, key1="BkSpc"                },
}

-- Touch controller layout (keyboardless boards, and any board once the user
-- turns touch input on). The four face buttons keep the console's diamond
-- (Y left, X top, B bottom, A right) and the shoulders share the top strip
-- with Start/Select. lib/padlayout owns the user's edits — drag, resize,
-- per-pad off — persisted per app, so this is only the default. The lib
-- ships with firmware newer than this launcher's min_fw, so it may be absent.
local pl_ok, padlayout = pcall(require, "lib/padlayout")
if not pl_ok then padlayout = nil end

local pad = padlayout and padlayout.new{
    app = "Snes",
    presets = { {
        name = "Default",
        zones = {
            { id="up",     out=SNES.UP,     label="^",    x=52,  y=118, w=64, h=56 },
            { id="left",   out=SNES.LEFT,   label="<",    x=0,   y=174, w=56, h=66 },
            { id="down",   out=SNES.DOWN,   label="v",    x=56,  y=174, w=60, h=66 },
            { id="right",  out=SNES.RIGHT,  label=">",    x=116, y=174, w=56, h=66 },
            { id="btn_y",  out=SNES.Y,      label="Y",    x=176, y=182, w=46, h=46 },
            { id="btn_x",  out=SNES.X,      label="X",    x=228, y=136, w=46, h=46 },
            { id="btn_b",  out=SNES.B,      label="B",    x=228, y=190, w=46, h=46 },
            { id="btn_a",  out=SNES.A,      label="A",    x=276, y=164, w=44, h=46 },
            { id="btn_l",  out=SNES.L,      label="L",    x=54,  y=0,   w=50, h=30 },
            { id="btn_r",  out=SNES.R,      label="R",    x=106, y=0,   w=50, h=30 },
            { id="select", out=SNES.SELECT, label="SEL",  x=158, y=0,   w=54, h=30 },
            { id="start",  out=SNES.START,  label="STRT", x=214, y=0,   w=56, h=30 },
            { id="quit",   out=keybind.QUIT, label="QUIT", x=0, y=0,    w=52, h=30 },
        },
    } },
} or nil

-- Built once the screen helpers below exist; save_config/load_config reach it
-- as an upvalue.
local kb

-- Renderer: 0 = Speed (Core-1 worker), 1 = Accuracy (Core 0).
-- The worker binds one frame-boundary snapshot of the palette and BG base
-- registers, so games that rewrite those mid-frame (HDMA colour gradients,
-- mid-frame base switches) render them wrong. Core 0 re-reads them per span.
local renderer = 0

local found_roms = {}   -- { {name, path}, ... }
local seen_lower = {}
local selected_rom = 1
local selected_rom_name = nil
local scr = nil

-- Stable, manager-registered root (same pattern as the GameBoy launcher).
local root = apps.new_root({
    w = W, h = H,
    bg_color = "#000000", bg_opa = lvgl.OPA(255),
    border_width = 0, pad_all = 0,
})
root:clear_flag(lvgl.FLAG.SCROLLABLE)

-- fileman routes the drive from the L:/S: prefix itself; sizes=false skips
-- the per-entry size lookup, so huge ROM folders list fast (watchdog-safe).
local function scan_dir_for_roms(dir_path)
    local entries = fileman.list(dir_path, {
        sizes = false,
        filter = function(e)
            return e.type == "file" and is_rom(e.name:lower())
        end,
    }) or {}
    for _, e in ipairs(entries) do
        local low = e.name:lower()
        if not seen_lower[low] then
            seen_lower[low] = true
            found_roms[#found_roms + 1] = {
                name = e.name,
                path = fileman.join(dir_path, e.name),
            }
        end
    end
end

-- Game folder picked in Settings (saved as folder= in the config), scanned
-- after the built-in folders.
local extra_dir = nil

-- Reads only the folder= line: the folder must be known before the scan
-- phases run, while load_config needs the scan results for the ROM selection.
local function load_folder()
    local f = io.open(CFG_PATH, "r")
    if not f then return end
    local text = f:read("*a") or ""
    f:close()
    for line in text:gmatch("[^\r\n]+") do
        local d = line:match("^folder=(.+)$")
        if d then extra_dir = d end
    end
end

-- Rescans every folder after the game folder changes, keeping the selected
-- ROM by name.
local function rescan_roms()
    local keep = found_roms[selected_rom] and found_roms[selected_rom].name
    found_roms = {}
    seen_lower = {}
    scan_dir_for_roms(app_dir)
    scan_dir_for_roms(sd_app_dir)
    if EXTRA_DIR ~= app_dir and EXTRA_DIR ~= sd_app_dir then
        scan_dir_for_roms(EXTRA_DIR)
    end
    if extra_dir then scan_dir_for_roms(extra_dir) end
    table.sort(found_roms, function(a, b)
        return a.name:lower() < b.name:lower()
    end)
    selected_rom = 1
    for i, r in ipairs(found_roms) do
        if r.name == keep then selected_rom = i; break end
    end
end

-- Remember key bindings, trackball tuning, the renderer, the game folder and
-- the last played ROM.
local function save_config()
    local f = io.open(CFG_PATH, "w")
    if not f then return end
    kb:save_lines(f)
    f:write(string.format("renderer=%d\n", renderer))
    if extra_dir then f:write("folder=" .. extra_dir .. "\n") end
    if #found_roms > 0 then
        f:write("rom=" .. found_roms[selected_rom].name .. "\n")
    end
    f:close()
end

local function load_config()
    kb:reset_defaults()
    local f = io.open(CFG_PATH, "r")
    if not f then return end
    local text = f:read("*a")
    f:close()
    if not text then return end
    for line in text:gmatch("[^\r\n]+") do
        -- Binding and trk_* lines are the library's; the patterns below are
        -- disjoint from them, so an unconsumed line just falls through.
        kb:load_line(line)
        local rmode = line:match("^renderer=([01])$")
        if rmode then renderer = tonumber(rmode) end
        local rname = line:match("^rom=(.+)$")
        if rname then selected_rom_name = rname end
        local d = line:match("^folder=(.+)$")
        if d then extra_dir = d end
    end
end

-- ============================================================
-- Screen management — single navigable scope per view, focusables as direct
-- children; the new view goes to nav.replace BEFORE the old one is deleted
-- (GameBoy/App Library swap_view pattern).
-- ============================================================
local FONT = lvgl.BUILTIN_FONT.MONTSERRAT_12

local function show_screen(builder)
    local old = scr
    scr = root:Object({
        w = W, h = H, x = 0, y = 0,
        bg_color = "#000000", bg_opa = lvgl.OPA(255),
        border_width = 0, pad_all = 8,
        flex = {
            flex_direction = "row", flex_wrap = "wrap",
            justify_content = "center", row_gap = 6, column_gap = 6,
        },
    })
    builder(scr)
    nav.replace(scr, { flags = nav.ROLLOVER + nav.SCROLL_FIRST })
    if old then apps.delete_view(old) end
end

local create_main_screen
local create_help_screen
local create_about_screen
local create_radio_screen
local create_chip_screen
local create_settings_screen

-- ============================================================
-- Radios vs the Speed renderer
-- ============================================================
-- The Core-1 render worker's band context is 13,076 bytes and lives on that
-- task's stack, so Speed needs a 16KB (or at least 14KB) contiguous block of
-- internal SRAM. BLE and WiFi hold enough of that pool that the worker cannot
-- spawn; the module then renders on Core 0, which runs but is what Accuracy
-- already does. This prompt exists so that picking Speed actually gets it.
-- The second argument to the _*_set_enabled bindings applies the change
-- without writing prefs, so the next boot restores whatever the user had set;
-- they are NOT switched back on when the module exits.
local function radios_on()
    return _ble_get_enabled(), _wifi_get_enabled()
end

local function radios_off()
    if _ble_get_enabled() then _ble_set_enabled(false, false) end
    if _wifi_get_enabled() then _wifi_set_enabled(false, false) end
end

-- Deferred launch: the firmware tears Lua down, runs the module, then
-- recreates Lua and returns to the launcher. The 50ms timer lets the caller's
-- status text paint before Lua goes away.
local function launch_now()
    local km = kb:keymap_string()
    local ts = kb:trkball_string()
    local rom = found_roms[selected_rom]
    lvgl.Timer{
        period = 50,
        cb = function(t)
            t:delete()
            -- -stackkb 24 is a ceiling: the firmware caps the module task
            -- there instead of taking a 32KB rung, which leaves internal
            -- SRAM for the render worker.
            local args = { ELF_PATH, to_vfs_path(rom.path),
                "-stackkb", "24",
                "-render", tostring(renderer),
                "-trkball", ts }
            -- Omitted when nothing is bound, so the firmware falls back
            -- to passthrough instead of parsing an empty table.
            if km then
                args[#args + 1] = "-keymap"
                args[#args + 1] = km
            end
            if _elf_touch_layout and pad then
                local tl = pad:zones()
                if tl then _elf_touch_layout(tl) end
            end
            _launch_elf(table.unpack(args))
        end
    }
end

-- Controls, the key picker and the trackball Input screen all live in
-- lib/keybind. It renders through this app's show_screen, so the view stack,
-- nav flags and theme are unchanged; only the duplicated code is gone.
kb = keybind.new{
    actions     = ACTIONS,
    root        = root,
    show_screen = show_screen,
    font        = FONT,
    accent      = ACCENT,
    on_back     = function() create_main_screen() end,
    on_save     = save_config,
    trackball   = { momentum = true, impulse = 15, friction = 82, thresh = 4 },
}

-- ============================================================
-- Main screen
-- ============================================================
create_main_screen = function()
    show_screen(function(c)
        c:Label{
            text = TITLE,
            text_font = lvgl.BUILTIN_FONT.MONTSERRAT_22,
            text_color = ACCENT,
            w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
        }

        -- ROM selector — dropdown over all found ROMs
        local rom_opts = "No ROMs found"
        if #found_roms > 0 then
            local names = {}
            for i, r in ipairs(found_roms) do names[i] = r.name end
            rom_opts = table.concat(names, "\n")
        end

        local romDd = c:Dropdown{
            options = rom_opts,
            w = lvgl.PCT(100), h = 30,
        }
        if #found_roms > 0 then
            romDd:set{ selected = selected_rom - 1 }
        end
        romDd:onevent(lvgl.EVENT.VALUE_CHANGED, function()
            if #found_roms == 0 then return end
            selected_rom = romDd:get("selected") + 1
            save_config()
        end)

        local has_elf = ELF_PATH ~= nil
        local has_roms = #found_roms > 0
        local status = c:Label{
            text = (not has_elf) and (ELF_NAME .. " not found!")
                or (has_roms and "Ready to play" or ROM_HINT),
            text_font = FONT,
            text_color = (has_elf and has_roms) and "#888888" or "#FF6666",
            w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
        }

        local launchBtn = c:Button{ w = lvgl.PCT(48), h = 34 }
        launchBtn:Label{ text = "Play", align = lvgl.ALIGN.CENTER }
        launchBtn:onClicked(function()
            if not ELF_PATH then
                status:set{ text = ELF_NAME .. " not found!" }
                return
            end
            if #found_roms == 0 then
                status:set{ text = "No ROMs found!" }
                return
            end
            -- Before the radio prompt: no point freeing memory for a cart
            -- that cannot boot.
            local chip = unsupported_chip(found_roms[selected_rom].path)
            if chip then
                create_chip_screen(chip)
                return
            end
            -- Only Speed needs the radios down, and only when one is up.
            local ble, wifi = radios_on()
            if renderer == 0 and (ble or wifi) then
                create_radio_screen()
                return
            end
            status:set{ text = "Loading..." }
            launch_now()
        end)

        local ctrlBtn = c:Button{ w = lvgl.PCT(48), h = 34 }
        ctrlBtn:Label{ text = "Controls", align = lvgl.ALIGN.CENTER }
        ctrlBtn:onClicked(function() kb:open() end)

        if pad then
            local touchBtn = c:Button{ w = lvgl.PCT(48), h = 34 }
            touchBtn:Label{ text = "Touch", align = lvgl.ALIGN.CENTER }
            touchBtn:onClicked(function()
                pad:open{
                    show_screen = show_screen, font = FONT, accent = ACCENT,
                    on_back = function() create_main_screen() end,
                }
            end)
        end

        local setBtn = c:Button{ w = lvgl.PCT(48), h = 34 }
        setBtn:Label{ text = "Settings", align = lvgl.ALIGN.CENTER }
        setBtn:onClicked(function() create_settings_screen() end)

        local helpBtn = c:Button{ w = lvgl.PCT(48), h = 34 }
        helpBtn:Label{ text = "Quit help", align = lvgl.ALIGN.CENTER }
        helpBtn:onClicked(function() create_help_screen() end)

        local aboutBtn = c:Button{ w = lvgl.PCT(48), h = 30 }
        aboutBtn:Label{ text = "About", align = lvgl.ALIGN.CENTER }
        aboutBtn:onClicked(function() create_about_screen() end)

        local quitBtn = c:Button{ w = lvgl.PCT(48), h = 30 }
        quitBtn:Label{ text = "Quit", align = lvgl.ALIGN.CENTER }
        quitBtn:onClicked(function()
            apps.go_home()   -- manager tears down the stable root
        end)
    end)
end

-- ============================================================
-- Settings screen — the game folder (lib/filepick) and the renderer.
-- `note` shows in red under the folder row.
-- ============================================================
local setting_row = keybind.rows{ font = FONT, on_save = save_config }

local function folder_rows(c, note)
    c:Label{
        text = "Game folder",
        text_font = FONT, text_color = "#CCCCCC",
        w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
    }
    local shown = extra_dir or "None"
    if #shown > 26 then shown = "..." .. shown:sub(-23) end
    local pick = c:Button{ w = extra_dir and lvgl.PCT(70) or lvgl.PCT(100), h = 28 }
    pick:Label{ text = shown, text_font = FONT, align = lvgl.ALIGN.CENTER }
    pick:onClicked(function()
        require("lib/filepick").open(root, {
            mode = "dir",
            title = "Choose a game folder",
            start = extra_dir or "S:/",
            on_pick = function(path)
                -- The module opens files through /sd and /littlefs only.
                if path:sub(1, 2) == "U:" then
                    create_settings_screen("Games can't run from a USB drive:\n"
                        .. "pick a folder on the SD card or in\n"
                        .. "internal storage.")
                    return
                end
                extra_dir = path
                rescan_roms()
                save_config()
                create_settings_screen()
            end,
        })
    end)
    if extra_dir then
        local clear = c:Button{ w = lvgl.PCT(26), h = 28 }
        clear:Label{ text = "Clear", text_font = FONT, align = lvgl.ALIGN.CENTER }
        clear:onClicked(function()
            extra_dir = nil
            rescan_roms()
            save_config()
            create_settings_screen()
        end)
    end
    if note then
        c:Label{
            text = note,
            text_font = FONT, text_color = "#FF6666",
            w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
        }
    end
end

create_settings_screen = function(note)
    show_screen(function(c)
        c:Label{
            text = "SETTINGS",
            text_font = FONT, text_color = ACCENT,
            w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
        }
        folder_rows(c, note)

        setting_row(c, "Renderer",
            function() return (renderer == 0) and "< Speed >" or "< Accuracy >" end,
            function() renderer = (renderer == 0) and 1 or 0 end
        )

        c:Label{
            text = "Game folder: its ROMs join the list,\n"
                 .. "as well as those in " .. EXTRA_DIR .. " and\n"
                 .. "the app folder\n"
                 .. "Renderer: Speed is much faster; Accuracy\n"
                 .. "redraws the slower way some games need\n"
                 .. "if the picture looks wrong",
            text_font = FONT, text_color = "#666666",
            w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
        }

        local backBtn = c:Button{ w = lvgl.PCT(60), h = 28 }
        backBtn:Label{ text = "Back", text_font = FONT, align = lvgl.ALIGN.CENTER }
        backBtn:onClicked(function() create_main_screen() end)
    end)
end

-- ============================================================
-- Quit help screen (firmware-wide Alt+Backspace exit chord)
-- ============================================================
create_help_screen = function()
    show_screen(function(c)
        c:Label{
            text = "QUIT TO LAUNCHER",
            text_font = FONT,
            text_color = ACCENT,
            w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
        }

        c:Label{
            text = "While the game is running, hold\n"
                 .. "ALT + Backspace for about 1.5 seconds\n"
                 .. "to quit back to the launcher.\n\n"
                 .. "Or tap the Quit key - Y by default,\n"
                 .. "rebindable under Controls. That is the\n"
                 .. "only exit on a device in legacy\n"
                 .. "keyboard mode, where holds and key\n"
                 .. "combos do not register.",
            text_font = FONT,
            text_color = "#CCCCCC",
            w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
        }

        local okBtn = c:Button{ w = lvgl.PCT(60), h = 30 }
        okBtn:Label{ text = "OK", align = lvgl.ALIGN.CENTER }
        okBtn:onClicked(function() create_main_screen() end)
    end)
end

-- ============================================================
-- Unsupported-coprocessor message. Reached from Play when the selected
-- ROM's header names a chip the emulator does not implement.
-- ============================================================
create_chip_screen = function(chip)
    show_screen(function(c)
        c:Label{
            text = "CHIP NOT SUPPORTED",
            text_font = FONT,
            text_color = ACCENT,
            w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
        }

        c:Label{
            text = "This game needs the\n"
                 .. chip .. " coprocessor,\n"
                 .. "which this emulator does not\n"
                 .. "have yet. The game cannot run\n"
                 .. "on this device.",
            text_font = FONT,
            text_color = "#CCCCCC",
            w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
        }

        local okBtn = c:Button{ w = lvgl.PCT(60), h = 30 }
        okBtn:Label{ text = "OK", align = lvgl.ALIGN.CENTER }
        okBtn:onClicked(function() create_main_screen() end)
    end)
end

-- ============================================================
-- Speed-renderer memory prompt. Reached from Play only when the Speed
-- renderer is selected and at least one radio is up.
-- ============================================================
create_radio_screen = function()
    show_screen(function(c)
        c:Label{
            text = "FREE MEMORY?",
            text_font = FONT,
            text_color = ACCENT,
            w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
        }

        local msg = c:Label{
            text = "The Speed renderer needs memory\n"
                 .. "that Bluetooth and WiFi are\n"
                 .. "holding.\n\n"
                 .. "Continue turns BOTH of them off\n"
                 .. "before the game starts. They stay\n"
                 .. "off after you quit - turn them\n"
                 .. "back on in Settings > Wireless,\n"
                 .. "or restart the device.\n\n"
                 .. "The Accuracy renderer runs\n"
                 .. "without shutting anything down.",
            text_font = FONT,
            text_color = "#CCCCCC",
            w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
        }

        local yesBtn = c:Button{ w = lvgl.PCT(48), h = 32 }
        yesBtn:Label{ text = "Continue", align = lvgl.ALIGN.CENTER }
        yesBtn:onClicked(function()
            msg:set{ text = "Turning Bluetooth and WiFi off..." }
            -- Torn down one paint later so that text is on screen for it;
            -- launch_now() then takes its own 50ms before Lua goes away.
            lvgl.Timer{
                period = 50,
                cb = function(t)
                    t:delete()
                    radios_off()
                    launch_now()
                end
            }
        end)

        local noBtn = c:Button{ w = lvgl.PCT(48), h = 32 }
        noBtn:Label{ text = "Cancel", align = lvgl.ALIGN.CENTER }
        noBtn:onClicked(function() create_main_screen() end)
    end)
end

-- ============================================================
-- About screen — emulator license and credits.
-- The scope container scrolls (nav.SCROLL_FIRST), so the text runs
-- past the panel height without extra machinery.
-- ============================================================
create_about_screen = function()
    show_screen(function(c)
        c:Label{
            text = "ABOUT",
            text_font = FONT,
            text_color = ACCENT,
            w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
        }

        c:Label{
            text = "SNES emulation by Snes9x, via the\n"
                 .. "retro-go pure-C port, based on\n"
                 .. "libretro snes9x2010.\n\n"
                 .. "LICENSE - PLEASE NOTE:\n"
                 .. "Snes9x is NOT free software. It may\n"
                 .. "be used and distributed for\n"
                 .. "NON-COMMERCIAL, personal use only.\n"
                 .. "Commercial use requires permission\n"
                 .. "from the copyright holders.\n\n"
                 .. "Also includes:\n"
                 .. "ndssfc (GPL v2) (c) 2010 dking,\n"
                 .. "  BassAceGold, ShadauxCat, Nebuleon\n"
                 .. "ZSNES code (GPL v2)\n"
                 .. "  (c) 1997-2001 ZSNES Team\n\n"
                 .. "Snes9x (c) Gary Henderson,\n"
                 .. "Jerremy Koot, John Weidman,\n"
                 .. "Brad Jorsch, Nach, zones, BearOso,\n"
                 .. "OV2, byuu, neviksti, Shay Green\n"
                 .. "and many others - the full list is\n"
                 .. "in the source tree's LICENSE.\n\n"
                 .. "Super Nintendo is a trademark of\n"
                 .. "Nintendo. No game ROMs are included\n"
                 .. "- supply your own.",
            text_font = FONT,
            text_color = "#CCCCCC",
            w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
        }

        local okBtn = c:Button{ w = lvgl.PCT(60), h = 30 }
        okBtn:Label{ text = "OK", align = lvgl.ALIGN.CENTER }
        okBtn:onClicked(function() create_main_screen() end)
    end)
end

-- ============================================================
-- Startup: phased directory scanning, then UI
-- ============================================================
local init_phase = 0
return function()
    init_phase = init_phase + 1

    if init_phase == 1 then
        scan_dir_for_roms(app_dir)
        return false
    elseif init_phase == 2 then
        scan_dir_for_roms(sd_app_dir)
        return false
    elseif init_phase == 3 then
        if EXTRA_DIR ~= app_dir and EXTRA_DIR ~= sd_app_dir then
            scan_dir_for_roms(EXTRA_DIR)
        end
        load_folder()
        if extra_dir then scan_dir_for_roms(extra_dir) end
        return false
    end

    table.sort(found_roms, function(a, b)
        return a.name:lower() < b.name:lower()
    end)
    load_config()
    if selected_rom_name then
        for i, r in ipairs(found_roms) do
            if r.name == selected_rom_name then selected_rom = i; break end
        end
    end
    create_main_screen()
    return true
end
