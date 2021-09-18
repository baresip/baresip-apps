#
# Makefile
#
# Copyright (C) 2010 Alfred E. Heggestad
# Copyright (C) 2021 Christian Spielberger
#

PROJECT	  := baresip-apps

# Verbose and silent build modes
ifeq ($(V),)
HIDE=@
endif

LIBRE_MIN	:= 2.0.1-dev8
LIBREM_MIN	:= 1.0.0-dev

INSTALL := install
ifeq ($(DESTDIR),)
PREFIX  := /usr/local
else
PREFIX  := /usr
endif

ifndef LIBRE_MK
LIBRE_MK  := $(shell [ -f ../re/mk/re.mk ] && \
	echo "../re/mk/re.mk")

ifeq ($(LIBRE_MK),)
LIBRE_MK  := $(shell [ -f /usr/share/re/re.mk ] && \
	echo "/usr/share/re/re.mk")
endif
ifeq ($(LIBRE_MK),)
LIBRE_MK  := $(shell [ -f /usr/local/share/re/re.mk ] && \
	echo "/usr/local/share/re/re.mk")
endif
endif

include $(LIBRE_MK)
include mk/modules.mk


ifndef LIBREM_PATH
LIBREM_PATH	:= $(shell [ -d ../rem ] && echo "../rem")
endif

ifeq ($(LIBREM_PATH),)
ifneq ($(SYSROOT_LOCAL),)
LIBREM_PATH	:= $(shell [ -f $(SYSROOT_LOCAL)/include/rem/rem.h ] && \
	echo "$(SYSROOT_LOCAL)")
endif
endif

ifeq ($(LIBREM_PATH),)
LIBREM_PATH	:= $(shell [ -f $(SYSROOT)/include/rem/rem.h ] && \
	echo "$(SYSROOT)")
endif

# Include path
LIBREM_INC := $(shell [ -f $(LIBREM_PATH)/include/rem.h ] && \
	echo "$(LIBREM_PATH)/include")
ifeq ($(LIBREM_INC),)
LIBREM_INC := $(shell [ -f $(LIBREM_PATH)/include/rem/rem.h ] && \
	echo "$(LIBREM_PATH)/include/rem")
endif
ifeq ($(LIBREM_INC),)
LIBREM_INC := $(shell [ -f /usr/local/include/rem/rem.h ] && \
	echo "/usr/local/include/rem")
endif
ifeq ($(LIBREM_INC),)
LIBREM_INC := $(shell [ -f /usr/include/rem/rem.h ] && echo "/usr/include/rem")
endif

# Library path
ifeq ($(LIBREM_SO),)
LIBREM_SO  := $(shell [ -f $(LIBREM_PATH)/librem.a ] && \
	echo "$(LIBREM_PATH)")
endif
ifeq ($(LIBREM_SO),)
LIBREM_SO :=$(shell [ -f $(LIBREM_PATH)/librem$(LIB_SUFFIX) ] && \
	echo "$(LIBREM_PATH)")
endif
ifeq ($(LIBREM_SO),)
LIBREM_SO := $(shell [ -f $(LIBREM_PATH)/lib/librem$(LIB_SUFFIX) ] && \
	echo "$(LIBREM_PATH)/lib")
endif
ifeq ($(LIBREM_SO),)
LIBREM_SO  := $(shell [ -f /usr/local/lib/librem$(LIB_SUFFIX) ] \
	&& echo "/usr/local/lib")
endif
ifeq ($(LIBREM_SO),)
LIBREM_SO  := $(shell [ -f /usr/lib/librem$(LIB_SUFFIX) ] && \
	echo "/usr/lib")
endif
ifeq ($(LIBREM_SO),)
LIBREM_SO  := $(shell [ -f /usr/lib64/librem$(LIB_SUFFIX) ] && \
	echo "/usr/lib64")
endif


ifeq ($(LIBREM_PKG_PATH),)
LIBREM_PKG_PATH  := $(shell [ -f ../rem/librem.pc ] && echo "../rem/")
endif

ifeq ($(LIBREM_PKG_PATH),)
LIBREM_PKG_PATH  := $(shell [ -f $(PKG_CONFIG_PATH)/librem.pc ] && \
	echo "$(PKG_CONFIG_PATH)")
endif

ifeq ($(LIBREM_PKG_PATH),)
LIBREM_PKG_PATH  := $(shell [ -f $(LIBREM_SO)/pkgconfig/librem.pc ] && \
	echo "$(LIBREM_SO)/pkgconfig")
endif


ifndef BARESIP_PATH
BARESIP_PATH := $(shell [ -f ../baresip/Makefile ] && \
	echo "../baresip")
endif
ifndef BARESIP_MOD_MK
BARESIP_MOD_MK := $(shell [ -f $(BARESIP_PATH)/mk/mod.mk ] && \
	echo "$(BARESIP_PATH)/mk/mod.mk")
endif

CFLAGS    += -I$(LIBRE_INC) -I$(BARESIP_PATH)/include
CFLAGS    += -I$(LIBREM_INC)


CXXFLAGS  += -I$(LIBRE_INC) -I$(BARESIP_PATH)/include
CPPFLAGS += -DHAVE_INTTYPES_H


MOD_BINS:= $(patsubst %,%$(MOD_SUFFIX),$(MODULES))
MOD_MK	:= $(patsubst %,modules/%/module.mk,$(MODULES))
MOD_BLD	:= $(patsubst %,$(BUILD)/modules/%,$(MODULES))
LIBDIR     := $(PREFIX)/lib
MOD_PATH   := $(LIBDIR)/baresip/modules
CFLAGS    += -DMOD_PATH=\"$(MOD_PATH)\"

all: $(MOD_BINS)

.PHONY: modules
modules:	$(MOD_BINS)

include $(MOD_MK)

$(BUILD): Makefile
	@mkdir -p $(BUILD)
	@touch $@

install: $(MOD_BINS)
	@mkdir -p $(DESTDIR)$(MOD_PATH)
	$(INSTALL) -m 0644 $(MOD_BINS) $(DESTDIR)$(MOD_PATH)

uninstall:
	@rm -rf $(DESTDIR)$(MOD_PATH)

.PHONY: clean
clean:
	@rm -rf $(MOD_BINS) $(BUILD)
	@rm -f *stamp \
	`find . -name "*.[od]"` \
	`find . -name "*~"` \
	`find . -name "\.\#*"`

appsinfo: info
	@echo "  BARESIP_MOD_MK:   $(BARESIP_MOD_MK)"
	@echo "  MODULES:          $(MODULES)"
	@echo "  MOD_MK:           $(MOD_MK)"
	@echo "  MOD_BINS:         $(MOD_BINS)"
	@echo "  autotest_SRCS:    $(autotest_SRCS)"
	@echo "  autotest_OBJS:    $(autotest_OBJS)"
