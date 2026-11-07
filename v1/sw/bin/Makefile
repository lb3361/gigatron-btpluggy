SHELL=/bin/sh
ESPTOOL=esptool
PORT=PleaseChooseAPort

all: btpluggy-merged.bin.gz btpluggy-with-fat.bin.gz

%.gz : %
	gzip -9fk $<

btpluggy-merged.bin: ../build/btpluggy.bin
	cd ../build && ${ESPTOOL} --chip esp32 merge-bin -o ../bin/btpluggy-merged.bin @flash_args

btpluggy-with-fat.bin: btpluggy-merged.bin ../browser/fat.img.gz
	zcat < ../browser/fat.img.gz > fat.bin
	${ESPTOOL} --chip esp32 merge-bin -o $@ --flash-size 8MB  \
		0x0 btpluggy-merged.bin 0x400000 fat.bin

../build/btpluggy.bin: ../build
	cd .. && idf.py build

../build:
	cd .. && idf.py set-target esp32

clean: FORCE
	-rm *.bin *.hex *.gz

flash: FORCE
	${ESPTOOL} -p ${PORT} write-flash 0x0 btpluggy-merged.bin

FORCE:

.PHONY: FORCE

