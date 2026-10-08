local caps = ...

local body = [[
Turns the device into a simple messenger for a few chosen people and group chats.

There are three pages to pick from, switched with the Nodes, Channels and Apps buttons.

Nodes starts with your favorite contacts. To find anyone else, type part of their name in the box and press Search; the list changes to the matches, and an empty search brings the favorites back. Only user contacts are offered; repeaters, rooms and sensors are not part of simple mode.

Channels lists every channel on the device.

Apps lists every installed app. A picked app gets a button on the simple home screen; when it is closed the device returns there.

Tap a row to tick it. Ticks are kept while you search and switch pages.

When the right ones are ticked, press Start Simple Mode. The button is at the top of the list and again at the bottom, and the space key does the same from anywhere on the page except while typing in the search box. Starting saves the ticks; Save on its own keeps them without starting. The home screen becomes a list of just those contacts, channels and apps; tapping a chat opens it, with a box to type a message and send it. Nothing else is on the screen.

The message count in the top bar counts only the picked chats. The top bar is loaded once at start-up, so that part of the switch, in either direction, shows after the next restart.

The Settings button on the simple home screen has the volume bar, a Themes button that opens the normal theme picker, Contacts, Channels and Apps, Nicknames, and Normal Mode, which brings the full launcher back.

Contacts, Channels and Apps opens these same picker pages from inside simple mode, so the list can be changed without going back to the normal launcher. The start button reads Done there: it saves the ticks and returns to the simple home screen.

Nicknames lists the picked contacts and channels. Tap one to type the name simple mode should show for it, such as Mom in place of a call sign. The nickname appears on the home screen and in that chat. When a contact with a nickname posts in a channel, their messages there show the nickname too. It is display only: messages still go to the real contact or channel, and Use real name removes it. Start Simple Mode and Normal Mode both switch at once, no restart needed.

Simple mode keeps its own copy of the chosen contacts. At every start-up it checks the MeshCore contact list, adds back any that were removed, and marks all of them as favorites so the device does not drop them when the contact list fills up. Removing one on purpose means taking it off this list first, in normal mode.

Channels are not added back. If a picked channel is deleted from the device, the simple home screen shows its name with a note instead of a button.

Simple mode needs the MeshCore protocol. If the device starts with a different protocol running, simple mode switches itself off and the normal launcher comes back, with the picks kept for next time.

A firmware update puts the normal launcher back. Open this app again and press Start Simple Mode to return to simple mode; the picks are remembered.]]

return { body = body }
