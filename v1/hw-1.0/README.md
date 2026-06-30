
## Lower board version 1.0 and 1.1

Version 1.0 only works when plugged into the Gigatron.
Otherwise the three floating inputs SERCLK and IE cause spurious interrupts that crash the ESP32.

Version 1.1 fixes this by using a 74LVCH244AQ20 as a level converter instead of 74LVC244AQ20.
Both chips are largely compatible, but the LVCH version has a bus-hold circuitry that deals with the floating inputs.

