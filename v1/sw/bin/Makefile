SHELL=/bin/sh
ESPTOOL=esptool
PORT=PleaseChooseAPort

btpluggy-merged.bin: ../build/btpluggy.bin
	cd ../build && ${ESPTOOL} --chip esp32 merge-bin -o ../bin/btpluggy-merged.bin @flash_args

../build/btpluggy.bin: ../build
	cd .. && idf.py build

../build:
	cd .. && idf.py set-target esp32

clean: FORCE
	-rm btpluggy-merged.bin

flash: FORCE
	${ESPTOOL} -p ${PORT} write-flash 0x0 btpluggy-merged.bin

FORCE:

.PHONY: FORCE

