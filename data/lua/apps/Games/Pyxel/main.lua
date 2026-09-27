local lvgl = require("lvgl")
local apps = require("lib/apps")
local nav = require("lib/nav")
local fileman = require("lib/fileman")
local keybind = require("lib/keybind")

local app_dir = ...

local W = lvgl.HOR_RES()
local H = lvgl.VER_RES()

-- ============================================================
-- Pyxel game launcher
-- Games: .pyxapp / .zip archives and .py scripts in S:/pyxel, and folders
-- there holding a main.py.
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

local CFG_PATH = app_dir .. "/pyxel.cfg"

local games = {}      -- { name, path }
local selected = 1
local selected_name = nil
local sound_on = true
local scr = nil

-- ============================================================
-- Controls: T-Deck keys drive a virtual gamepad. Pyxel games read
-- GAMEPAD1 d-pad/A/B/Start; the module turns host code 0xC0 + n into
-- GAMEPAD1_BUTTON_A + n. Keys left unbound reach the game as themselves.
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
    { id="btn_a", label="A",      out=PAD.A,     key1="Space", key2="TrkClk" },
    { id="btn_b", label="B",      out=PAD.B,     key1="x"                    },
    { id="btn_x", label="X",      out=PAD.X,     key1="c"                    },
    { id="btn_y", label="Y",      out=PAD.Y,     key1="v"                    },
    { id="start", label="Start",  out=PAD.START, key1="p"                    },
    { id="back",  label="Back",   out=PAD.BACK                               },
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
        local g = line:match("^game=(.+)$")
        if g then selected_name = g end
    end
end

local function save_config()
    local f = io.open(CFG_PATH, "w")
    if not f then return end
    kb:save_lines(f)
    f:write("sound=" .. (sound_on and "1" or "0") .. "\n")
    if games[selected] then f:write("game=" .. games[selected].name .. "\n") end
    f:close()
end

local root = apps.new_root({
    w = W, h = H,
    bg_color = "#000000", bg_opa = lvgl.OPA(255),
    border_width = 0, pad_all = 0,
})
root:clear_flag(lvgl.FLAG.SCROLLABLE)

local function scan_games()
    local entries = fileman.list(GAMES_DIR, { sizes = false }) or {}
    for _, e in ipairs(entries) do
        local low = e.name:lower()
        local path = GAMES_DIR .. "/" .. e.name
        if e.type == "file" and (low:match("%.pyxapp$") or low:match("%.zip$") or low:match("%.py$")) then
            games[#games + 1] = { name = e.name, path = path }
        elseif e.type == "dir" and exists(path .. "/main.py") then
            games[#games + 1] = { name = e.name .. "/", path = path .. "/main.py" }
        end
    end
    table.sort(games, function(a, b) return a.name:lower() < b.name:lower() end)
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

kb = keybind.new{
    actions     = ACTIONS,
    root        = root,
    show_screen = show_screen,
    font        = FONT,
    accent      = ACCENT,
    note        = "Keys here press a virtual gamepad, which every\n"
               .. "Pyxel game reads. Unbound keys type as themselves.",
    on_back     = function() create_main_screen() end,
    on_save     = function() save_config() end,
    trackball   = { momentum = true, impulse = 15, friction = 82, thresh = 4 },
}

local function start_game(g)
    local km = kb:keymap_string()
    lvgl.Timer{
        period = 50,
        cb = function(t)
            t:delete()
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
            end
            if _elf_touch_layout and pad then
                local tl = pad:zones()
                if tl then _elf_touch_layout(tl) end
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
            text = "Put Pyxel games in /pyxel on the SD card:\n"
                .. ".pyxapp files, or a folder with main.py.\n\n"
                .. "Default gamepad: W A S D or the trackball\n"
                .. "move, Space or trackball click = A, X = B,\n"
                .. "C = X, V = Y, P = Start. Change them in\n"
                .. "Controls. Other keys, Enter included,\n"
                .. "type straight into the game.\n\n"
                .. "In games that show a mouse pointer, the\n"
                .. "trackball moves it and its click is the\n"
                .. "left mouse button.\n\n"
                .. "Tap the y key (Quit in Controls) to leave\n"
                .. "a game, or hold ALT + Backspace for 1.5 s.\n\n"
                .. "The Sound button turns game sound on\n"
                .. "or off. Games that save keep their\n"
                .. "saves in /pyxel/save on the SD card.",
            text_font = FONT, text_color = "#CCCCCC",
            w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
        }
        local ok = c:Button{ w = lvgl.PCT(60), h = 30 }
        ok:Label{ text = "OK", align = lvgl.ALIGN.CENTER }
        ok:onClicked(function() create_main_screen() end)
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
            text = #games > 0 and "Ready to play" or "Put games in /pyxel on the SD card",
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

        local snd = c:Button{ w = lvgl.PCT(48), h = 34 }
        local snd_label = snd:Label{
            text = sound_on and "Sound: On" or "Sound: Off",
            align = lvgl.ALIGN.CENTER,
        }
        snd:onClicked(function()
            sound_on = not sound_on
            snd_label:set{ text = sound_on and "Sound: On" or "Sound: Off" }
            save_config()
        end)

        local ctrl = c:Button{ w = lvgl.PCT(48), h = 34 }
        ctrl:Label{ text = "Controls", align = lvgl.ALIGN.CENTER }
        ctrl:onClicked(function() kb:open() end)

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

        local quit = c:Button{ w = lvgl.PCT(48), h = 34 }
        quit:Label{ text = "Quit", align = lvgl.ALIGN.CENTER }
        quit:onClicked(function() apps.go_home() end)
    end)
end

local init_phase = 0
return function()
    init_phase = init_phase + 1
    if init_phase == 1 then
        scan_games()
        load_config()
        if selected_name then
            for i, g in ipairs(games) do
                if g.name == selected_name then selected = i; break end
            end
        end
        return false
    end
    create_main_screen()
    return true
end
