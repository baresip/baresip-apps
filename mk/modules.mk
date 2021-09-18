#
# modules.mk
#
# Copyright (C) 2021 Christian Spielberger
#
# Switches for build
#
#   USE_AUTOTEST     A simple solution for call loops for automatic testing
#   USE_INTERCOM     A simplified interface for intercom
#   USE_KAOPTIONS    A simple keepalive sender via SIP OPTIONS messages

MOD_ALL := 1

ifneq ($(MOD_ALL),)
USE_AUTOTEST := 1
USE_INTERCOM := 1
USE_KAOPTIONS:= 1
endif

MODULES += auloop
MODULES += vidloop

ifneq ($(USE_AUTOTEST),)
MODULES += autotest
endif
ifneq ($(USE_INTERCOM),)
MODULES += intercom
endif
ifneq ($(USE_KAOPTIONS),)
MODULES += kaoptions
endif
