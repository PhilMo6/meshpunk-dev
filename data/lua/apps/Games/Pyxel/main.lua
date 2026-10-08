local lvgl = require("lvgl")
local apps = require("lib/apps")
local nav = require("lib/nav")
local fileman = require("lib/fileman")
local keybind = require("lib/keybind")
local utils = require("lib/utils")

local app_dir = ...

local W = lvgl.HOR_RES()
local H = lvgl.VER_RES()

-- ============================================================
-- Pyxel game launcher
-- Games: .pyxapp / .zip archives and .py scripts in S:/pyxel, and folders
-- there holding a main.py; the same in the game folder picked in Settings;
-- plus the example games in the app's games/ folder.
-- ============================================================

local GAMES_DIR = "S:/pyxel"
local sd_app_dir = app_dir:gsub("^L:", "S:")

-- Firmware path prefixes -> the VFS mount points the ELF module's fopen uses.
local function to_vfs_path(path)
    if path:sub(1, 2) == "S:" then return "/sd" .. path:sub(3) end
    if path:sub(1, 2) == "L:" then return "/littlefs" .. path:sub(3) end
    return path
end

local function exists(path)
    local f = io.open(path, "r")
    if f then f:close(); return true end
    return false
end

local function find_file(name)
    for _, dir in ipairs({ app_dir, sd_app_dir }) do
        if exists(dir .. "/" .. name) then return dir end
    end
    return nil
end

local ELF_DIR = find_file("pyxel.app.elf") or sd_app_dir
local ELF_PATH = ELF_DIR .. "/pyxel.app.elf"
local PYLIB = ELF_DIR .. "/pylib"
local EXAMPLES_DIR = ELF_DIR .. "/games"

local CFG_PATH = app_dir .. "/pyxel.cfg"

local games = {}      -- { name, path }
local selected = 1
local selected_name = nil
local sound_on = true
local match_on = true
local extra_dir = nil -- game folder picked in Settings, e.g. "S:/games"
local scr = nil

-- ============================================================
-- Controls: T-Deck keys and the touch pad drive a virtual gamepad; the
-- module turns host code 0xC0 + n into GAMEPAD1_BUTTON_A + n. With Match
-- game on, start_game points each control at a key the game reads instead
-- (see match_controls). Keys left unbound reach the game as themselves.
-- ============================================================
local PAD = {
    A = 0xC0, B = 0xC1, X = 0xC2, Y = 0xC3, BACK = 0xC4, START = 0xC6,
    UP = 0xCB, DOWN = 0xCC, LEFT = 0xCD, RIGHT = 0xCE,
}

local ACTIONS = {
    { id="up",    label="Up",     out=PAD.UP,    key1="w",     key2="TrkUp"  },
    { id="down",  label="Down",   out=PAD.DOWN,  key1="s",     key2="TrkDn"  },
    { id="left",  label="Left",   out=PAD.LEFT,  key1="a",     key2="TrkLt"  },
    { id="right", label="Right",  out=PAD.RIGHT, key1="d",     key2="TrkRt"  },
    { id="btn_a", label="A",      out=PAD.A,     key1="m",     key2="TrkClk" },
    { id="btn_b", label="B",      out=PAD.B,     key1="n"                    },
    { id="btn_x", label="X",      out=PAD.X,     key1="k"                    },
    { id="btn_y", label="Y",      out=PAD.Y,     key1="j"                    },
    { id="start", label="Start",  out=PAD.START, key1="Enter"                },
    { id="back",  label="Back",   out=PAD.BACK,  key1="BkSpc"                },
}

local pl_ok, padlayout = pcall(require, "lib/padlayout")
if not pl_ok then padlayout = nil end

local pad = padlayout and padlayout.new{
    app = "Pyxel",
    presets = { {
        name = "Default",
        zones = {
            { id="up",    out=PAD.UP,    label="^",   x=52,  y=118, w=64, h=56 },
            { id="left",  out=PAD.LEFT,  label="<",   x=0,   y=174, w=56, h=66 },
            { id="down",  out=PAD.DOWN,  label="v",   x=56,  y=174, w=60, h=66 },
            { id="right", out=PAD.RIGHT, label=">",   x=116, y=174, w=56, h=66 },
            { id="btn_b", out=PAD.B,     label="B",   x=188, y=174, w=60, h=66 },
            { id="btn_a", out=PAD.A,     label="A",   x=254, y=152, w=66, h=66 },
            { id="start", out=PAD.START, label="STA", x=120, y=0,   w=58, h=30 },
            { id="quit",  out=keybind.QUIT, label="QUIT", x=0, y=0, w=52, h=30 },
        },
    } },
} or nil

-- ============================================================
-- Match game. The launcher reads the game's Python source for the Pyxel
-- names it uses (KEY_* and GAMEPAD1_BUTTON_*). A control keeps its gamepad
-- button when the game reads that button, and otherwise sends a key the game
-- reads; a control the game has no use for keeps its gamepad button.
-- ============================================================

-- Host codes the module turns into Pyxel keys (src/input.c host_to_keys),
-- by Pyxel key name without the KEY_ prefix.
local KEY_CODES = {
    UP = 0x81, DOWN = 0x82, LEFT = 0x83, RIGHT = 0x84,
    SPACE = 0x20, RETURN = 0x0D, BACKSPACE = 0x08, TAB = 0x09, ESCAPE = 0x1B,
    SHIFT = 0x80, LSHIFT = 0x80, ALT = 0x8C, LALT = 0x8C,
}
for c = 0x61, 0x7A do KEY_CODES[string.char(c - 0x20)] = c end   -- A..Z
for c = 0x30, 0x39 do KEY_CODES[string.char(c)] = c end          -- 0..9

-- Touch pad labels for keys; a letter or digit is labelled with itself.
local KEY_LABELS = {
    UP = "^", DOWN = "v", LEFT = "<", RIGHT = ">", SPACE = "SPC", RETURN = "ENT",
    BACKSPACE = "BS", TAB = "TAB", ESCAPE = "ESC", SHIFT = "SHF", LSHIFT = "SHF",
    ALT = "ALT", LALT = "ALT",
}

-- A direction sends its gamepad button, else its arrow key, else its
-- W A S D key.
local MATCH_DIRS = {
    { id = "up",    pad = "UP",    btn = "DPAD_UP",    arrow = "UP",    letter = "W" },
    { id = "down",  pad = "DOWN",  btn = "DPAD_DOWN",  arrow = "DOWN",  letter = "S" },
    { id = "left",  pad = "LEFT",  btn = "DPAD_LEFT",  arrow = "LEFT",  letter = "A" },
    { id = "right", pad = "RIGHT", btn = "DPAD_RIGHT", arrow = "RIGHT", letter = "D" },
}

-- Keys the buttons take first, in this order; after them, any other key the
-- game reads, alphabetically. Q and Escape are never taken (games quit on
-- them) and the arrows are directions only.
local ACTION_KEYS = { "SPACE", "RETURN", "Z", "X", "C", "V", "SHIFT" }
local NOT_ACTION = { Q = true, ESCAPE = true, UP = true, DOWN = true, LEFT = true, RIGHT = true }

-- Gamepad out code -> control id, for rewriting the touch zones.
local PAD_ACTION = {}
for _, a in ipairs(ACTIONS) do PAD_ACTION[a.out] = a.id end

-- Largest .py read out of a .pyxapp (uncompressed bytes).
local PY_MAX = 256 * 1024

local function read_all(path)
    local f = io.open(path, "r")
    if not f then return nil end
    local s = f:read("*a")
    f:close()
    return s
end

-- The .py files of a folder game, and of its subfolders `depth` deep.
local function folder_sources(dir, out, depth)
    for _, e in ipairs(fileman.list(dir, { sizes = false }) or {}) do
        local p = dir .. "/" .. e.name
        if e.type == "file" and e.name:lower():match("%.py$") then
            out[#out + 1] = read_all(p)
        elseif e.type == "dir" and depth > 0 then
            folder_sources(p, out, depth - 1)
        end
    end
end

-- The .py entries of a .pyxapp / .zip: stored, or DEFLATE decompressed with
-- _inflate.
local function zip_sources(path, out)
    local f = io.open(path, "r")
    if not f then return end
    local size = f:seek("end") or 0
    -- The end-of-central-directory record is the last 22 bytes, unless the
    -- archive carries a comment (up to 65535 bytes after it).
    local tail, e = "", nil
    if size >= 22 then
        f:seek("set", size - 22)
        tail = f:read(22) or ""
        if tail:sub(1, 4) == "PK\5\6" then e = 1 end
    end
    if not e and size > 22 then
        local n = math.min(size, 22 + 65535)
        f:seek("set", size - n)
        tail = f:read(n) or ""
        for i = #tail - 21, 1, -1 do
            if tail:sub(i, i + 3) == "PK\5\6" then e = i; break end
        end
    end
    if not e then f:close(); return end
    local count = string.unpack("<I2", tail, e + 10)
    local cd_size, cd_off = string.unpack("<I4I4", tail, e + 12)
    f:seek("set", cd_off)
    local cd = f:read(cd_size) or ""
    local p = 1
    for _ = 1, count do
        if p + 45 > #cd or cd:sub(p, p + 3) ~= "PK\1\2" then break end
        local method = string.unpack("<I2", cd, p + 10)
        local csize, usize, nlen, xlen, clen = string.unpack("<I4I4I2I2I2", cd, p + 20)
        local lho = string.unpack("<I4", cd, p + 42)
        local name = cd:sub(p + 46, p + 45 + nlen)
        p = p + 46 + nlen + xlen + clen
        if name:lower():match("%.py$") and usize > 0 and usize <= PY_MAX
                and (method == 0 or method == 8) then
            f:seek("set", lho)
            local lh = f:read(30) or ""
            if #lh == 30 and lh:sub(1, 4) == "PK\3\4" then
                local ln, lx = string.unpack("<I2I2", lh, 27)
                f:seek("set", lho + 30 + ln + lx)
                local data = f:read(csize)
                if data and method == 8 then data = _inflate(data, usize) end
                out[#out + 1] = data
            end
        end
    end
    f:close()
end

-- The Python source of a game, however it is packaged.
local function game_sources(g)
    local out = {}
    if g.name:sub(-1) == "/" then
        folder_sources(g.path:match("^(.*)/[^/]*$"), out, 3)
    elseif g.path:lower():match("%.py$") then
        out[1] = read_all(g.path)
    else
        zip_sources(g.path, out)
    end
    return out
end

-- A line of Python without its # comment. A # inside a quoted string (with
-- backslash escapes) is kept.
local function uncomment(line)
    if not line:find("#", 1, true) then return line end
    local quote, i = nil, 1
    while i <= #line do
        local c = line:sub(i, i)
        if quote then
            if c == "\\" then i = i + 1
            elseif c == quote then quote = nil end
        elseif c == '"' or c == "'" then
            quote = c
        elseif c == "#" then
            return line:sub(1, i - 1)
        end
        i = i + 1
    end
    return line
end

-- The Pyxel names the source's code uses (comments left out): used["KEY_UP"],
-- and used["BTN_A"] for GAMEPAD1_BUTTON_A.
local function used_names(sources)
    local used = {}
    for _, src in ipairs(sources) do
        for line in src:gmatch("[^\n]+") do
            local code = uncomment(line)
            for k in code:gmatch("%f[%w_]KEY_([%w_]+)") do used["KEY_" .. k] = true end
            for b in code:gmatch("%f[%w_]GAMEPAD1_BUTTON_([%w_]+)") do used["BTN_" .. b] = true end
        end
    end
    return used
end

-- Control id -> out code for the controls matched to the game, plus their
-- touch labels (keys only) and the name each one sends, for the log line.
local function match_controls(used)
    local outs, labels, names, taken = {}, {}, {}, {}
    local function give(id, key)
        local code = KEY_CODES[key]
        outs[id] = code
        labels[id] = KEY_LABELS[key] or key
        names[id] = "KEY_" .. key
        taken[code] = true
    end
    local function give_pad(id, pad_name)
        outs[id] = PAD[pad_name]
        names[id] = "pad"
    end
    for _, d in ipairs(MATCH_DIRS) do
        if used["BTN_" .. d.btn] then
            give_pad(d.id, d.pad)
        elseif used["KEY_" .. d.arrow] then
            give(d.id, d.arrow)
        elseif used["KEY_" .. d.letter] then
            give(d.id, d.letter)
        end
    end
    -- W A S D read together are a direction set, not buttons.
    local wasd = used.KEY_W and used.KEY_A and used.KEY_S and used.KEY_D
    local cands = {}
    for _, key in ipairs(ACTION_KEYS) do
        if used["KEY_" .. key] then cands[#cands + 1] = key end
    end
    local rest = {}
    for name in pairs(used) do
        local key = name:match("^KEY_(.+)$")
        if key and KEY_CODES[key] and not NOT_ACTION[key]
                and not (wasd and (key == "W" or key == "A" or key == "S" or key == "D")) then
            rest[#rest + 1] = key
        end
    end
    table.sort(rest)
    for _, key in ipairs(rest) do cands[#cands + 1] = key end
    local ci = 1
    local function next_key()
        while ci <= #cands do
            local key = cands[ci]
            ci = ci + 1
            if not taken[KEY_CODES[key]] then return key end
        end
        return nil
    end
    local function button(id, btn)
        if used["BTN_" .. btn] then
            give_pad(id, btn)
        else
            local key = next_key()
            if key then give(id, key) end
        end
    end
    button("btn_a", "A")
    -- Start takes Enter before B, X and Y can.
    if used.BTN_START then
        give_pad("start", "START")
    elseif used.KEY_RETURN and not taken[KEY_CODES.RETURN] then
        give("start", "RETURN")
    end
    button("btn_b", "B")
    button("btn_x", "X")
    button("btn_y", "Y")
    if not outs.start then
        local key = next_key()
        if key then give("start", key) end
    end
    if used.BTN_BACK then give_pad("back", "BACK") end
    return outs, labels, names
end

-- Built once the screen helpers exist; the config functions reach it as an upvalue.
local kb

local function load_config()
    kb:reset_defaults()
    local f = io.open(CFG_PATH, "r")
    if not f then return end
    local text = f:read("*a") or ""
    f:close()
    for line in text:gmatch("[^\r\n]+") do
        kb:load_line(line)
        local v = line:match("^sound=(%d)$")
        if v then sound_on = (v == "1") end
        local m = line:match("^match=(%d)$")
        if m then match_on = (m == "1") end
        local g = line:match("^game=(.+)$")
        if g then selected_name = g end
        local d = line:match("^folder=(.+)$")
        if d then extra_dir = d end
    end
end

local function save_config()
    local f = io.open(CFG_PATH, "w")
    if not f then return end
    kb:save_lines(f)
    f:write("sound=" .. (sound_on and "1" or "0") .. "\n")
    f:write("match=" .. (match_on and "1" or "0") .. "\n")
    if extra_dir then f:write("folder=" .. extra_dir .. "\n") end
    if games[selected] then f:write("game=" .. games[selected].name .. "\n") end
    f:close()
end

local root = apps.new_root({
    w = W, h = H,
    bg_color = "#000000", bg_opa = lvgl.OPA(255),
    border_width = 0, pad_all = 0,
})
root:clear_flag(lvgl.FLAG.SCROLLABLE)

-- Adds the games in dir, skipping names already listed: a game in /pyxel on
-- the SD card replaces one of the same name in the Settings game folder, and
-- either replaces the example game of that name.
local function scan_dir(dir, listed)
    local entries = fileman.list(dir, { sizes = false }) or {}
    for _, e in ipairs(entries) do
        local low = e.name:lower()
        local path = fileman.join(dir, e.name)
        local g
        if e.type == "file" and (low:match("%.pyxapp$") or low:match("%.zip$") or low:match("%.py$")) then
            g = { name = e.name, path = path }
        elseif e.type == "dir" and exists(path .. "/main.py") then
            g = { name = e.name .. "/", path = path .. "/main.py" }
        end
        if g and not listed[g.name:lower()] then
            listed[g.name:lower()] = true
            games[#games + 1] = g
        end
    end
end

local function scan_games()
    games = {}
    local listed = {}
    scan_dir(GAMES_DIR, listed)
    if extra_dir then scan_dir(extra_dir, listed) end
    scan_dir(EXAMPLES_DIR, listed)
    table.sort(games, function(a, b) return a.name:lower() < b.name:lower() end)
end

-- Selects the game with this name, or the first game.
local function select_game(name)
    selected = 1
    for i, g in ipairs(games) do
        if g.name == name then selected = i; return end
    end
end

-- Rescans after the game folder changes, keeping the selected game.
local function rescan_games()
    local name = games[selected] and games[selected].name
    scan_games()
    select_game(name)
end

local FONT = lvgl.BUILTIN_FONT.MONTSERRAT_12
local ACCENT = "#FFB347"

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
local create_settings_screen

-- Label + cycling value button, persisted (see lib/keybind).
local setting_row = keybind.rows{ font = FONT, on_save = save_config }

kb = keybind.new{
    actions     = ACTIONS,
    root        = root,
    show_screen = show_screen,
    font        = FONT,
    accent      = ACCENT,
    note        = "Keys here press a virtual gamepad. With Match\n"
               .. "game on, each control sends a key the game\n"
               .. "reads instead. Unbound keys type as themselves.",
    on_back     = function() create_main_screen() end,
    on_save     = function() save_config() end,
    trackball   = { momentum = true, impulse = 15, friction = 82, thresh = 4 },
}

local function start_game(g)
    lvgl.Timer{
        period = 50,
        cb = function(t)
            t:delete()
            -- Match game: controls matched to the names the game's code uses.
            -- A key the game reads is left unbound on the matched controls,
            -- so it reaches the game as itself.
            local outs, labels, km_opts
            if match_on then
                local used = used_names(game_sources(g))
                local names
                outs, labels, names = match_controls(used)
                local skip = {}
                for name in pairs(used) do
                    local code = KEY_CODES[name:match("^KEY_(.+)$") or ""]
                    if code then skip[code] = true end
                end
                km_opts = { outs = outs, skip = skip }
                local parts = {}
                for _, a in ipairs(ACTIONS) do
                    if names[a.id] then parts[#parts + 1] = a.id .. "=" .. names[a.id] end
                end
                print("[pyxel] match game: " .. (#parts > 0 and table.concat(parts, " ") or "no keys found"))
            end
            local km = kb:keymap_string(km_opts)
            -- Deferred launch: the firmware tears Lua down, runs the game,
            -- then recreates Lua and returns to the launcher home.
            local args = { ELF_PATH, to_vfs_path(g.path),
                "-pylib", to_vfs_path(PYLIB),
                "-sound", sound_on and "1" or "0",
                "-stackkb", "32",
                "-trkball", kb:trkball_string() }
            -- Omitted when nothing is bound, so the firmware falls back to
            -- passthrough instead of parsing an empty table.
            if km then
                args[#args + 1] = "-keymap"
                args[#args + 1] = km
                -- Alt+Enter switches the bindings off so the keyboard types
                -- (input(), name entry); the game starts with them on.
                local ok_caps, caps = pcall(_input_caps)
                if ok_caps and type(caps) == "table" and caps.keyboard then
                    args[#args + 1] = "-kbtoggle"
                    args[#args + 1] = "1"
                end
            end
            -- The device clock for the game's time and datetime modules, as
            -- "<epoch seconds>,<UTC offset minutes>" (one argument: the
            -- firmware passes at most 16).
            local ok_tz, tz = pcall(_rtc_tz_offset_minutes)
            args[#args + 1] = "-clock"
            args[#args + 1] = string.format("%d,%d", utils.now(), ok_tz and tz or 0)
            if _elf_touch_layout and pad then
                local tl = pad:zones()
                if tl then
                    if outs then
                        for _, z in ipairs(tl) do
                            local id = PAD_ACTION[z.out]
                            if id and outs[id] then
                                z.out = outs[id]
                                z.label = labels[id] or z.label
                            end
                        end
                    end
                    _elf_touch_layout(tl)
                end
            end
            _launch_elf(table.unpack(args))
        end
    }
end

local function create_help_screen()
    show_screen(function(c)
        c:Label{
            text = "HOW TO PLAY",
            text_font = FONT, text_color = ACCENT,
            w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
        }
        c:Label{
            text = "Seven example games come with the app.\n"
                .. "Add more in /pyxel on the SD card:\n"
                .. ".pyxapp files, or a folder with main.py.\n"
                .. "Settings can add one more folder to\n"
                .. "look for games in.\n\n"
                .. "Default gamepad: W A S D or the trackball\n"
                .. "move, M or trackball click = A, N = B,\n"
                .. "K = X, J = Y, Enter = Start, Backspace =\n"
                .. "Back. Change them in Controls. Other\n"
                .. "keys type straight into the game.\n\n"
                .. "Match game (on by default) reads the\n"
                .. "game's code at launch and points each\n"
                .. "control at a key that game uses.\n\n"
                .. "In games that show a mouse pointer, the\n"
                .. "trackball moves it and its click is the\n"
                .. "left mouse button.\n\n"
                .. "Tap the y key (Quit in Controls) to leave\n"
                .. "a game, or hold ALT + Backspace for 1.5 s.\n\n"
                .. "Settings turns game sound and Match\n"
                .. "game on or off. Games that save keep\n"
                .. "their saves in /pyxel/save on the SD\n"
                .. "card.\n\n"
                .. "When a game asks you to type, ALT + Enter\n"
                .. "switches the keys to typing. Press it\n"
                .. "again to get the gamepad back.",
            text_font = FONT, text_color = "#CCCCCC",
            w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
        }
        local ok = c:Button{ w = lvgl.PCT(60), h = 30 }
        ok:Label{ text = "OK", align = lvgl.ALIGN.CENTER }
        ok:onClicked(function() create_main_screen() end)
    end)
end

local function create_about_screen()
    show_screen(function(c)
        c:Label{
            text = "ABOUT",
            text_font = FONT, text_color = ACCENT,
            w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
        }
        c:Label{
            text = "Plays games made with Pyxel\n"
                .. "(c) 2018-2026 Takashi Kitao\n"
                .. "License: MIT\n"
                .. "github.com/kitao/pyxel\n\n"
                .. "The engine is a C port of Pyxel 2.9.9,\n"
                .. "and games run their own Python code\n"
                .. "on MicroPython. Not affiliated with\n"
                .. "or supported by the Pyxel project.\n\n"
                .. "Python by MicroPython\n"
                .. "(c) 2013-2026 Damien P. George\n"
                .. "  and contributors\n"
                .. "License: MIT\n"
                .. "micropython.org\n\n"
                .. "PNG and ZIP support by LodePNG\n"
                .. "(c) 2005-2020 Lode Vandevenne\n"
                .. "License: zlib\n\n"
                .. "Source and license texts are in the\n"
                .. "app repo this app installs from:\n"
                .. "github.com/PhilMo6/meshpunk-apps\n"
                .. "  module-src/pyxel\n\n"
                .. "Example games from Pyxel 2.9.9, each\n"
                .. "MIT licensed by its author:\n"
                .. "30 Seconds of Daylight - Adam\n"
                .. "Cursed Caverns - Takashi Kitao\n"
                .. "Laser Jetman - Adam\n"
                .. "Mega Wing - Takashi Kitao\n"
                .. "Megaball - Adam\n"
                .. "Space Rescue - Takashi Kitao\n"
                .. "Vortexion - Adam\n"
                .. "Their license is games/LICENSE.txt\n"
                .. "in this app's folder.\n\n"
                .. "Games you add keep their own\n"
                .. "licenses.",
            text_font = FONT, text_color = "#CCCCCC",
            w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
        }
        local ok = c:Button{ w = lvgl.PCT(60), h = 30 }
        ok:Label{ text = "OK", align = lvgl.ALIGN.CENTER }
        ok:onClicked(function() create_main_screen() end)
    end)
end

-- note: shown in red under the game folder row.
create_settings_screen = function(note)
    show_screen(function(c)
        c:Label{
            text = "SETTINGS",
            text_font = FONT, text_color = ACCENT,
            w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
        }

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
                    rescan_games()
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
                rescan_games()
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

        setting_row(c, "Sound",
            function() return sound_on and "< ON >" or "< OFF >" end,
            function() sound_on = not sound_on end
        )

        setting_row(c, "Match game",
            function() return match_on and "< ON >" or "< OFF >" end,
            function() match_on = not match_on end
        )

        c:Label{
            text = "Game folder: its games join the list,\n"
                .. "as well as those in /pyxel on the SD card\n"
                .. "Sound: game sound on or off\n"
                .. "Match game: points each control at a\n"
                .. "key the game reads (see Help)",
            text_font = FONT, text_color = "#666666",
            w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
        }

        local back = c:Button{ w = lvgl.PCT(60), h = 28 }
        back:Label{ text = "Back", text_font = FONT, align = lvgl.ALIGN.CENTER }
        back:onClicked(function() create_main_screen() end)
    end)
end

create_main_screen = function()
    show_screen(function(c)
        c:Label{
            text = "Pyxel",
            text_font = lvgl.BUILTIN_FONT.MONTSERRAT_22,
            text_color = ACCENT,
            w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
        }
        local opts = "No games found"
        if #games > 0 then
            local names = {}
            for i, g in ipairs(games) do names[i] = g.name end
            opts = table.concat(names, "\n")
        end
        local dd = c:Dropdown{ options = opts, w = lvgl.PCT(100), h = 28 }
        if #games > 0 then dd:set{ selected = selected - 1 } end
        dd:onevent(lvgl.EVENT.VALUE_CHANGED, function()
            if #games == 0 then return end
            selected = dd:get("selected") + 1
            save_config()
        end)

        local status = c:Label{
            text = #games > 0 and "Ready to play"
                or "Put games in /pyxel on the SD card, or pick a game folder in Settings",
            text_font = FONT,
            text_color = #games > 0 and "#888888" or "#FF6666",
            w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
        }

        local play = c:Button{ w = lvgl.PCT(48), h = 34 }
        play:Label{ text = "Play", align = lvgl.ALIGN.CENTER }
        play:onClicked(function()
            if #games == 0 then return end
            status:set{ text = "Loading..." }
            start_game(games[selected])
        end)

        local ctrl = c:Button{ w = lvgl.PCT(48), h = 34 }
        ctrl:Label{ text = "Controls", align = lvgl.ALIGN.CENTER }
        ctrl:onClicked(function() kb:open() end)

        local set = c:Button{ w = lvgl.PCT(48), h = 34 }
        set:Label{ text = "Settings", align = lvgl.ALIGN.CENTER }
        set:onClicked(function() create_settings_screen() end)

        if pad then
            local touch = c:Button{ w = lvgl.PCT(48), h = 34 }
            touch:Label{ text = "Touch", align = lvgl.ALIGN.CENTER }
            touch:onClicked(function()
                pad:open{
                    show_screen = show_screen, font = FONT, accent = ACCENT,
                    on_back = function() create_main_screen() end,
                }
            end)
        end

        local help = c:Button{ w = lvgl.PCT(48), h = 34 }
        help:Label{ text = "Help", align = lvgl.ALIGN.CENTER }
        help:onClicked(function() create_help_screen() end)

        local about = c:Button{ w = lvgl.PCT(48), h = 34 }
        about:Label{ text = "About", align = lvgl.ALIGN.CENTER }
        about:onClicked(function() create_about_screen() end)

        local quit = c:Button{ w = lvgl.PCT(48), h = 34 }
        quit:Label{ text = "Quit", align = lvgl.ALIGN.CENTER }
        quit:onClicked(function() apps.go_home() end)
    end)
end

local init_phase = 0
return function()
    init_phase = init_phase + 1
    if init_phase == 1 then
        -- The config first: it names the Settings game folder to scan.
        load_config()
        scan_games()
        select_game(selected_name)
        return false
    end
    create_main_screen()
    return true
end
