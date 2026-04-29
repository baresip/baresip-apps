/**
 * @file calltimer.c Established call timeout module
 *
 * Copyright (C) 2026 Silvio Fosso
 */

#include <string.h>
#include <re.h>
#include <baresip.h>

/**
 * @defgroup calltimer calltimer
 *
 * Established call timeout module.
 *
 * Configuration:
 *
 *   calltimer_max_duration 120
 *
 * Semantics:
 *
 *   0   = disabled
 *   > 0 = maximum established call duration in seconds
 *
 * The timer is per-call. If there are N established calls, each call has
 * its own independent timer. When a timer expires, only that specific call
 * is terminated.
 */

struct calltimer_entry {
	struct le he;
	struct call *call;
	struct tmr tmr;
};

static struct {
	struct hash *calls;
	uint32_t max_duration;
} d;


static void entry_destructor(void *arg)
{
	struct calltimer_entry *entry = arg;

	tmr_cancel(&entry->tmr);
	hash_unlink(&entry->he);
	mem_deref(entry->call);
}


static bool entry_find_handler(struct le *le, void *arg)
{
	const struct calltimer_entry *entry = le->data;
	return entry->call == arg;
}


static struct calltimer_entry *entry_find(const struct call *call)
{
	struct le *le;

	if (!call || !d.calls)
		return NULL;

	le = hash_lookup(d.calls, hash_fast_str(call_id(call)),
			 entry_find_handler, (void *)call);

	return le ? le->data : NULL;
}


static void timer_stop(struct call *call)
{
	struct calltimer_entry *entry;

	if (!call)
		return;

	entry = entry_find(call);
	if (entry)
		mem_deref(entry);
}


static void timeout_handler(void *arg)
{
	struct calltimer_entry *entry = arg;
	struct call *call;
	struct ua *ua;

	if (!entry || !entry->call)
		return;

	call = entry->call;
	ua = call_get_ua(call);

	info("calltimer: maximum established call duration reached"
	      " for call %s after %u seconds\n",
	      call_id(call), d.max_duration);

	ua_hangup(ua, call, 0, "Maximum call duration reached");
}


static void timer_start(struct call *call)
{
	struct calltimer_entry *entry;

	if (!call || !d.max_duration)
		return;

	entry = entry_find(call);
	if (entry) {
		tmr_cancel(&entry->tmr);
	}
	else {
		entry = mem_zalloc(sizeof(*entry), entry_destructor);
		if (!entry)
			return;

		entry->call = mem_ref(call);

		hash_append(d.calls, hash_fast_str(call_id(call)),
			    &entry->he, entry);
	}

	tmr_start(&entry->tmr, d.max_duration * 1000,
		  timeout_handler, entry);

	debug("calltimer: started timer for call %s, duration=%u seconds\n",
	      call_id(call), d.max_duration);
}


static void event_handler(enum bevent_ev ev, struct bevent *event, void *arg)
{
	struct call *call = bevent_get_call(event);

	(void)arg;

	switch (ev) {

	case BEVENT_CALL_ESTABLISHED:
		timer_start(call);
		break;

	case BEVENT_CALL_CLOSED:
		timer_stop(call);
		break;

	default:
		break;
	}
}


static int module_init(void)
{
	int err;

	memset(&d, 0, sizeof(d));

	conf_get_u32(conf_cur(), "calltimer_max_duration",
		     &d.max_duration);

	err = hash_alloc(&d.calls, 32);
	if (err)
		return err;

	err = bevent_register(event_handler, NULL);
	if (err) {
		d.calls = mem_deref(d.calls);
		return err;
	}

	debug("calltimer: module loaded, max_duration=%u seconds\n",
	      d.max_duration);

	return 0;
}


static int module_close(void)
{
	bevent_unregister(event_handler);

	hash_flush(d.calls);
	d.calls = mem_deref(d.calls);

	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(calltimer) = {
	"calltimer",
	"application",
	module_init,
	module_close
};
