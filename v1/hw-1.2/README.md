
## Lower board version 1.2

This version of the lower board incorporates two changes:

* It includes three resistors to pull down the potentially floating inputs.  It may be possible to replace the 74LVC244iAQQ20 chips by 74LVCH244AQ20 and omit the resitors, but more testing is needed.
  
* Components have been moved to preserve a copper free zone under the ESP32 antenna.  This is not critical because there is space between the lower board and the ESP board, but any RF improvement is welcome.
  

