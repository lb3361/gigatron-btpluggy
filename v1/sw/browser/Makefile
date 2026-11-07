SHELL=/bin/sh
ROM=./gigatron-rom
GTBASIC=${ROM}/Contrib/at67/tools/gtbasic/gtbasic
HAVE_MTOOLS := $(shell which mformat)
HAVE_DOSFS  := $(shell which dosfs)

all: check Browser_SDCard_ROMv5a_v1.gt1 Browser_SDCard_ROMv5a_v2.gt1 fat.img.gz

%.gt1: %.gbas
	$(GTBASIC) $<

%.gz : %
	gzip -9fk $<

ifdef HAVE_MTOOLS

fat.img: Browser_SDCard_ROMv5a_v2.gt1
	dd if=/dev/zero of=fat.img bs=4M count=1
	mformat -i $@
	mcopy -i $@ ${ROM}/Apps/WozMon/WozMon.gt1 ::WozMon.gt1
	mcopy -i $@ $< ::SDCard.gt1
	mdir -i $@

else ifdef HAVE_DOSFS

fat.img: Browser_SDCard_ROMv5a_v2.gt1
	dd if=/dev/zero of=fat.img bs=4M count=1
	yes | dosformat -f $@ -s
	doswrite -f $@ WozMon.gt1 < ${ROM}/Apps/WozMon/WozMon.gt1
	doswrite -f $@ SDCard.gt1 < $<
	dosdir -f $@

else

fat.img: Browser_SDCard_ROMv5a_v2.gt1
	@echo "Warning: not building fat.img without mtools or dosfs"

endif

check: FORCE
	@if ! test -f ${ROM}/ROMv6.rom ; then \
	   echo "Error: please symlink 'gigatron-rom' in this directory"; \
	   false ; fi
	@if ! test -x ${GTBASIC} ; then \
	   echo "Error: please compile gtbasic in 'gigatron-rom/Contrib/at67'";\
	   false ; fi

clean: FORCE
	-rm *.gasm *.gt1 fat.img fat.img.gz

FORCE:

.PHONY: FORCE
