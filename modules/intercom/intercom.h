/**
 * @file intercom.h Intercom module internal interface
 *
 * Copyright (C) 2021 Commend.com - c.spielberger@commend.com
 */

struct intercom {
	int32_t adelay;
	enum answer_method met;
	bool privacy;
};


void ua_event_handler(struct ua *ua, enum ua_event ev,
			     struct call *call, const char *prm, void *arg);
bool ic_privacy(void);
