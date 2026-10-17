# ZMK Status LEDs

## Goal

Enable status signaling via keeboard LEDs, to omit using displays. Perfect for keebs such as corne v4.

## New behaviors

- Displaying BT connection status
  - Highlights possible BT slots and flashes different color on the currently connected one
  - When changing to another BT slot flash fast if it enters pairing behavior (slot unoccuppied)
  - When changing to another BT slot (occuppied) and waiting for connection blink slowly
- Displaying battery status
  - Specify a subarray of LEDs (4 or 5) that will be used to display the battery status by signaling different thresholds
  - Example: using 5 slots for the status and battery being at 80%, four out of five LEDs should be green and the last one red
