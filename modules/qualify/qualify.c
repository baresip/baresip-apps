/**
 * @file qualify.c Pinging of peer in CALL_STATE_INCOMING via SIP OPTIONS
 *
 * Copyright (C) 2023 Commend.com - m.fridrich@commend.com
 */

#include <string.h>
#include <stdlib.h>
#include <re.h>
#include <baresip.h>


/**
 * @defgroup qualify module
 *
 * This module implements a pinging mechanism using SIP OPTIONS to qualify the
 * peer while a call is in the INCOMING state to ensure that the peer is
 * reachable.
 *
 * Configure in address parameter `extra`:
 * qual_freq     [seconds]    qualify frequency
 * qual_to       [seconds]    qualify timeout
 *
 * The OPTIONS are only sent if both options are present, both are not zero,
 * qualify_freq is greater than qual_to, and the call is incoming. As soon as
 * the call is established or closed, sending of OPTIONS is stopped.
 * If no response to an OPTIONS request is received within the specified
 * timeout, UA_EVENT_CUSTOM with "Peer offline" is triggered.
 * The sending of OPTIONS still continues and if a subsequent OPTIONS is
 * answered, UA_EVENT_CUSTOM with "Peer online" is triggered.
 *
 * Example:
 * <sip:A@sip.example.com>;extra=qual_freq=5,qual_to=2
 *
 */


#define DEBUG_MODULE "qualify"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


struct qualle {
	struct le he;
	struct call *call;
	bool offline;
	struct tmr freq_tmr;
	struct tmr to_tmr;
};


struct qualify {
	struct hash *qual_map;
};


static struct qualify q = { NULL };


/* Forward declaration */
static int call_start_qualify(struct call *call,
			      const struct account *acc,
			      struct qualle *qualle);


static void qualle_destructor(void *arg)
{
	struct qualle *qualle = arg;

	hash_unlink(&qualle->he);
	tmr_cancel(&qualle->to_tmr);
	tmr_cancel(&qualle->freq_tmr);
}


/**
 * Get specified field in the accounts extra parameter list
 *
 * @param acc  Accounts object
 * @param n    Specified field
 * @param v    uint32_t ptr for the specified value
 *
 * @return 0 if success, otherwise errorcode
 */
static int account_extra_uint(const struct account *acc, const char *n,
			      uint32_t *v)
{
	struct pl pl;
	struct pl val;
	const char *extra = NULL;

	if (!acc || !n  || !v)
		return EINVAL;

	extra = account_extra(acc);
	if (!str_isset(extra))
		return EINVAL;

	pl_set_str(&pl, extra);
	if (!fmt_param_sep_get(&pl, n, ',', &val))
		return EINVAL;

	*v = pl_u32(&val);

	return 0;
}


static void options_resp_handler(int err, const struct sip_msg *msg, void *arg)
{
	(void)msg;
	struct qualle *qualle = arg;

	if (err) {
		warning("OPTIONS reply error: %m\n", err);
		return;
	}

	tmr_cancel(&qualle->to_tmr);

	if (qualle->offline) {
		qualle->offline = false;
		ua_event(call_get_ua(qualle->call), UA_EVENT_CUSTOM,
			 qualle->call, "Peer online");
	}
}


static void to_handler(void *arg)
{
	struct qualle *qualle = arg;
	struct call *call = qualle->call;
	uint32_t qual_to = 0;
	account_extra_uint(call_account(call), "qual_to", &qual_to);

	if (!qualle->offline) {
		qualle->offline = true;
		ua_event(call_get_ua(call), UA_EVENT_CUSTOM, call,
			 "Peer offline");
	}

	info("No response recevied to OPTIONS in %u seconds.", qual_to);
}


static void freq_handler(void *arg)
{
	struct qualle *qualle = arg;
	(void)call_start_qualify(qualle->call, call_account(qualle->call),
				 qualle);
}


/**
 * Returns -1 if qual_freq or qual_to are zero
 *	   -2 if qual_to is greater than or equal to qual_freq
 *	   0 on success
 *	   else error code
 */
static int call_start_qualify(struct call *call,
			      const struct account *acc,
			      struct qualle *qualle)
{
	int err;
	struct sa peer_addr;
	char peer_uri[128];
	uint32_t qual_to = 0;
	uint32_t qual_freq = 0;
	int newle = qualle == NULL;

	account_extra_uint(acc, "qual_freq", &qual_freq);
	account_extra_uint(acc, "qual_to", &qual_to);

	if (!call || !qual_freq || !qual_to) {
		return -1;
	}

	if (qual_to >= qual_freq) {
		warning("Will not send OPTIONS because qualify timeout is "
			"greater than or equal to qualify frequency.\n"
			"qual_to: %u, qual_freq: %u\n", qual_to, qual_freq);
		return -2;
	}

	if (newle) {
		qualle = mem_zalloc(sizeof(*qualle), qualle_destructor);
		if (!qualle)
			return ENOMEM;

		qualle->call = call;
		tmr_init(&qualle->to_tmr);
		tmr_init(&qualle->freq_tmr);
		hash_append(q.qual_map, hash_fast_str(account_aor(acc)),
			    &qualle->he, qualle);
	}

	(void)call_msg_src(call, &peer_addr);
	err = re_snprintf(peer_uri, sizeof(peer_uri), "sip:%H:%d",
		   sa_print_addr, &peer_addr, sa_port(&peer_addr));

	if (err == -1 || err == 0) {
		warning("Failed to get peer URI for sending OPTIONS ping. "
			"Trying again in %u seconds.\n", err, qual_freq);
		tmr_start(&qualle->freq_tmr, qual_freq * 1000, freq_handler,
			  qualle);
		return err;
	}

	err = ua_options_send(call_get_ua(call), peer_uri,
			      options_resp_handler, qualle);
	if (err) {
		warning("Sending OPTIONS failed with err %d. "
			"Trying again in %u seconds.\n", err, qual_freq);
		tmr_start(&qualle->freq_tmr, qual_freq * 1000, freq_handler,
			  qualle);
		return err;
	}

	tmr_start(&qualle->to_tmr, qual_to * 1000, to_handler, qualle);
	tmr_start(&qualle->freq_tmr, qual_freq * 1000, freq_handler, qualle);

	return 0;
}


static bool qualle_get_applyh(struct le *le, void *arg)
{
	(void)le;
	(void)arg;

	return true;
}


static void call_stop_qualify(struct account *acc)
{
	struct qualle *qualle;
	struct le *le = hash_lookup(q.qual_map,
				    hash_fast_str(account_aor(acc)),
				    qualle_get_applyh, NULL);

	if (!le || !le->data)
		return;

	qualle = le->data;
	mem_deref(qualle);
}


static void ua_event_handler(struct ua *ua, enum ua_event ev,
			     struct call *call, const char *prm, void *arg)
{
	struct account *acc = ua_account(ua);
	(void) call;
	(void) prm;
	(void) arg;

	switch (ev) {
		case UA_EVENT_CALL_INCOMING:
			(void)call_start_qualify(call, acc, NULL);
			break;
		case UA_EVENT_CALL_ESTABLISHED:
		case UA_EVENT_CALL_ANSWERED:
			if (call_is_outgoing(call))
			    break;

			call_stop_qualify(acc);
			break;
		case UA_EVENT_CALL_CLOSED:
			call_stop_qualify(acc);
			break;
		case UA_EVENT_CUSTOM:
			warning("UA_EVENT_CUSTOM. prm: %s\n", prm);
			break;
		default:
			break;
	}
}


static int module_init(void)
{
	int err;

	info("qualify: init\n");

	err = uag_event_register(ua_event_handler, NULL);
	err |= hash_alloc(&q.qual_map, 32);

	return err;
}


static int module_close(void)
{
	uag_event_unregister(ua_event_handler);
	hash_flush(q.qual_map);
	mem_deref(q.qual_map);

	return 0;
}


const struct mod_export DECL_EXPORTS(qualify) = {
	"qualify",
	"application",
	module_init,
	module_close
};
