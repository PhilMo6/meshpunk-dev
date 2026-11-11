local caps, dev = ...

local body = [[
MeshPunk is a LoRa mesh communicator with full MeshCore support - plus offline maps, music, games and emulators, themes, a file manager, and an App Library for installing more over WiFi.

Highlights:
- SD card, BLE (phone apps), WiFi
- GPS sets the clock automatically
- Full emoji support, with a downloadable extended set
- Background apps - music keeps playing while you do other things
]]

if dev.name == "tdisplay_p4" then
    body = body .. [[
- USB accessories: keyboard, mouse, gamepad, thumb drive - plugged into the USB-A socket, powered while the device is on USB power (see the USB accessories page)
]]
elseif dev.usb_power then
    body = body .. [[
- USB accessories: keyboard, mouse, gamepad, audio adapter, thumb drive - powered by the device itself (see the USB accessories page)
]]
else
    body = body .. [[
- USB accessories: keyboard, mouse, gamepad, audio adapter, thumb drive (they need external power - see the USB accessories page)
]]
end

-- What this particular device gives you to work with. Everything else in the
-- guide is written for the device it is being read on, so this sets the scene
-- rather than listing alternatives the reader does not have.
if caps.keyboard and caps.trackball then
    body = body .. [[

You have a keyboard, a trackball and a touchscreen, so every part of this guide applies to you.]]
elseif caps.keyboard and caps.touch then
    body = body .. [[

You have a keyboard and a touchscreen. The keyboard types, its arrow keys move the focus and Enter selects; the touchscreen works everywhere too. The on-screen keyboard and the on-screen game pads stay available through the input mode, which the Meta key or the BOOT button cycles - the Touch controls page covers them.]]
elseif caps.touch then
    body = body .. [[

This device is driven by its touchscreen. There is no built-in keyboard, so text is typed on the on-screen keyboard and games use on-screen buttons - the Touch controls page covers both.]]
end

if dev.eth then
    body = body .. [[

With Ethernet switched on (Settings > WiFi) the BOOT button is not an input - its pin carries Ethernet data - so the touchscreen selects and the keyboard's Meta key cycles the input mode.]]
end

if dev.audio == "buzzer" then
    local power = dev.usb_power and "" or ", and like every USB accessory it needs external power"
    body = body .. [[

Sound comes from a buzzer, which plays notification melodies and app tones. For music and game audio you need a USB audio adapter]] .. power .. [[ - see the USB accessories page.]]
end

body = body .. [[

This guide covers day-to-day use. Flip pages with the < and > buttons above.]]

return {
    title   = "Welcome",
    section = "Guide",
    order   = 10,
    body    = body,
}
