-include ../version

DISTFILES=divdi3.c hal.c iw_ndis.c iw_ndis.h loader.c loader.h \
		longlong.h Makefile misc_funcs.c ndis.c ndis.h \
		ndiswrapper.h ntoskernel.c ntoskernel.h ntoskernel_io.c \
		pe_linker.c pe_linker.h pnp.c pnp.h proc.c usb.c usb.h \
		winnt_types.h workqueue.c wrapmem.h wrapmem.c wrapper.c \
		wrapndis.h wrapndis.c x86_64_stubs.S

KVERS ?= $(shell uname -r)
KPSUB := $(shell echo $(KVERS) | sed -e 's/\([^\.]*\)\.\([^\.]*\)\..*/\1\2/')

# distros use different paths for kernel include files

KBUILD  ?= /lib/modules/$(KVERS)/build
KSRC ?= $(shell if \
	[ -f /lib/modules/$(KVERS)/source/include/linux/kernel.h ]; then \
		echo /lib/modules/$(KVERS)/source ; \
	else \
		echo $(KBUILD); \
	fi)

DESTDIR =
INST_DIR := $(DESTDIR)/lib/modules/$(KVERS)/misc

SRC_DIR=$(shell pwd)

-include $(KBUILD)/.config

CFLAGS += $(shell [ -f $(KSRC)/include/linux/modversions.h ] && \
		  echo -DEXPORT_SYMTAB -DMODVERSIONS \
		  -include $(KSRC)/include/linux/modversions.h)

ifndef DISABLE_USB
CFLAGS += $(shell if grep -q 'struct module \*owner;' \
		   $(KSRC)/include/linux/usb.h ; then \
			echo -DUSB_DRIVER_OWNER; \
		  fi)
endif

# returns of structs and unions in registers when possible, like Windows 
CFLAGS += -freg-struct-return

# to produce debug trace, add option "DEBUG=<n>" where <n> is 1 to 6
ifdef DEBUG
CFLAGS += -DDEBUG=$(DEBUG) -g
endif

# to debug timers, add option "TIMER_DEBUG=1 DEBUG=<n>"
ifdef TIMER_DEBUG
CFLAGS += -DTIMER_DEBUG
endif

# to debug event layer, add option "EVENT_DEBUG=1 DEBUG=<n>"
ifdef EVENT_DEBUG
CFLAGS += -DEVENT_DEBUG
endif

# to debug USB layer, add option "USB_DEBUG=1 DEBUG=<n>"
ifdef USB_DEBUG
CFLAGS += -DUSB_DEBUG
endif

# to debug I/O layer, add option "USB_DEBUG=1 DEBUG=<n>"
ifdef IO_DEBUG
CFLAGS += -DIO_DEBUG
endif

# to debug worker threads
ifdef WORK_DEBUG
CFLAGS += -DWORK_DEBUG
endif

# if ALLOC_DEGUG=1 is defined, memory leak information is printed
#if ALLOC_DEBUG=2, details about individual allocations leaking is printed
#if ALLOC_DEBUG=3, tags in ExAllocatePoolWithTag leaking printed
ifdef ALLOC_DEBUG
CFLAGS += -DALLOC_DEBUG=$(ALLOC_DEBUG)
endif

CFLAGS += -DDRIVER_VERSION=\"${DRIVER_VERSION}\"

.PHONY: prereq_check gen_exports clean dist_clean \
		 x86_64_stubs install stack_check

all : prereq_check x86_64_stubs gen_exports default stack_check

OBJS := hal.o iw_ndis.o loader.o misc_funcs.o ndis.o \
	    ntoskernel.o ntoskernel_io.o pe_linker.o pnp.o proc.o \
		wrapmem.o wrapndis.o wrapper.o

# by default, USB layer is compiled in if USB support is in kernel;
# to disable USB support in ndiswrapper even if USB support is in kenrel,
# add option "DISABLE_USB=1"

ifdef DISABLE_USB
CFLAGS += -DDISABLE_USB
else
ifeq ($(CONFIG_USB),y)
OBJS += usb.o
endif
ifeq ($(CONFIG_USB),m)
OBJS += usb.o
endif
endif

divdi3.o: divdi3.c longlong.h

hal.o: hal.c ntoskernel.h

iw_ndis..o:	iw_ndis.c iw_ndis.h wrapndis.h

loader.o: loader.c loader.h ndis.h wrapndis.h

misc_funcs.o: misc_funcs.c ndis.h

ndis.o: ndis.c ndis.h iw_ndis.h wrapndis.h

wrapndis.o: wrapndis.c wrapndis.h

ntoskernel.o: ntoskernel.c ndis.h usb.h

ntoskernel_io.o: ntoskernel_io.c ndis.h usb.h

pe_linker.o: pe_linker.c

pnp.o: pnp.c

proc.o: proc.c ndis.h iw_ndis.h wrapndis.h

usb.o: usb.c usb.h ndis.h

wrapper.o: wrapper.c wrapndis.h iw_ndis.h ntoskernel.h loader.h

iw_ndis.h: ndis.h
loader.h: ndiswrapper.h
ndis.h: ntoskernel.h
ntoskernel.h: ndiswrapper.h winnt_types.h pe_linker.h wrapmem.h
usb.h: ntoskernel.h
wrapndis.h: ndis.h

hal_exports.h: hal.c
misc_funcs_exports.h: misc_funcs.c
ndis_exports.h: ndis.c
ntoskernel_exports.h: ntoskernel.c
ntoskernel_io_exports.h: ntoskernel_io.c
ifndef DISABLE_USB
usb_exports.h: usb.c
endif

ifeq ($(CONFIG_X86_64),y)
OBJS += x86_64_stubs.o
x86_64_stubs.o: x86_64_stubs.h x86_64_stubs.S

x86_64_stubs: x86_64_stubs.h

ifdef DISABLE_USB
x86_64_stubs.h: hal.c misc_funcs.c ndis.c ntoskernel.c ntoskernel_io.c \
	pnp.c wrapndis.c
else
x86_64_stubs.h: hal.c misc_funcs.c ndis.c ntoskernel.c ntoskernel_io.c \
	pnp.c usb.c wrapndis.c
endif
	@for file in $^; do \
		echo; \
		echo "# automatically generated from $$file"; \
		sed -n \
			-e 's/.*WIN_FUNC(\([^\,]\+\) *\, *\([0-9]\+\)).*/\
			   win2lin(\1, \2)/p'   \
			-e 's/.*WIN_FUNC_PTR(\([^\,]\+\) *\, *\([0-9]\+\)).*/\
			   win2lin(\1, \2)/p'   \
		   $$file | sed -e 's/[ \t]\+//' | sort -u; \
	done > $@
else
x86_64_stubs:

OBJS += divdi3.o
endif

# generate exports symbol table from C files
%_exports.h: %.c
	@if :; then \
		echo "/* automatically generated from $< */"; \
		echo "#ifdef CONFIG_X86_64"; \
		sed -n \
			-e 's/.*WIN_FUNC(\([^\,]\+\) *\, *\([0-9]\+\)).*/\
			   WIN_FUNC_DECL(\1, \2)/p' \
			-e 's/.*WIN_FUNC_PTR(\([^\,]\+\) *\, *\([0-9]\+\)).*/\
			   WIN_FUNC_DECL(\1, \2)/p' \
		   $< | sed -e 's/[ \t]\+//' | sort -u; \
		echo "#endif"; \
		echo "struct wrap_export $(basename $<)_exports[] = {";\
		sed -n \
			-e 's/.*WIN_FUNC(_win_\([^\,]\+\) *\, *\([0-9]\+\)).*/\
				WIN_WIN_SYMBOL(\1,\2),/p' \
			-e 's/.*WIN_FUNC(\([^\,]\+\) *\, *\([0-9]\+\)).*/\
				WIN_SYMBOL(\1,\2),/p' \
			-e 's/.*WIN_SYMBOL_MAP(\("[^"]\+"\)[ ,\n]\+\([^)]\+\)).*/\
				{\1,(generic_func)\2},/p'  \
			   $< | sed -e 's/[ \t]*/   /' | sort -u; \
		echo "   {NULL, NULL}"; \
		echo "};"; \
	fi > $@

ifdef DISABLE_USB
gen_exports: ndis_exports.h hal_exports.h ntoskernel_exports.h \
			 ntoskernel_io_exports.h misc_funcs_exports.h
else
gen_exports: ndis_exports.h hal_exports.h ntoskernel_exports.h \
			 ntoskernel_io_exports.h misc_funcs_exports.h \
			 usb_exports.h
endif

prereq_check:
	@ if [ ! -f $(KBUILD)/include/linux/version.h ]; then \
		echo "Can't find kernel build files in $(KBUILD);"; \
		echo "  give the path to kernel build directory with " ; \
		echo "  KBUILD=<path> argument to make";\
		exit 1;\
	  fi
	@ if [ ! -f $(KSRC)/include/linux/kernel.h ]; then \
		echo "Can't find kernel source files in $(KSRC);"; \
		echo "  give the path to kernel sources directory with " ; \
		echo "  KSRC=<path> argument to make";\
		exit 1;\
	fi

clean:
	rm -rf $(MODULE) ndiswrapper.o $(OBJS) usb.o x86_64_stubs.o \
	   divdi3.o workqueue.o .*.ko.cmd .*.o.cmd \
	   ndiswrapper.mod.[oc] *~ .tmp_versions

distclean: clean
	rm -f *_exports.h .\#* x86_64_stubs.h

ifeq ($(KPSUB),24)
OBJS += workqueue.o

MODULE := ndiswrapper.o
CFLAGS  += -DLINUX -D__KERNEL__ -DMODULE -I$(KSRC)/include \
		  -Wall -Wstrict-prototypes -fomit-frame-pointer    \
		  -fno-strict-aliasing -pipe -O2

ifneq ($(CONFIG_X86_64),y)
CFLAGS += -mpreferred-stack-boundary=2 
endif
default: $(OBJS)
	$(LD) -r -o $(MODULE) $(OBJS)
else

MODULE := ndiswrapper.ko
obj-m := ndiswrapper.o 

ndiswrapper-objs := $(OBJS)

default:
	$(MAKE) -C $(KBUILD) SUBDIRS=$(SRC_DIR) \
		DRIVER_VERSION=$(DRIVER_VERSION)

endif

stack_check:
	  @ if [ "x$(CONFIG_X86_64)" = "x" -a $(KPSUB) -eq 26 ]; then \
		   if grep -q CONFIG_4KSTACKS $(KBUILD)/.config; then \
			  if grep -q "CONFIG_4KSTACKS=y" $(KBUILD)/.config; then \
				echo; echo; \
			  	echo "*** WARNING: "\
					 "Kernel is compiled with 4K stack size option"\
					 "(CONFIG_4KSTACKS); many Windows drivers will"\
					 "not work with this option enabled. Disable"\
					 "CONFIG_4KSTACKS option, recompile and install"\
					 "kernel";\
				echo; echo; \
			  fi;\
			else \
				echo; echo; \
			    echo "*** WARNING: Kernel seems to have 4K size stack option" \
			     "(CONFIG_4KSTACKS) removed; many Windows drivers will"\
			     "need at least 8K size stacks."\
			     "You should read wiki about 4K size stack issue."\
				 "Don't complain about crashes until you resolve this."; \
				echo; echo; \
			fi;\
		fi

install: prereq_check x86_64_stubs gen_exports default stack_check
	mkdir -p $(INST_DIR)
	install -m 0644 $(MODULE) $(INST_DIR)
	-@/bin/rm -rf $(SRC_DIR)/.tmp_versions
	-/sbin/depmod -a $(KVERS)

dist:
	@for file in $(DISTFILES); do \
	  cp  $$file $(distdir)/$$file; \
	done

