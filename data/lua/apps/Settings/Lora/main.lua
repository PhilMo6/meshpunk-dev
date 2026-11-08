-- Settings/Lora — choose which LoRa protocol runs the radio.
-- Protocols are installable packages (MeshCore ships preinstalled; others
-- come from the App Library's LoRa Protocols category) living in
-- L:/meshpunk/lora_protos/<id>/ and loaded at boot; "none" boots with the
-- LoRa chip parked (no protocol at all). The choice is the firmware
-- lora_protocol pref; switching takes effect on reboot. The selection is
-- honored unconditionally every boot; a missing or invalid package boots
-- with the radio off and a bell notice saying so.
local lvgl  = require("lvgl")
local apps  = require("lib/apps")
local nav   = require("lib/nav")
local theme = require("lib/theme")

local root = apps.new_root()
root:set { w = lvgl.HOR_RES(), h = lvgl.VER_RES(), pad_all = 0, border_width = 0, bg_opa = 0 }
root:clear_flag(lvgl.FLAG.SCROLLABLE)

theme.show_background()

local content = root:Object {
    flex = { flex_direction = "row", flex_wrap = "wrap" },
    w = lvgl.HOR_RES(), h = lvgl.VER_RES(),
    border_width = 0, pad_all = 6, bg_opa = 0,
}
nav.replace(content)

content:Label { text = "LoRa Protocol", w = lvgl.PCT(70), h = 26 }
local back_btn = content:Button { w = 50, h = 22 }
back_btn:Label { text = "Home", align = lvgl.ALIGN.CENTER }

local active, requested = _lora_proto()
local status = content:Label { text = "", w = lvgl.PCT(100), h = 16 }

content:Label { text = "Running now: " .. active, w = lvgl.PCT(100), h = 16 }

-- Reboot button, shown only while the saved choice differs from what runs.
local reboot_btn = content:Button { w = lvgl.PCT(60), h = 30 }
reboot_btn:Label { text = "Reboot to apply", align = lvgl.ALIGN.CENTER }
reboot_btn:onClicked(function()
    _system_reboot()
end)
local function refresh_reboot()
    if requested ~= active then
        reboot_btn:clear_flag(lvgl.FLAG.HIDDEN)
    else
        reboot_btn:add_flag(lvgl.FLAG.HIDDEN)
    end
end
refresh_reboot()

content:Label { text = "-- Boot protocol --", w = lvgl.PCT(100), h = 16 }

-- One row per installed protocol package. Selecting persists the choice; the
-- radio switches on the next boot.
local rows = {}
local function refresh_rows()
    for id, lbl in pairs(rows) do
        local tag = ""
        if id == requested then tag = "  [next boot]" end
        if id == active then tag = "  [running]" end
        if id == active and id == requested then tag = "  [running]" end
        local suffix = ""
        if id == "none" then suffix = " (no radio)" end
        lbl.text = id .. suffix .. tag
    end
end

local function add_row(id)
    local btn = content:Button { w = lvgl.PCT(100), h = 30 }
    local lbl = btn:Label { align = lvgl.ALIGN.LEFT_MID }
    rows[id] = lbl
    btn:onClicked(function()
        local stored = _lora_proto_set(id)
        requested = stored
        if stored ~= id then
            status.text = "Invalid id - kept " .. stored
        elseif stored == active then
            status.text = "Already running " .. stored
        else
            status.text = stored .. " will run after reboot"
        end
        refresh_rows()
        refresh_reboot()
    end)
end

for _, id in ipairs(_lora_proto_list() or {}) do
    if id ~= "none" then add_row(id) end
end
-- Always available: boot with the LoRa chip parked. Everything except radio
-- traffic works; protocol apps show their protocol notice.
add_row("none")
refresh_rows()

-- ── Antenna (boards with a switch: _device_caps().antenna_switch) ───────────
-- T-Display P4: the internal antenna or the MMCX connector. Switching to the
-- connector is two-tap arm/confirm (Settings/Power's pattern): a transmit
-- into an empty connector loads the PA with nothing, so an antenna goes on
-- first. Back to internal is one tap. The choice applies at once and
-- persists; _radio_antenna refuses, with a reason, while a send is on the
-- air.
local caps = _device_caps and _device_caps() or {}
if caps.antenna_switch and _radio_antenna then
    content:Label { text = "-- Antenna --", w = lvgl.PCT(100), h = 16 }
    local external = _radio_antenna()
    local ant_btn = content:Button { w = lvgl.PCT(100), h = 30 }
    local ant_lbl = ant_btn:Label { align = lvgl.ALIGN.LEFT_MID }
    local ARM_MS = 8000
    local armed = false
    local arm_timer = nil

    local function ant_text()
        if armed then return "Tap again to switch to the MMCX antenna" end
        if external then return "Antenna: External (MMCX)" end
        return "Antenna: Internal"
    end

    local function disarm()
        armed = false
        if arm_timer then arm_timer:delete(); arm_timer = nil end
        ant_lbl.text = ant_text()
    end

    local function apply(want)
        local ok, now, why = _radio_antenna(want)
        external = now
        if ok then
            status.text = want and "External antenna (MMCX) selected" or "Internal antenna selected"
        else
            status.text = "Not switched: " .. (why or "")
        end
        disarm()
    end

    ant_btn:onClicked(function()
        if external then
            apply(false)
            return
        end
        if not armed then
            armed = true
            ant_lbl.text = ant_text()
            status.text = "Fit an antenna on the MMCX first, then tap again"
            arm_timer = apps.add_timer { period = ARM_MS, cb = function(t)
                t:delete()
                arm_timer = nil
                disarm()
                status.text = ""
            end }
            return
        end
        apply(true)
    end)
    ant_lbl.text = ant_text()
end

content:Label {
    text = "Protocols install from the App Library (LoRa Protocols). Your selection is honored on every boot; a selected protocol that is not installed boots with the radio off. Bluetooth is a separate slot - see Settings > Ble.",
    w = lvgl.PCT(100),
}

back_btn:onClicked(function()
    apps.go_home()
end)

return root
