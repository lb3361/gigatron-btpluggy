

## Precompiled flash

Esptool command to flash:
```
$ esptool --chip esp32 -p /dev/...PORT...  write-flash 0x0 bin/btpluggy-merged.bin
```

## Compilation

Alternatively, compile with ESP-IDF 6.0.

```
$ idf.py set-target esp32
$ idf.py build
$ idf.py flash monitor
```

File [patch/idf-hid-parser.patch](patch/idf6-hid-parser.patch) is an optional 
patch that makes the esp_hid parser a bit more tolerant to strange gamepads. 

Compilation is still in debug mode. Use `idf.py menuconfig` to change this if you need.

Core 0 runs bluetooth. Core 1 exclusively runs the gigatron interface in 
order to ensure that the 74hc595 emulation interrupts are not delayed.

Optional: rebuild the merged binary:
```
$ ( cd build; esptool --chip esp32 merge-bin -o ../bin/btpluggy-merged.bin @flash_args )
```

## Usage

Press the user button (the one close to the red led) to pair keyboard or gamepad.
Long press cancels all parings.

Keyboards work very nicely. Gamepads are hit and miss.


## Loader programs

There is a fat partition in the upper 4MB of the flash memory.
Programs in that partition can be loaded using Ctrl+Fn with n from 2 to 11.
Program `btpluggy.py` can be used to write a fat image to that partition.

Here is a recipe using the `dosfs` programs (https://github.com/lb3361/dosfs)
```
# create an empty image file
dd if=/dev/zero bs=4M count=1 of=fat.img 
# format it with a fat filesystem (-s for no partition table)
dosformat -f fat.img -s
# copy your preferred programs
cat $HOME/gigatron/gigatron-rom/Apps/WozMon/WozMon.gcl \
 | doswrite -f fat.img wozmon.gt1 
cat $HOME/gigatron/gigatron-rom/Contrib/at67/hw/SDCard_NO_ROM/BabelFish/Browser_SDCard_ROMv2_v1.gt1
 | doswrite -f fat.img sdcard.gt1 
# check
dosdir -f fat.img
# write fat image. you need a usb cable between your computer and the device
./btpluggy.py writefat fat.img
```

