# BtPluggy

See the forumn post [here](https://forum.gigatron.io/viewtopic.php?t=637) or [here](https://forum.gigatronttl.eu/viewtopic.php?t=637).


This ESP32-based board plugs into the socket of the 74HC595 on the Gigatron mainboard. It simulates the shift register well enough to make the gigatron happy. One can still attach anything to the gamepad connector and everything work as before. The board also acts as a bluetooth host for keyboards and gamepads, injecting keystrokes and button presses into the Gigatron input port.

The current version is composed of a lower board with two tiny level shifters, topped with an equally sized [ESP32 Adafruit Isty Bitsy](https://learn.adafruit.com/adafruit-itsybitsy-esp32/overview). This is not the most common ESP32 dev board, but it is small and it supports both Bluetooth Classic (BtClassic) and Bluetooth Low Energy (BLE), two rather different protocols in fact. Some people claim that BLE is fast replacing BtClassic. In fact, most Bluetooth keyboards and nearly all gamepads use BtClassic.

Short press the user button (the one close to the power led) and the blue led flashes for two minutes to pair a keyboard or gamepad. All paired devices are remembered in nonvolatile memory. They reconnect automatically the next time. A long press on the user button erases all pairings. Keyboards use the same keymaps as the original Pluggy, with ctrl+alt+Fn to change language. In addition, the caps lock key works! Up to four keyboards or gamepads can be connected at the same time.

Status:

  * Both BtClassic and BLE keyboards seem to be working reliably. My test keyboards are the [Targus AKB862](https://www.amazon.com/dp/B09LFYFN5W) (BtClassic) and the [Logitech K380S](https://www.amazon.com/dp/B0CY2734J2) (the K380S is BLE, the K380 is BtClassic, both look exactly the same.)
  * The picture is less clean for gamepads. For mysteriour reasons, the [8BitDo Micro](https://www.amazon.com/dp/B0CDG5HCCH) does not reconnect reliably unless a keyboard is already connected. The [Aolion N5](https://www.amazon.com/dp/B0FNVZ96T4) suffers from the opposite problem.
