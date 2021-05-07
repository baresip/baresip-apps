/**
 * @file incoming.c Intercom module process incoming calls
 *
 * Copyright (C) 2021 Commend.com - c.spielberger@commend.com
 */

#include <string.h>
#include <stdlib.h>
#include <re.h>
#include <baresip.h>

#include "intercom.h"




static bool is_intercom(const struct pl *name)
{
	return 0 == pl_strcmp(name, "Intercom");
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


static int incoming_handler(const struct pl *name,
		const struct pl *val, void *arg)
{
	struct call    *call = arg;
	struct ua *ua  = call_get_ua(call);
	struct account *acc  = ua_account(ua);
	enum sdp_dir ardir, vrdir;

	if (!name || !val)
		return 0;

	if (!is_intercom(name))
		return 0;

	ardir =sdp_media_rdir(
			stream_sdpmedia(audio_strm(call_audio(call))));
	vrdir = sdp_media_rdir(
			stream_sdpmedia(video_strm(call_video(call))));

	info("intercom: [ ua=%s call=%s ] %r: %r - "
	     "audio-video: %s-%s\n",
	     account_aor(acc), call_id(call), name, val,
	     sdp_dir_name(ardir), sdp_dir_name(vrdir));

	if (ic_privacy() && is_normal(val)) {
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

	if (is_announcement(val)) {
		module_event("intercom", "override-aufile", ua, call,
				"sip_autoanswer_aufile:icannounce_aufile");
		return 0;
	}

	if (is_forcetalk(val)) {
		module_event("intercom", "override-aufile", ua, call,
				"sip_autoanswer_aufile:icforce_aufile");
		return 0;
	}

	if (is_surveillance(val)) {
		module_event("intercom", "override-aufile", ua, call,
				"sip_autoanswer_aufile:none");
		return 0;
	}

	return 0;
}


static int outgoing_handler(const struct pl *name,
		const struct pl *val, void *arg)
{
	struct call    *call = arg;
	struct ua *ua  = call_get_ua(call);

	if (!name || !val)
		return 0;

	if (!is_intercom(name))
		return 0;

	module_event("intercom", "outgoing", ua, call, "%r", val);

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

	if (!is_intercom(name))
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
		return 0;
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

	switch (ev) {

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

