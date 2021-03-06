#Comment out this line to disable debugging
#CFLAGS += -DDEBUG

#CFLAGS += -DTEST_PM

KSRC  := /lib/modules/$(shell uname -r)/build

KVERS := $(shell uname -r |  sed -e 's/\([^\.]*\)\.\([^\.]*\)\..*/\1\2/')

INST_DIR := /lib/modules/$(shell uname -r)/misc

SRC_DIR=$(shell pwd)

.PHONY: prereq_check

.PHONY: clean

.PHONY: install

all : prereq_check default

wrapper.o:	wrapper.c loader.h ndis.h loader.h

loader.o:	loader.c ndis.h

winsyms.o:	winsyms.c ndis.h

misc_funcs.o:	misc_funcs.c ndis.h

ndis.o:	ndis.c ndis.h

prereq_check:
	@ if [ ! -f $(KSRC)/include/linux/version.h ]; then \
		echo "You don't seem to have sources for your kernel; \
		       install them in /usr/src, link $(KSRC) to it and try again."; \
		exit 1;\
	  fi


OBJS   := wrapper.o loader.o winsyms.o ndis.o misc_funcs.o

ifeq ($(KVERS),24)
MODULE := ndiswrapper.o
CFLAGS  += -DLINUX -D__KERNEL__ -DMODULE -I$(KSRC)/include \
  -Wall -Wstrict-prototypes -fomit-frame-pointer    \
  -fno-strict-aliasing -pipe -mpreferred-stack-boundary=2 -O2

default: $(OBJS)
	$(LD) -r -o $(MODULE) $(OBJS)
clean:
	rm -f $(MODULE) $(OBJS)

else
MODULE := ndiswrapper.ko
obj-m := ndiswrapper.o 
ndiswrapper-objs := $(OBJS)

default:
	$(MAKE) -C $(KSRC) SUBDIRS=$(SRC_DIR) modules

clean:
	rm -f $(MODULE) ndiswrapper.o $(OBJS) .ndiswrapper.ko.cmd ndiswrapper.mod.[oc] .*.o.cmd *~

endif

install: default
	mkdir -p $(INST_DIR)
	install -m 0644 $(MODULE) $(INST_DIR)
	depmod -a
