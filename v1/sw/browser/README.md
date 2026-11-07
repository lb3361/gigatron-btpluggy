
This directory provides good version of the sdcard browser
and tools to construct a fat image to burn with btpluggy.py

* `Browser_SDCard_ROMv5a_v2.gt1` contains bug fixes and supports long file names up to 22 characters

* `Browser_SDCard_ROMv5a_v1.gt1` matches the original with just the ExecAddr bug corrected.

* `fat.img.gz` is a compressed minimal fat image containing
  `WozMon.gt1` and `SDCard.gt1`, that is the recommanded browser for
  BtPluggy.  Assuming you have a usb cable connected to the BtPluggy,
  burn this image with:
  ```
  % cd ..
  % gunzip -k browser/fat.img.gz
  % ./btpluggy.py writefat browser/fat.img
  ```
  
* `Makefile` contains the recipe to rebuild both browsers and the fat
  image.  To use it, you must create a symlink to `gigatron-rom` in
  this directory and make sure gtbasic is compiled, which is not
  always easy because the code available in `gigatron-rom/Contrib` is
  dated and might need fixes to work with modern compilers. You also
  need either mtools or dosfs to create the image
  ```
  % ln -s path/to/gigatron-rom .
  % (cd gigatron-rom/Contrib/at67; cmake ; make )
  % make
  ```

