/**
 * @file incoming.c Intercom module process incoming calls
 *
 * Copyright (C) 2021 Commend.com - c.spielberger@commend.com
 */

#include <string.h>
#include <stdlib.h>
#include <re.h>
#include <baresip.h>

#include "iccustom.h"
#include "intercom.h"

static int reject_call(struct call *call, uint16_t scode, const char *reason)
{
	struct ua *ua = call_get_ua(call);
	call_hangup(call, scode, reason);

	ua_event(ua, UA_EVENT_CALL_CLOSED, call, reason);
	return mem_deref_later(call);
}


static bool is_normal(const struct pl *val)
{
	return !pl_strcmp(val, "normal");
}


static bool is_announcement(const struct pl *val)
{
	return !pl_strcmp(val, "announcement");
}


static bool is_forcetalk(const struct pl *val)
{
	return !pl_strcmp(val, "forcetalk");
}


static bool is_surveillance(const struct pl *val)
{
	return !pl_strcmp(val, "surveillance");
}


static bool is_preview(const struct pl *val)
{
	struct pl subj = PL("preview");

	(void)conf_get(conf_cur(), "icpreview_subject", &subj);
	return !strncmp(val->p, subj.p, subj.l);
}


static bool is_intercom(const struct pl *name, const struct pl *val)
{
	if (pl_strcmp(name, "Subject"))
		return false;

	if (is_normal(val) ||
	    is_announcement(val) ||
	    is_forcetalk(val) ||
	    is_surveillance(val) ||
	    is_preview(val) ||
	    ic_is_custom(val))
		return true;

	return false;
}


static bool account_extra_bool(const struct account *acc, const char *name,
		bool *set)
{
	struct pl pl = PL_INIT;
	struct pl val = PL_INIT;

	pl_set_str(&pl, account_extra(acc));

	if (!pl_isset(&pl))
		return false;

	if (!fmt_param_sep_get(&pl, name, ',', &val))
		return false;

	if (!pl_strcmp(&val, "yes")) {
		*set = true;
		return true;
	}
	else if (!pl_strcmp(&val, "no")) {
		*set = false;
		return true;
	}

	return false;
}


static int incoming_handler(const struct pl *name,
		const struct pl *val, void *arg)
{
	struct call    *call = arg;
	struct ua *ua  = call_get_ua(call);
	struct account *acc  = ua_account(ua);
	enum sdp_dir ardir, vrdir;
	bool privacy        = false;
	bool allow_announce = true;
	bool allow_force    = false;
	bool allow_surveil  = false;
	int err = 0;

	if (!name || !val)
		return 0;

	if (!is_intercom(name, val))
		return 0;

	ardir =sdp_media_rdir(
			stream_sdpmedia(audio_strm(call_audio(call))));
	vrdir = sdp_media_rdir(
			stream_sdpmedia(video_strm(call_video(call))));

	info("intercom: [ ua=%s call=%s ] %r: %r - "
	     "audio-video: %s-%s\n",
	     account_aor(acc), call_id(call), name, val,
	     sdp_dir_name(ardir), sdp_dir_name(vrdir));

	(void)conf_get_bool(conf_cur(), "icprivacy", &privacy);
	(void)conf_get_bool(conf_cur(), "icallow_announce", &allow_announce);
	(void)conf_get_bool(conf_cur(), "icallow_force", &allow_force);
	(void)conf_get_bool(conf_cur(), "icallow_surveil", &allow_surveil);

	(void)account_extra_bool(acc, "icprivacy", &privacy);
	(void)account_extra_bool(acc, "icallow_announce", &allow_announce);
	(void)account_extra_bool(acc, "icallow_force", &allow_force);
	(void)account_extra_bool(acc, "icallow_surveil", &allow_surveil);

	if (privacy && is_normal(val)) {
		info("intercom: auto answer suppressed - privacy mode on\n");
		call_set_answer_delay(call, -1);
		module_event("intercom", "override-aufile", ua, call,
				"ring_aufile:icring_aufile");
		return 0;
	}

	module_event("intercom", "incoming", ua, call, "%r", val);

	if (is_normal(val)) {
		module_event("intercom", "override-aufile", ua, call,
				"sip_autoanswer_aufile:icnormal_aufile");
		return 0;
	}

	if (ic_is_custom(val)) {
		struct pl *auf = iccustom_aufile(val);
		if (!iccustom_allowed(val)) {
			reject_call(call, 406, "Not Acceptable");
			return 0;
		}

		module_event("intercom", "override-aufile", ua, call,
				"sip_autoanswer_aufile:%r", auf);
		return 0;
	}

	if (is_announcement(val)) {
		if (!allow_announce) {
			reject_call(call, 406, "Not Acceptable");
			return 0;
		}

		module_event("intercom", "override-aufile", ua, call,
				"sip_autoanswer_aufile:icannounce_aufile");
		return 0;
	}

	if (is_forcetalk(val)) {
		if (!allow_force) {
			reject_call(call, 406, "Not Acceptable");
			return 0;
		}

		module_event("intercom", "override-aufile", ua, call,
				"sip_autoanswer_aufile:icforce_aufile");
		return 0;
	}

	if (is_surveillance(val)) {
		if (!allow_surveil) {
			reject_call(call, 406, "Not Acceptable");
			return 0;
		}

		module_event("intercom", "override-aufile", ua, call,
				"sip_autoanswer_aufile:none");
		return 0;
	}

	if (is_preview(val)) {
		module_event("intercom", "override-aufile", ua, call,
				"ring_aufile:icpreview_aufile");
		err |= call_progress_dir(call, SDP_INACTIVE, SDP_RECVONLY);
	}

	return err;
}


static int outgoing_handler(const struct pl *name,
		const struct pl *val, void *arg)
{
	struct call    *call = arg;
	struct ua *ua  = call_get_ua(call);

	if (!name || !val)
		return 0;

	if (!is_intercom(name, val))
		return 0;

	module_event("intercom", "outgoing", ua, call, "%r", val);
	module_event("intercom", "override-aufile", ua, call,
		     "ringback_aufile:icringback_aufile");

	return 0;
}


static int established_handler(const struct pl *name,
		const struct pl *val, void *arg)
{
	struct call    *call = arg;
	struct ua *ua  = call_get_ua(call);
	enum sdp_dir aldir, vldir;
	bool outgoing = call_is_outgoing(call);

	if (!name || !val)
		return 0;

	if (!is_intercom(name, val))
		return 0;

	aldir =sdp_media_ldir(
			stream_sdpmedia(audio_strm(call_audio(call))));
	vldir = sdp_media_ldir(
			stream_sdpmedia(video_strm(call_video(call))));

	if (outgoing && is_forcetalk(val)) {

		/* this allows incoming re-INVITE with SDP dir SDP_SENDRECV */
		call_set_media_direction(call,
				aldir ? SDP_SENDRECV : SDP_INACTIVE,
				vldir ? SDP_SENDRECV : SDP_INACTIVE);
	}

	module_event("intercom", outgoing ?
			"outgoing-established" : "incoming-established",
			ua, call, "%r", val);
	return 0;
}


void ua_event_handler(struct ua *ua, enum ua_event ev,
			     struct call *call, const char *prm, void *arg)
{
	const struct list *hdrs;
	(void)prm;
	(void)arg;
	(void)ua;

	if (call_state(call)==CALL_STATE_TERMINATED)
		return;

	switch (ev) {

	case UA_EVENT_CREATE:
		ua_add_xhdr_filter(ua, "Subject");
		break;

	case UA_EVENT_CALL_INCOMING:

		hdrs = call_get_custom_hdrs(call);
		(void)custom_hdrs_apply(hdrs, incoming_handler, call);
		break;

	case UA_EVENT_CALL_LOCAL_SDP:
		if (call_state(call) != CALL_STATE_OUTGOING)
			break;

		hdrs = call_get_custom_hdrs(call);
		(void)custom_hdrs_apply(hdrs, outgoing_handler, call);
		break;

	case UA_EVENT_CALL_ESTABLISHED:

		if (!call_is_outgoing(call))
			break;

		hdrs = call_get_custom_hdrs(call);
		(void)custom_hdrs_apply(hdrs, established_handler, call);
		break;

	default:
		break;
	}
}

