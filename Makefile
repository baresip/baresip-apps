#
# Makefile
#
# Copyright (C) 2010 Alfred E. Heggestad
# Copyright (C) 2021 Christian Spielberger
#

# Verbose and silent build modes
ifeq ($(V),)
HIDE=@
endif

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

ifndef BARESIP_PATH
BARESIP_PATH := $(shell [ -f ../baresip/Makefile ] && \
	echo "../baresip")
endif
ifndef BARESIP_MOD_MK
BARESIP_MOD_MK := $(shell [ -f $(BARESIP_PATH)/mk/mod.mk ] && \
	echo "$(BARESIP_PATH)/mk/mod.mk")
endif

CFLAGS    += -I$(LIBRE_INC) -I$(BARESIP_PATH)
CXXFLAGS  += -I$(LIBRE_INC) -I$(BARESIP_PATH)
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
