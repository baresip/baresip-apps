/**
 * @file intercom.h Intercom module internal interface
 *
 * Copyright (C) 2021 Commend.com - c.spielberger@commend.com
 */


void event_handler(enum ua_event ev, struct bevent *event, void *arg);

int mem_deref_later(void *arg);
struct iccustom *iccustom_find(const struct pl *val);
int common_icdial(struct re_printf *pf, const char *cmd,
		  enum sdp_dir dir, const char *prm, const char *hdr,
		  struct call **callp);
