#
# modules.mk
#
# Copyright (C) 2021 Christian Spielberger
#
# Switches for build
#
#   USE_AUTOTEST     A simple solution for call loops for automatic testing
#   USE_INTERCOM     A simplified interface for intercom

MOD_ALL := 1

ifneq ($(MOD_ALL),)
USE_AUTOTEST := 1
USE_INTERCOM := 1
endif

ifneq ($(USE_AUTOTEST),)
MODULES += autotest
endif
ifneq ($(USE_INTERCOM),)
MODULES += intercom
endif
