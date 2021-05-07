#
# module.mk
#
# Copyright (C) 2021 Commend.com - c.spielberger@commend.com
#

MOD		:= intercom
$(MOD)_SRCS	+= intercom.c incoming.c

include mk/mod.mk
