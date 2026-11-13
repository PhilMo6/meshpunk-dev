--[[
  lib/voice_popup — a recording with the open speech engine (lib/dictation;
  Tools > Voice to Text keeps it open while it runs) from another app, in a
  popup over it: Listening, Writing it down, then where the text went, for
  1.5 s.

    voice_popup.mic_sink(d)          -- Voice to Text's d.mic_sink while it runs in
                                     -- the background: the mic key records into the
                                     -- text box selected at the press, or onto the
                                     -- clipboard when none is
    voice_popup.insert(d, ta, opts)  -- a recording into the text box ta, ended by
                                     -- the popup's End (Cancel drops it);
                                     -- opts.inserted() runs after the text went in

  The text goes in at the box's cursor, with a space where it meets a word,
  cut to the box's max_length. A box deleted or full by then gets nothing,
  and the popup says so. Modal overlay pattern (lib/emoji_popup): a
  CLICKABLE full-screen overlay takes every tap, and the box is a nav scope
  over the app's, so keys stay in it.
]]

local lvgl      = require("lvgl")
local nav       = require("lib/nav")
local clipboard = require("lib/clipboard")
local dictation = require("lib/dictation")

local M = {}

local HOLD_TICKS = 15     -- the result stays up 15 ticks of the 100 ms timer
local current = nil       -- the popup up now (one at a time)

local function is_textarea(o) return tostring(o):sub(1, 9) == "textarea(" end

-- luavgl clears an object LVGL deleted, and every call on it raises then.
local function alive(o) return (pcall(tostring, o)) end

-- The text box that has the input focus: the focus group's object when it
-- is one, else, under it (a nav scope's container), the text box LVGL marks
-- FOCUSED. Gridnav marks the child the trackball is on; a tap marks the text
-- box tapped (touch clears gridnav's marks first, nav_disarm_on_touch).
local function selected_textarea()
    local f = lvgl.group.get_default():get_focused()
    if not f then return nil end
    if is_textarea(f) then return f end
    local function find(o, depth)
        for i = 0, o:get_child_cnt() - 1 do
            local c = o:get_child(i)
            if is_textarea(c) then
                if (c:get_state() & lvgl.STATE.FOCUSED) ~= 0 then return c end
            elseif depth < 4 then
                local hit = find(c, depth + 1)
                if hit then return hit end
            end
        end
        return nil
    end
    return find(f, 1)
end

-- Puts s into the text box ta at its cursor; returns the characters put in
-- (0: the box is full).
local function insert_text(ta, s)
    local text = ta.text or ""
    local n = utf8.len(text)
    local pos = ta.cursor_pos
    if pos > n then pos = n end
    local cut = utf8.offset(text, pos + 1)
    local before, after = text:sub(1, cut - 1), text:sub(cut)
    if before ~= "" and not before:find("%s$") then s = " " .. s end
    if after ~= "" and not after:find("^%s") then s = s .. " " end
    local max = ta.max_length
    if max > 0 then
        local room = max - n
        if room <= 0 then return 0 end
        if utf8.len(s) > room then s = s:sub(1, utf8.offset(s, room + 1) - 1) end
        if not s:find("%S") then return 0 end
    end
    ta.text = before .. s .. after
    local added = utf8.len(s)
    ta.cursor_pos = pos + added
    return added
end

local function close(p)
    if current ~= p then return end
    current = nil
    p.timer:delete()
    -- Pop our own scope only: an app that swapped its views meanwhile has
    -- its container focused now, and nav has let ours go already.
    local g = lvgl.group.get_default()
    if g:get_focused() == p.box then
        nav.pop()
        -- The scope beneath resumes on its first visible child; the text box
        -- the text went into gets the focus back.
        if p.target and alive(p.target) and p.target:get_parent() == g:get_focused() then
            nav.set_focused(p.target)
        end
    end
    p.overlay:delete()
end

-- opts: target (a text box; nil = the clipboard), hint (the listening line),
-- buttons (End + Cancel), inserted (runs after the text went into target).
local function open(d, opts)
    local W, H = lvgl.HOR_RES(), lvgl.VER_RES()
    local overlay = lvgl.Object {
        w = W, h = H, x = 0, y = 0,
        bg_color = "#000000", bg_opa = 128, border_width = 0, pad_all = 0,
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)
    overlay:add_flag(lvgl.FLAG.CLICKABLE)      -- modal: every tap stops here
    _obj_move_foreground(overlay)
    local box = overlay:Object {
        w = W - 40, h = lvgl.SIZE_CONTENT, align = lvgl.ALIGN.CENTER,
        bg_color = "#333333", radius = 6,
        border_width = 1, border_color = "#555555",
        pad_all = 8, pad_row = 6, pad_column = 6,
        flex = { flex_direction = "row", flex_wrap = "wrap" },
    }
    box:clear_flag(lvgl.FLAG.SCROLLABLE)
    local p = {
        overlay = overlay, box = box, target = opts.target,
        hint = opts.hint, inserted = opts.inserted, shown = nil,
    }
    box:Label { text = "Voice to Text", w = lvgl.PCT(100) }
    p.msg = box:Label { text = opts.hint, w = lvgl.PCT(100) }
    if opts.buttons then
        p.end_b = box:Button { w = lvgl.PCT(48), h = 30 }
        p.end_b:Label { text = "End", align = lvgl.ALIGN.CENTER }
        p.end_b:onClicked(function() dictation.stop(d) end)
        p.cancel_b = box:Button { w = lvgl.PCT(48), h = 30 }
        p.cancel_b:Label { text = "Cancel", align = lvgl.ALIGN.CENTER }
        p.cancel_b:onClicked(function() dictation.cancel(d) end)
    end
    nav.push(box, { focus = p.end_b })

    local function show(s)
        if s ~= p.shown then
            p.shown = s
            p.msg.text = s
        end
    end

    p.sink = {
        done = function(text, err)
            local say
            if err == "cancelled" then
                say = "Cancelled: nothing was written down."
            elseif err then
                say = "Nothing was written down: " .. err
            elseif text == "" then
                say = "Nothing was heard."
            elseif p.target then
                if not alive(p.target) then
                    say = "The text box is gone: nothing was inserted."
                elseif insert_text(p.target, text) == 0 then
                    say = "The text box is full: nothing was inserted."
                else
                    say = "Inserted into the text box."
                    if p.inserted then p.inserted() end
                end
            else
                clipboard.copy(text)
                say = "Copied to the clipboard."
            end
            p.hold = HOLD_TICKS
            show(say)
            if p.end_b then
                p.end_b:add_flag(lvgl.FLAG.HIDDEN)
                p.cancel_b:add_flag(lvgl.FLAG.HIDDEN)
            end
        end,
    }

    p.timer = lvgl.Timer { period = 100, cb = function()
        if p.hold then
            p.hold = p.hold - 1
            if p.hold <= 0 then close(p) end
            return
        end
        local status = (d.status ~= "") and (" (" .. d.status .. ")") or ""
        if d.state == "working" then
            if p.end_b then p.end_b:add_flag(lvgl.FLAG.HIDDEN) end
            show("Writing it down..." .. status)
        elseif d.state == "listening" then
            show(p.hint .. status)
        end
    end }

    current = p
    return p
end

function M.mic_sink(d)
    if current then return nil end
    local p = open(d, {
        target = selected_textarea(),
        hint = "Listening... let go of the mic key when you are done.",
    })
    return p.sink
end

function M.insert(d, ta, opts)
    if current then return end
    local p = open(d, {
        target = ta, buttons = true, inserted = opts and opts.inserted,
        hint = "Listening... tap End when you are done.",
    })
    local ok, err = dictation.record(d, p.sink)
    if not ok then p.sink.done(nil, err) end
end

return M
