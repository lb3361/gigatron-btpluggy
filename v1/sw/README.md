
Esptool command to flash:
```
$ esptool --chip esp32 -p /dev/...PORT...  write-flash 0x0 bin/btpluggy-merged.bin
```

Alternatively, compile with ESP-IDF 6.0.

```
$ idf.py set-target esp32
$ idf.py build
$ idf.py flash monitor
```

File [patch/idf-hid-parser.patch](patch/idf6-hid-parser.patch) is an optional patch that makes the esp_hid parser a bit more tolerant to strange gamepads. 

Compilation is still in debug mode. Use `idf.py menuconfig` to change this if you need.

Core 0 runs bluetooth. Core 1 exclusively runs the gigatron interface in order to ensure that the 74hc595 emulation interrupts are not delayed.

Optional: rebuild the merged binary:
```
$ ( cd build; esptool --chip esp32 merge-bin -o ../bin/btpluggy-merged.bin @flash_args )
```
