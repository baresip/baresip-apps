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
 * qual_int      [seconds]    qualify interval
 * qual_to       [seconds]    qualify timeout
 *
 * The OPTIONS are only sent if both options are present, both are not zero,
 * qual_int is greater than qual_to, and the call is incoming. As soon as
 * the call is established or closed, sending of OPTIONS is stopped.
 * If no response to an OPTIONS request is received within the specified
 * timeout, a BEVENT_MODULE with "peer offline" is triggered.
 * In this case, the sending of OPTIONS still continues and if a subsequent
 * OPTIONS is answered, a BEVENT_MODULE with "peer online" is triggered.
 *
 * Example:
 * <sip:A@sip.example.com>;extra=qual_int=5,qual_to=2
 *
 */


struct qualle {
	struct le he;
	struct call *call;
	bool offline;
	struct tmr int_tmr;
	struct tmr to_tmr;
};


struct qualify {
	struct hash *qual_map;
};


static struct qualify q = { NULL };


/* Forward declaration */
static void call_start_qualify(struct call *call,
			       const struct account *acc,
			       struct qualle *qualle);


static void qualle_destructor(void *arg)
{
	struct qualle *qualle = arg;

	hash_unlink(&qualle->he);
	tmr_cancel(&qualle->to_tmr);
	tmr_cancel(&qualle->int_tmr);
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
		info("qualify: OPTIONS reply error (%m)\n", err);
		mem_deref(qualle);
		return;
	}

	tmr_cancel(&qualle->to_tmr);

	if (qualle->offline && qualle->call) {
		qualle->offline = false;
		module_event("qualify", "peer online",
			     call_get_ua(qualle->call), qualle->call, "");
	}

	mem_deref(qualle);
}


static void to_handler(void *arg)
{
	struct qualle *qualle = arg;
	struct call *call = qualle->call;
	uint32_t qual_to = 0;
	account_extra_uint(call_account(call), "qual_to", &qual_to);

	if (!qualle->offline) {
		qualle->offline = true;
		module_event("qualify", "peer offline",
			     call_get_ua(qualle->call), qualle->call, "");
	}

	debug("qualify: no response received to OPTIONS in %u seconds",
	      qual_to);
}


static void interval_handler(void *arg)
{
	struct qualle *qualle = arg;
	call_start_qualify(qualle->call, call_account(qualle->call), qualle);
}


static void call_start_qualify(struct call *call,
			      const struct account *acc,
			      struct qualle *qualle)
{
	struct sa peer_addr;
	char peer_uri[128];
	uint32_t qual_to = 0;
	uint32_t qual_int = 0;
	int newle = qualle == NULL;

	account_extra_uint(acc, "qual_int", &qual_int);
	account_extra_uint(acc, "qual_to", &qual_to);

	if (!call || !qual_int || !qual_to)
		return;

	if (qual_to >= qual_int) {
		warning("qualify: timeout >= interval (%u >= %u)\n",
			qual_to, qual_int);
		return;
	}

	if (newle) {
		qualle = mem_zalloc(sizeof(*qualle), qualle_destructor);
		if (!qualle)
			return;

		qualle->call = call;
		tmr_init(&qualle->to_tmr);
		tmr_init(&qualle->int_tmr);
		hash_append(q.qual_map, hash_fast_str(call_id(call)),
			    &qualle->he, qualle);
	}

	(void)call_msg_src(call, &peer_addr);

	int n = re_snprintf(peer_uri, sizeof(peer_uri),
			  "sip:%J%s", &peer_addr,
			  sip_transp_param(call_transp(call)));

	if (n <= 0) {
		warning("qualify: failed to get peer URI for %s\n",
			call_peeruri(call));
		return;
	}

	int err = ua_options_send(call_get_ua(call), peer_uri,
				  options_resp_handler, mem_ref(qualle));
	if (err) {
		mem_deref(qualle);
		warning("qualify: sending OPTIONS failed (%m)\n", err);
		tmr_start(&qualle->int_tmr, qual_int * 1000, interval_handler,
			  qualle);
		return;
	}

	tmr_start(&qualle->to_tmr, qual_to * 1000, to_handler, qualle);
	tmr_start(&qualle->int_tmr, qual_int * 1000, interval_handler, qualle);
}


static bool qualle_get_applyh(struct le *le, void *arg)
{
	const struct call *call = arg;
	const struct qualle *qualle = le->data;

	return qualle->call == call;
}


static struct qualle *call_get_qualle(struct call *call)
{
	if (!call)
		return NULL;

	struct le *le = hash_lookup(q.qual_map, hash_fast_str(call_id(call)),
				    qualle_get_applyh, call);

	return le ? le->data : NULL;
}


static void qualle_stop_tmrs(struct qualle *qualle)
{
	if (!qualle)
		return;

	tmr_cancel(&qualle->to_tmr);
	tmr_cancel(&qualle->int_tmr);
}


static void event_handler(enum bevent_ev ev, struct bevent *event, void *arg)
{
	struct ua   *ua   = bevent_get_ua(event);
	struct call *call = bevent_get_call(event);
	struct account *acc = ua_account(ua);
	struct qualle *qualle;
	(void) arg;

	switch (ev) {
		case BEVENT_CALL_INCOMING:
			(void)call_start_qualify(call, acc, NULL);
			break;

		case BEVENT_CALL_ESTABLISHED:
			if (call_is_outgoing(call))
			    break;

			qualle = call_get_qualle(call);
			qualle_stop_tmrs(qualle);
			break;

		case BEVENT_CALL_CLOSED:
			qualle = call_get_qualle(call);
			qualle_stop_tmrs(qualle);

			if (qualle) {
				qualle->call = NULL;
				mem_deref(qualle);
			}

			break;
		default:
			break;
	}
}


static int module_init(void)
{
	int err;

	info("qualify: init\n");

	err  = bevent_register(event_handler, NULL);
	err |= hash_alloc(&q.qual_map, 32);

	return err;
}


static int module_close(void)
{
	bevent_unregister(event_handler);
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
