/**
 * @file kaoptions.c Keepalive via SIP OPTIONS message
 *
 * Copyright (C) 2021 Commend.com - c.huber@commend.com
 */

#include <string.h>
#include <stdlib.h>
#include <re.h>
#include <baresip.h>


/**
 * @defgroup kaoptions module
 *
 * This module implements a lightweight keepalive mechanism via SIP OPTIONS
 * requests. It can be used if the SIP proxy does not support RFC-5626
 * NAT outbound. It is not capable of detecting any connection flow problems
 * like a change in a NAT binding.
 *
 * Configuration:
 * kaoptions     [seconds]
 *
 * Extra account address parameters:
 * The module can be activated by adding the kaoptions to the accounts
 * parameter `extra`.
 *
 * Example:
 * <sip:A@sip.example.com>;extra=kaoptions=30
 *
 */


#define DEBUG_MODULE "kaoptions"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


struct kaoptions {
	struct list ka_ual;
};


static struct kaoptions kao = {
	LIST_INIT,
};


struct kao_element {
	struct le le;

	struct ua *ua;
	struct tmr tmr;

	size_t ka_interval;
};


static void kao_element_destructor(void *arg)
{
	struct kao_element *kaoe = arg;

	tmr_cancel(&kaoe->tmr);
}


/**
 * Keepalive Options UA comparison
 *
 * @param le  List element (kao_element)
 * @param arg Argument     (UA object)
 *
 * @return true if UA ptr match, false otherwise
 */
static bool kao_element_ua_cmp(struct le *le, void *arg)
{
	struct ua *ua = arg;
	struct kao_element *kaoe = le->data;

	return kaoe->ua == ua;
}


/**
 * Get specified field in the accounts extra parameter list
 *
 * @param acc  Accounts object
 * @param n    Specified field
 * @param v    Int ptr for the specified value
 *
 * @return 0 if success, otherwise errorcode
 */
static int account_extra_int(const struct account *acc, const char *n, int *v)
{
	struct pl pl;
	struct pl val;
	const char *extra = NULL;

	if (!acc || !n  || !v)
		return EINVAL;

	extra = account_extra(acc);
	if (!str_isset(extra))
		return EINVAL;

	pl_set_str(&pl, account_extra(acc));
	if (!fmt_param_sep_get(&pl, n, ',', &val))
		return EINVAL;

	*v = pl_u32(&val);

	return 0;
}


/**
 * Keepalive timer handler
 *
 * @param arg Argument (kao_element)
 */
static void keepalive_send_handler(void *arg)
{
	struct account *acc = NULL;
	struct kao_element *kaoe = arg;
	char *uri = NULL;
	int err = 0;

	acc = ua_account(kaoe->ua);
	err = re_sdprintf(&uri, "%H", uri_encode, account_luri(acc));
	if (err)
		goto out;

	err = ua_options_send(kaoe->ua, uri, NULL, NULL);
	if (err)
		goto out;

	tmr_start(&kaoe->tmr, kaoe->ka_interval, keepalive_send_handler, kaoe);

out:
	mem_deref(uri);
}


/**
 * Allocate and start a SIP OPTIONS keepalive for a given ua
 *
 * @param ua UA object
 *
 * @return 0 if success, otherwise errorcode
 */
static int kaoptions_alloc(struct ua *ua)
{
	struct account *acc;
	struct le *le;
	struct kao_element *kaoe = NULL;
	int sec = 0;
	int err = 0;

	if (!ua)
		return EINVAL;

	acc = ua_account(ua);
	err = account_extra_int(acc, "kaoptions", &sec);
	if (err)
		return err;

	le = list_apply(&kao.ka_ual, true, kao_element_ua_cmp, ua);
	if (le)
		return 0;

	kaoe = mem_zalloc(sizeof(*kaoe), kao_element_destructor);
	if (!kaoe)
		return ENOMEM;

	kaoe->ua = ua;
	kaoe->ka_interval = sec * 1000;
	tmr_start(&kaoe->tmr, kaoe->ka_interval, keepalive_send_handler, kaoe);

	list_append(&kao.ka_ual, &kaoe->le, kaoe);

	return err;
}


/**
 * Stop a SIP OPTIONS keepalive for a given ua
 *
 * @param ua UA object
 *
 * @return 0 if success, otherwise errorcode
 */
static int kaoptions_stop(struct ua *ua)
{
	struct le *le;
	struct kao_element *kaoe;

	if (!ua)
		return EINVAL;

	le = list_apply(&kao.ka_ual, true, kao_element_ua_cmp, ua);
	if (!le)
		return 0;

	kaoe = le->data;
	list_unlink(&kaoe->le);
	mem_deref(kaoe);

	return 0;
}


static void event_handler(enum bevent_ev ev, struct bevent *event, void *arg)
{
	struct ua *ua = bevent_get_ua(event);
	(void) arg;

	switch (ev) {
		case BEVENT_REGISTER_OK:
				kaoptions_alloc(ua);
			break;
		case BEVENT_REGISTER_FAIL:
				kaoptions_stop(ua);
			break;
		case BEVENT_UNREGISTERING:
				kaoptions_stop(ua);
			break;

		default:
			break;
	}
}


static int module_init(void)
{
	int err;

	err = bevent_register(event_handler, NULL);

	info("kaoptions: init\n");
	return err;
}


static int module_close(void)
{
	bevent_unregister(event_handler);
	list_flush(&kao.ka_ual);

	return 0;
}


const struct mod_export DECL_EXPORTS(kaoptions) = {
	"kaoptions",
	"application",
	module_init,
	module_close
};
