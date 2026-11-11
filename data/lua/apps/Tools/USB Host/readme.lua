local caps, dev = ...

local body = [[
Runs the device as a USB host so you can plug accessories into it.
]]

if dev.name == "tdisplay_p4" then
    body = body .. [[

POWER
Plug accessories into the USB-A socket. It supplies 5 V to the accessory whenever the device itself is powered through either USB-C port - a charger, a power bank or a PC. On battery alone the socket is unpowered: plug a charger or power bank into either USB-C port and it comes on. Accessories with a USB-A plug go straight in; a USB-C accessory needs a USB-A to USB-C cable or adapter. The USB accessories page in the guide has the details.
]]
elseif dev.usb_power then
    body = body .. [[

POWER
This device powers the accessory from its USB-C port while host mode is on: 5 V turns on when an accessory is plugged in, and off when it is unplugged, when you press Stop, and in standby. A USB-C accessory plugs straight in; one with a USB-A plug needs a plain USB-C OTG adapter.

While host mode is on, a USB-C charger plugged into the port does not charge the device - press Stop to charge. The USB accessories page in the guide has the details.
]]
else
    body = body .. [[

POWER COMES FIRST
The device does not supply power over USB. If nothing happens when you plug something in, that is almost always the reason.

For a keyboard, mouse, gamepad or thumb drive, use a USB-C OTG splitter Y-cable with PD: a USB-C plug into the device, a USB-A socket for the accessory, and a second USB-C socket for a charger or power bank. Plug the charger in as well as the accessory - a plain OTG adapter with no power input does nothing here.

For headphones, use a USB-C to 3.5mm adapter that also has a USB-C charging port. Those bring their own power, so they need no OTG cable.

Any generic adapter of either kind works. The USB accessories page in the guide covers what to look for.
]]
end

if dev.name == "tdisplay_p4" then
    body = body .. [[

One accessory at a time; a USB hub between the device and the accessory is accepted.
]]
else
    body = body .. [[

USB hubs are not supported: the firmware recognises a hub and refuses it, so it is one accessory at a time.
]]
end

body = body .. [[

USING IT
Press Start to begin, and Stop when you are finished. Host mode runs background tasks the whole time it is on, so leaving it off saves memory and battery.

WHAT WORKS
- Audio adapter: routes all device audio out over USB - music, app sounds and game audio alike.
- Gamepad: map it to controls for any game with the Games > Gamepad app.
- Mouse: moves the highlight, click selects.
- Keyboard: types, and its keys work anywhere a built-in keyboard would.
- Thumb drive: browsable as the U: drive in Tools > Files.
]]

if dev.name == "tdisplay_p4" then
    body = body .. [[

The keyboard, audio adapter and thumb drive drivers are built in. The gamepad, mouse and link cable need drivers from the App Library, which has no builds for this board yet.]]
else
    body = body .. [[

Drivers for the gamepad, mouse and link cable download automatically from the App Library the first time they are needed.]]
end

if not caps.keyboard then
    local audio = (dev.audio == "i2s") and "an audio adapter adds headphones"
                                       or  "an audio adapter is how you get music and game sound out"
    body = body .. [[

On this device a USB keyboard also unlocks the keyboard shortcuts listed in the Guide tab, a gamepad is the comfortable way to play the emulators, and ]] .. audio .. [[.]]
end

body = body .. [[

Tools > USB Drive is the opposite arrangement - the device plugs into a PC and the PC supplies the power.]]

if dev.name == "tdisplay_p4" then
    body = body .. [[ Use the OTG USB-C port for it, not the serial one. The two modes take turns on the port: stop one before starting the other.]]
else
    body = body .. [[ Only one of the two can own the USB port, so after a USB Drive session the device needs a reboot before host mode will work again.]]
end

return { body = body }
