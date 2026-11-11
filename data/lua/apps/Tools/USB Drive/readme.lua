local caps, dev = ...
local p4 = dev.name == "tdisplay_p4"

local body = p4 and [[
Shares the SD card with a PC. Plug the OTG USB-C port - not the serial one - into the PC, press Start sharing, and the card appears as a removable USB drive over high-speed USB.
]] or [[
Shares the SD card with a PC. Plug the device into the PC, press Start sharing, and the card appears as a removable USB drive - about 1 MB/s, because the chip's USB is full-speed.
]]

body = body .. [[

While sharing, the PC owns the card exclusively: apps lose the SD drive and the mesh radio pauses. Eject the drive on the PC, then press Stop (or just leave the app) - the card remounts and the mesh resumes.

Internal files are not shared. To get one onto the PC, copy it to SD in Tools > Files first, then start sharing.
]]

body = body .. (p4 and [[

USB host mode (Tools > USB Host) and drive mode take turns on the port: stop one before starting the other.]] or [[

After a drive session, USB host mode (Tools > USB Host) needs a reboot before it will work again - the two cannot both own the USB port.]])

return { body = body }
