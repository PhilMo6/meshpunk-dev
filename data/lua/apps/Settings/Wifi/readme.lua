local caps, dev = ...

local body = [[
WiFi settings for this device.

- The ON/OFF button powers the WiFi radio. While off, nothing scans or connects and the radio draws nothing.
- The line under the button shows what is happening now: the network and IP address when connected, "Connecting...", or "Not connected".
- Saved networks are remembered across reboots. Tap the list to step through it, pick one, then Connect to join it or Forget to delete it.
- Scan Networks lists what is in range, strongest signal first; a * marks a network that needs a password. Choosing an open network joins immediately. Choosing a secured one shows the password box - type it and press Join. Networks you join this way are saved automatically.
- The device joins saved networks on its own in bounded rounds and parks the radio when none are reachable, instead of scanning forever.
]]

if dev.eth then
    body = body .. [[

ETHERNET
The Ethernet button powers the wired port. Off is the default. On, the port takes a cable whenever one is plugged in, gets an address from the network and carries the device's traffic ahead of WiFi; the WiFi line then shows "Ethernet" with the address. WiFi keeps working for when there is no cable.

While Ethernet is on, the BOOT button is not an input: its pin carries Ethernet data. The touchscreen selects and the keyboard's Meta key cycles the input mode. Standby wake and power-off wake still work, because Ethernet stops first.
]]
end

body = body .. [[

BLE settings live in Settings > BLE.]]

return { body = body }
