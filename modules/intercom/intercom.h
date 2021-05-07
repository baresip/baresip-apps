/**
 * @file intercom.h Intercom module internal interface
 *
 * Copyright (C) 2021 Commend.com - c.spielberger@commend.com
 */

struct intercom {
	int32_t adelay;           /**< Answer delay for outgoing calls     */
	enum answer_method met;   /**< SIP auto answer method              */

	struct tmr tmr;           /**< Timer for mem_deref later           */
	struct list deref;        /**< List of mem objects to deref        */
};


void ua_event_handler(struct ua *ua, enum ua_event ev,
			     struct call *call, const char *prm, void *arg);


int mem_deref_later(void *arg);
