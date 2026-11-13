--[[
  Tools > Voice to Text — speech to text. Hold the mic key (keyboards) or
  the Talk button, speak, let go; the text goes into the box a few seconds
  later, after its text (Append) or in its place (Overwrite). Copy puts the
  box's text on the clipboard.

  The engine is an audio plugin (stt.audioplug.elf, in this app's folder)
  driven by lib/dictation; its model file defaults to model.bin in this
  app's folder, where the App Library puts it (the app installs on the SD
  card: the model does not fit in internal storage), and Model picks another
  (kept in L:/voice_to_text.cfg with the Append / Overwrite choice).

  Home leaves the engine loaded in the background: the mic key then records
  from any app (lib/voice_popup) into the text box selected at the press, or
  onto the clipboard when none is, and Messenger's compose menu has Insert
  voice text. The home screen shows a row whose X unloads it. Quit unloads
  it. The home chord closes the app the same way as Quit.
]]

local app_dir   = assert(..., "Voice to Text: the launcher passed no app folder")
local lvgl      = require("lvgl")
local apps      = require("lib/apps")
local nav       = require("lib/nav")
local theme     = require("lib/theme")
local filepick  = require("lib/filepick")
local clipboard = require("lib/clipboard")
local dictation   = require("lib/dictation")
local voice_popup = require("lib/voice_popup")

local W, H          = lvgl.HOR_RES(), lvgl.VER_RES()
local KEY           = "voice_to_text"           -- background contract key
local CFG           = "L:/voice_to_text.cfg"
local NAME          = "Voice to Text"           -- title, mic session owner, launcher row
local DEFAULT_MODEL = app_dir .. "/model.bin"
local PLUGIN        = app_dir .. "/stt.audioplug.elf"

local root = apps.new_root()
root:set { w = W, h = H, pad_all = 0, border_width = 0, bg_opa = 0 }
root:clear_flag(lvgl.FLAG.SCROLLABLE)
theme.show_background()

-- ── Settings ──────────────────────────────────────────────────────────────────

-- mode: "append" adds each recording after the box's text, "overwrite"
-- puts it in the text's place.
local cfg = { model = DEFAULT_MODEL, mode = "append" }

local function load_cfg()
    local f = io.open(CFG, "r")
    if not f then return end
    local txt = f:read("*a") or ""
    f:close()
    local m = txt:match("model=([^\r\n]+)")
    if m and m ~= "" then cfg.model = m end
    local mode = txt:match("mode=(%a+)")
    if mode == "append" or mode == "overwrite" then cfg.mode = mode end
end

local function save_cfg()
    local f = io.open(CFG, "w")
    if not f then return end
    f:write(string.format("model=%s\nmode=%s\n", cfg.model, cfg.mode))
    f:close()
end

load_cfg()

-- ── The engine (UI-free: also runs with the app in the background) ─────────────

-- st.text = the box's text; st.live = the text of the recording running
-- into it, while it runs.
local bg = apps.background_of(KEY)
local st = (bg and bg.state) or { d = nil, open_err = nil, text = "", live = nil }

-- A recording made here (Talk, or the mic key while this screen is shown)
-- goes into the box; one that heard nothing leaves the box as it was.
local function box_sink()
    st.live = ""
    return {
        piece = function(text) st.live = text end,
        done = function(text)
            st.live = nil
            if text and text ~= "" then
                if cfg.mode == "overwrite" or st.text == "" then
                    st.text = text
                else
                    st.text = st.text .. " " .. text
                end
            end
        end,
    }
end

-- The box's text, with the recording running into it.
local function box_text()
    local live = st.live
    if not live or live == "" then return st.text end
    if cfg.mode == "overwrite" or st.text == "" then return live end
    return st.text .. " " .. live
end

-- Opens the engine, or keeps the one running (in the background). A failed
-- one (a model error, or standby unloaded it) is closed and opened again.
-- This screen is shown when it runs, so the mic key records into the box.
local function engine_open()
    if not (st.d and st.d.state ~= "failed") then
        if st.d then dictation.close(st.d); st.d = nil end
        st.d, st.open_err = dictation.open { plugin = PLUGIN, model = cfg.model, owner = NAME }
    end
    if st.d then st.d.mic_sink = box_sink end
end

local function engine_close()
    if st.d then dictation.close(st.d) end
    st.d = nil
end

local function status_text()
    local d = st.d
    if not d then return "Not started: " .. tostring(st.open_err) end
    if d.state == "loading" then return "Loading the speech engine..." end
    if d.state == "failed" then return "The engine did not start: " .. tostring(d.err) end
    if d.state == "listening" then return "Listening... let go when you are done." end
    if d.state == "working" then
        return "Writing it down" .. (d.status ~= "" and (" (" .. d.status .. ")") or "...")
    end
    local how = d.mic_key and "Hold the mic key or Talk, speak, let go." or "Hold Talk, speak, let go."
    if d.err then return how .. "  (" .. d.err .. ")" end
    return "Ready. " .. how
end

-- Registered on every launch (fresh closures). The manager's tick runs only
-- after Home (go_background): it reads the mic key and the results while
-- another app is in front; the app's own timer does it while it is shown.
apps.register_background {
    key      = KEY,
    app_name = "Voice_to_Text",                 -- the registry key (.version id)
    state    = st,
    period   = 100,
    tick     = function() if st.d then dictation.tick(st.d) end end,
    sounds   = function() return {} end,
    on_close = function() engine_close() end,
    status   = function()
        local d = st.d
        if not d then return NAME end
        if d.state == "listening" then return NAME .. ": listening" end
        if d.state == "working" then return NAME .. ": writing" end
        return NAME .. ": ready"
    end,
}

-- ── Views ─────────────────────────────────────────────────────────────────────

local ui = {}
local shown_text = nil

local function button(parent, text, w, h, fn)
    local b = parent:Button { w = w, h = h }
    local l = b:Label { text = text, align = lvgl.ALIGN.CENTER }
    if fn then b:onClicked(fn) end
    return b, l
end

local function leave_background()
    if st.d and st.d.state ~= "failed" then
        -- From other apps the mic key records into their selected text box.
        st.d.mic_sink = voice_popup.mic_sink
        apps.go_background(KEY)
    else
        engine_close()
        apps.close_background(KEY)
        apps.go_home()
    end
end

local function quit()
    engine_close()
    apps.close_background(KEY)
    apps.go_home()
end

apps.set_on_close(function()
    engine_close()
    apps.close_background(KEY)
    return true
end)

local function choose_model()
    filepick.open(root, {
        mode    = "file",
        title   = "Choose the model file",
        start   = cfg.model:match("^(.*)/[^/]*$") or "S:/",
        exts    = { "bin" },
        on_pick = function(path)
            cfg.model = path
            save_cfg()
            engine_close()
            engine_open()
        end,
    })
end

-- A note (Copy's, or why Talk did not record) stands in the status line
-- for its ticks of the 100 ms timer.
local note = { text = nil, ticks = 0 }
local shown_status = nil

local function refresh()
    if not ui.status then return end
    local s = note.text or status_text()
    if s ~= shown_status then                   -- a scrolling label: set on change only
        shown_status = s
        ui.status.text = s
    end
    local d = st.d
    local text = box_text()
    if text ~= shown_text then
        shown_text = text
        ui.text.text = (text ~= "") and text or "(The text appears here.)"
        ui.box:scroll_to { y = 100000 }         -- bounded to the end: the newest words
    end
    if ui.talk_lbl then
        ui.talk_lbl.text = (d and d.state == "listening") and "Listening..." or "Hold to talk"
    end
end

-- One screen that fits: only the text box scrolls. Every button is a direct
-- child of the nav container (the trackball reaches only those). Rows, top
-- to bottom: title + Home + Quit, the status line (one line; long ones
-- scroll sideways), the text box, Copy / Clear / Append-Overwrite / Model,
-- the talk button.
-- The box gets the height the rest leaves; the rows above it are measured
-- (the font sets the status line's height).
local function build()
    local GAP, TOP_H, TOOLS_H, TALK_H = 4, 24, 28, 44
    local c = root:Object {
        w = W, h = H, x = 0, y = 0, bg_opa = 0, border_width = 0,
        pad_all = 6, pad_row = GAP, pad_column = GAP,
        flex = { flex_direction = "row", flex_wrap = "wrap", align_items = "center" },
    }
    c:clear_flag(lvgl.FLAG.SCROLLABLE)
    nav.replace(c, { flags = nav.ROLLOVER + nav.SCROLL_FIRST })

    c:Label { text = NAME, flex_grow = 1 }
    button(c, "Home", 64, TOP_H, leave_background)
    button(c, "Quit", 64, TOP_H, quit)
    ui.status = c:Label {
        text = status_text(), w = lvgl.PCT(100),
        long_mode = lvgl.LABEL_CONST.LONG_SCROLL_CIRCULAR,
    }

    c:update_layout()
    local above = ui.status:get_coords().y2 - c:get_coords().y1 + 1
    ui.box = c:Object {
        w = lvgl.PCT(100), h = H - above - TOOLS_H - TALK_H - 3 * GAP - 6,
        border_width = 1, pad_all = 4,
    }
    ui.text = ui.box:Label { text = "", w = lvgl.PCT(100) }

    -- Copy, Clear and Model fit their labels; the Append / Overwrite toggle
    -- takes the rest of the row.
    button(c, "Copy", lvgl.SIZE_CONTENT, TOOLS_H, function()
        if st.text ~= "" then
            clipboard.copy(st.text)
            note.text, note.ticks = "Copied. Paste it in a text field.", 20
            refresh()
        end
    end)
    button(c, "Clear", lvgl.SIZE_CONTENT, TOOLS_H, function()
        st.text = ""
        refresh()
    end)
    local mode_b = c:Button { h = TOOLS_H, flex_grow = 1 }
    local mode_l = mode_b:Label { text = "", align = lvgl.ALIGN.CENTER }
    local function show_mode()
        mode_l.text = (cfg.mode == "overwrite") and "Overwrite" or "Append"
    end
    mode_b:onClicked(function()
        cfg.mode = (cfg.mode == "overwrite") and "append" or "overwrite"
        save_cfg()
        show_mode()
    end)
    show_mode()
    button(c, "Model", lvgl.SIZE_CONTENT, TOOLS_H, choose_model)

    -- st.btn_rec: the recording running was started by this button, so its
    -- release ends it (one the mic key started ends with the key).
    local talk
    talk, ui.talk_lbl = button(c, "Hold to talk", lvgl.PCT(100), TALK_H, nil)
    talk:onevent(lvgl.EVENT.PRESSED, function()
        if not st.d then return end
        local ok, err = dictation.record(st.d, box_sink())
        st.btn_rec = ok
        if not ok then
            note.text, note.ticks = "Not recording: " .. tostring(err), 30
            refresh()
        end
    end)
    local function talk_up()
        if st.btn_rec then
            st.btn_rec = false
            dictation.stop(st.d)
        end
    end
    talk:onevent(lvgl.EVENT.RELEASED, talk_up)
    talk:onevent(lvgl.EVENT.PRESS_LOST, talk_up)
    refresh()
    nav.set_focused(talk)
end

engine_open()
build()

apps.add_timer { period = 100, cb = function()
    if st.d then dictation.tick(st.d) end
    if note.text then
        note.ticks = note.ticks - 1
        if note.ticks <= 0 then note.text = nil end
    end
    refresh()
end }

return root
