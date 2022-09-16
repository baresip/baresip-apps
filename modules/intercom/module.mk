#
# module.mk
#
# Copyright (C) 2021 Commend.com - c.spielberger@commend.com
#

MOD		:= intercom
$(MOD)_SRCS	+= intercom.c events.c iccustom.c ichidden.c

include mk/mod.mk
