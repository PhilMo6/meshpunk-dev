local caps, dev = ...

local noise = dev.mic
    and "Noise: the microphone listens. Threshold is the peak level (50 to 32767) a sound has to reach; Level test shows the live peak next to it, so you can pick a number above the room's quiet level."
    or  "Noise: this device has no microphone."

return { body = table.concat({
    "Watches the device while you are away and sounds an alarm, messages one contact, or both, when something happens.",
    "Triggers. Each has its own page: whether it is watched, Alarm on or off, Message on or off, and the message text.",
    noise,
    "Movement: after arming, a GPS fix takes the start position; every check after that compares the position with it, and the trigger fires past the distance you set.",
    "Power: not available yet; this firmware cannot tell whether the device is plugged in.",
    "Touch: the first touch on the screen or key or trackball press fires the trigger. With a Grace period set, it starts that many seconds instead: enter the disarm code within them, or the trigger fires.",
    "Messages go as a direct message to the contact chosen under Send to, from the contact list of the radio protocol in use (MeshCore or MT-Lite). Add details puts the noise level, the distance and position, and the time after the text. A trigger fires at most once per Wait between messages.",
    "Arm starts the arming delay, then the watch. The app watches only while its screen is open (the screen may go dark); leaving the app disarms it. If the touch trigger is on and the app is closed with the home chord while armed, the touch message is sent as it closes.",
    "Disarm code: set one under Disarm code. Without a code, Disarm is a plain button anyone can press.",
    "The alarm plays a siren at full volume and restores the volume afterwards. While the microphone listens, other sounds are muted.",
}, "\n\n") }
