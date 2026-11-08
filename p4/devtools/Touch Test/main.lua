local lvgl = require("lvgl")
local apps = require("lib/apps")

-- ============================================================
-- Touch Test — show where the firmware thinks you pressed
-- ============================================================
-- The whole screen is one press area. Each press leaves three markers:
--
--   blue   follows the contact for as long as it is down
--   red    the touchdown coordinate — the one LVGL uses to decide which
--          widget a tap hits
--   green  where the contact was when it lifted
--
-- Red and green land on top of each other on a stationary tap; a gap
-- between them is the contact moving, or the reading moving under a still
-- finger. The distance from a marker to your fingertip is the error.
--
-- Everything is reported on screen. This copy (the firmware data tree) also
-- prints every touchdown, movement and lift as "[TOUCH TEST]" lines on the
-- serial log, with the mapped point and the panel's raw numbers; the release
-- copy in meshpunk-apps prints nothing.

local W = lvgl.HOR_RES()
local H = lvgl.VER_RES()

print(string.format("[TOUCH TEST] screen %dx%d; lines: down/move/up  ui x,y  raw x,y", W, H))

local scr = apps.new_root({
    w = W, h = H, x = 0, y = 0,
    bg_color = "#000000", bg_opa = lvgl.OPA(255),
    border_width = 0, pad_all = 0,
})
scr:clear_flag(lvgl.FLAG.SCROLLABLE)

-- Everything drawn here is decoration. LVGL objects are clickable by
-- default, so any of it left clickable would SWALLOW the press it sits
-- under — including a press aimed at a spot where a marker already parked.
local function deco(o)
    o:clear_flag(lvgl.FLAG.CLICKABLE)
    o:clear_flag(lvgl.FLAG.SCROLLABLE)
    return o
end

-- ── Markers ─────────────────────────────────────────────────────────────────
-- Blue is created last so it draws on top of the two parked markers.
local dot_press = deco(scr:Object{
    x = -10, y = -10, w = 7, h = 7,
    bg_color = "#FF3030", bg_opa = 255,
    radius = 4, border_width = 0, pad_all = 0,
})
local dot_lift = deco(scr:Object{
    x = -10, y = -10, w = 7, h = 7,
    bg_color = "#33CC44", bg_opa = 255,
    radius = 4, border_width = 0, pad_all = 0,
})
local dot_live = deco(scr:Object{
    x = -20, y = -20, w = 9, h = 9,
    bg_color = "#3399FF", bg_opa = 255,
    radius = 5, border_width = 0, pad_all = 0,
})

-- ── Readout ─────────────────────────────────────────────────────────────────
local line_press = deco(scr:Label{
    text = "press --",
    text_font = lvgl.BUILTIN_FONT.MONTSERRAT_12,
    text_color = "#FF6060", x = 4, y = 4,
})
local line_lift = deco(scr:Label{
    text = "lift  --",
    text_font = lvgl.BUILTIN_FONT.MONTSERRAT_12,
    text_color = "#33CC44", x = 4, y = 19,
})
local line_span = deco(scr:Label{
    text = "",
    text_font = lvgl.BUILTIN_FONT.MONTSERRAT_12,
    text_color = "#888888", x = 4, y = 34,
})
deco(scr:Label{
    text = "blue=live  red=touchdown  green=lift",
    text_font = lvgl.BUILTIN_FONT.MONTSERRAT_12,
    text_color = "#555555", x = 4, y = H - 15,
})

-- ── Press tracking ──────────────────────────────────────────────────────────
-- Ships with the firmware that provides _touch_raw, but degrade rather than
-- hard-error if the app is ever run against a build without it.
local EMPTY_RAW = { x0 = -1, y0 = -1, x1 = -1, y1 = -1, points = 0, drops = 0 }
local raw_fn = _touch_raw or function() return EMPTY_RAW end

local cur = nil

local function sample()
    local indev = lvgl.indev.get_act()
    if not indev then return nil end
    local x, y = indev:get_point()
    return x, y, raw_fn()
end

-- Second raw slot, when the frame carried one.
local function slot1(r)
    if r.x1 and r.x1 >= 0 then
        return string.format("  slot1 %d,%d", r.x1, r.y1)
    end
    return ""
end

local function begin_press()
    local x, y, r = sample()
    if not x then return end
    dot_live:set{ x = x - 4, y = y - 4 }
    cur = {
        px = x, py = y, prx = r.x0, pry = r.y0,   -- touchdown, mapped + raw
        lx = x, ly = y, lrx = r.x0, lry = r.y0,   -- latest sample
        minx = x, maxx = x, miny = y, maxy = y,   -- excursion during the press
        n = 1, jump = 0, maxpts = r.points,
    }
    line_press:set{ text = string.format("press %d,%d  raw %d,%d",
                                         x, y, r.x0, r.y0) }
    line_lift:set{ text = "lift  --" }
    line_span:set{ text = "" }
    print(string.format("[TOUCH TEST] down  ui %d,%d  raw %d,%d  pts %d%s",
                        x, y, r.x0, r.y0, r.points, slot1(r)))
end

local function continue_press()
    if not cur then return end
    local x, y, r = sample()
    if not x then return end
    dot_live:set{ x = x - 4, y = y - 4 }
    local dx, dy = x - cur.lx, y - cur.ly
    local step = math.floor(math.sqrt(dx * dx + dy * dy) + 0.5)
    if step > cur.jump then cur.jump = step end
    -- One line per change of the mapped point or of the raw reading, so a
    -- still finger with a stable reading prints nothing.
    if x ~= cur.lx or y ~= cur.ly or r.x0 ~= cur.lrx or r.y0 ~= cur.lry then
        print(string.format("[TOUCH TEST] move  ui %d,%d  raw %d,%d  pts %d%s",
                            x, y, r.x0, r.y0, r.points, slot1(r)))
    end
    cur.lx, cur.ly, cur.lrx, cur.lry = x, y, r.x0, r.y0
    if x < cur.minx then cur.minx = x end
    if x > cur.maxx then cur.maxx = x end
    if y < cur.miny then cur.miny = y end
    if y > cur.maxy then cur.maxy = y end
    if r.points > cur.maxpts then cur.maxpts = r.points end
    cur.n = cur.n + 1
end

local function end_press()
    if not cur then return end
    local r = raw_fn()
    dot_live:set{ x = -20, y = -20 }
    dot_press:set{ x = cur.px - 3, y = cur.py - 3 }
    dot_lift:set{ x = cur.lx - 3, y = cur.ly - 3 }
    line_lift:set{ text = string.format("lift  %d,%d  raw %d,%d",
                                        cur.lx, cur.ly, cur.lrx, cur.lry) }
    line_span:set{ text = string.format("span x%d..%d y%d..%d  jump %d  n %d  pts %d",
                                        cur.minx, cur.maxx, cur.miny, cur.maxy,
                                        cur.jump, cur.n, cur.maxpts) }
    print(string.format("[TOUCH TEST] up    ui %d,%d  raw %d,%d  span x%d..%d y%d..%d  jump %d  n %d  pts %d  drops %d",
                        cur.lx, cur.ly, cur.lrx, cur.lry,
                        cur.minx, cur.maxx, cur.miny, cur.maxy,
                        cur.jump, cur.n, cur.maxpts, r.drops))
    cur = nil
end

scr:add_flag(lvgl.FLAG.CLICKABLE)
scr:onevent(lvgl.EVENT.PRESSED,     function() begin_press() end)
scr:onevent(lvgl.EVENT.PRESSING,    function() continue_press() end)
scr:onevent(lvgl.EVENT.RELEASED,    function() end_press() end)
scr:onevent(lvgl.EVENT.PRESS_LOST,  function() end_press() end)

-- Focus sink. LVGL turns ENTER on the focused object into a synthetic
-- LV_EVENT_PRESSED/RELEASED pair (lv_indev.c), so with the root focused a
-- button press would fake a touch and park markers where the keypad indev's
-- stale point sits. This 1px invisible object holds focus instead: it has no
-- press handlers, and it is non-clickable so a finger can never hit it.
local keyholder = deco(scr:Object{
    x = 0, y = 0, w = 1, h = 1, bg_opa = 0, border_width = 0, pad_all = 0,
})
lvgl.group.get_default():add_obj(keyholder)
lvgl.group.focus_obj(keyholder)

return scr
