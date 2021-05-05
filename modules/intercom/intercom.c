/**
 * @file intercom.c Intercom module
 *
 * Copyright (C) 2021 Commend.com - c.spielberger@commend.com
 */

#include <string.h>
#include <stdlib.h>
#include <re.h>
#include <baresip.h>


/**
 * @defgroup intercom module
 *
 * This module implements intercom specific dial commands simplified compared
 * to use dialdir of module menu. Use command hangup of module menu to hangup
 * an intercom call!
 * It implements also a UA event handler that has to be processed before the
 * event handler of module menu. Thus be sure to load this module before module
 * menu!
 *
 */


#define DEBUG_MODULE "intercom"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


struct intercom {
	int32_t adelay;
	enum answer_method met;
};


static struct intercom st;


static int cmd_set_adelay(struct re_printf *pf, void *arg)
{
	const struct cmd_arg *carg = arg;

	if (!str_isset(carg->prm)) {
		st.adelay = 0;
		return 0;
	}

	st.adelay = atoi(carg->prm);
	if (st.adelay < 0)
		st.adelay = 0;

	(void)re_hprintf(pf, "Intercom answer delay changed to %ds\n",
			 st.adelay);
	return 0;
}


static int common_icdial(struct re_printf *pf, const char *cmd,
		enum sdp_dir dir, const char *prm, const char *hdr)
{
	int err;
	struct pl to  = PL_INIT;
	struct pl aon = PL("on");
	struct pl von = PL("on");
	enum sdp_dir adir, vdir;
	char *uri     = NULL;
	struct mbuf *uribuf = NULL;
	struct ua *ua;
	struct call *call;
	struct pl n   = PL("Intercom");
	struct pl v   = PL_INIT;
	const char *usage = "usage: /%s <address/number>"
			" audio=<on,off>"
			" video=<on,off>\n";

	if (!str_isset(prm)) {
		re_hprintf(pf, usage, cmd);
		return EINVAL;
	}

	err = re_regex(prm, str_len(prm), "[^ ]* audio=[^ ]* video=[^ ]*",
		&to, &aon, &von);
	if (err) {
		warning("intercom: could not parse %s\n", prm);
		re_hprintf(pf, usage, cmd);
		return EINVAL;
	}

	pl_strdup(&uri, &to);
	ua = uag_find_requri(uri);
	if (!ua) {
		warning("intercom: %s could not find UA\n", cmd);
		err = EINVAL;
		goto out;
	}

	pl_set_str(&v, hdr);
	err = ua_add_custom_hdr(ua, &n, &v);
	if (err) {
		warning("intercom: %s could not add header %s\n", cmd, hdr);
		err = EINVAL;
		goto out;
	}

	adir = !pl_strcmp(&aon, "on") ? dir : SDP_INACTIVE;
	vdir = !pl_strcmp(&von, "on") ? dir : SDP_INACTIVE;

	uribuf = mbuf_alloc(64);
	if (!uribuf) {
		err = ENOMEM;
		goto out;
	}

	err = account_uri_complete(ua_account(ua), uribuf, uri);
	if (err) {
		(void)re_hprintf(pf, "ua_connect failed to complete uri\n");
		goto out;
	}

	mem_deref(uri);

	uribuf->pos = 0;
	err = mbuf_strdup(uribuf, &uri, uribuf->end);
	if (err)
		goto out;

	re_hprintf(pf, "call uri: %s\n", uri);

	err |= ua_enable_autoanswer(ua, st.adelay, st.met);
	if (err)
		goto out;

	err = ua_connect_dir(ua, &call, NULL, uri, VIDMODE_ON, adir, vdir);
	if (err)
		goto out;

	re_hprintf(pf, "call id: %s\n", call_id(call));
out:
	mem_deref(uribuf);
	mem_deref(uri);
	(void)ua_disable_autoanswer(ua, st.met);
	(void)ua_rm_custom_hdr(ua, &n);
	return 0;
}


static int cmd_normal(struct re_printf *pf, void *arg)
{
	const struct cmd_arg *carg = arg;

	return common_icdial(pf, "icnormal", SDP_SENDRECV,
			     carg->prm, "normal");
}


static int cmd_announce(struct re_printf *pf, void *arg)
{
	const struct cmd_arg *carg = arg;

	return common_icdial(pf, "icannounce", SDP_SENDONLY,
			     carg->prm, "announcement");
}


static int cmd_force(struct re_printf *pf, void *arg)
{
	const struct cmd_arg *carg = arg;

	return common_icdial(pf, "icforce", SDP_SENDONLY,
			     carg->prm, "forcetalk");
}


static int cmd_surveil(struct re_printf *pf, void *arg)
{
	const struct cmd_arg *carg = arg;

	return common_icdial(pf, "icsurveil", SDP_RECVONLY,
			carg->prm, "surveillance");
}


static int intercom_handler(const struct pl *name, const struct pl *val,
	void *arg)
{
	struct call    *call = arg;
	struct ua *ua  = call_get_ua(call);
	struct account *acc  = ua_account(ua);
	enum sdp_dir ardir, vrdir;

	if (!name || !val)
		return 0;

	if (pl_strcmp(name, "Intercom"))
		return 0;


	ardir =sdp_media_rdir(
			stream_sdpmedia(audio_strm(call_audio(call))));
	vrdir = sdp_media_rdir(
			stream_sdpmedia(video_strm(call_video(call))));

	info("intercom: [ ua=%s call=%s ] %r: %r - "
	     "audio-video: %s-%s\n",
	     account_aor(acc), call_id(call), name, val,
	     sdp_dir_name(ardir), sdp_dir_name(vrdir));

	module_event("intercom", "incoming", ua, call, "%r", val);
	return 0;
}


static void ua_event_handler(struct ua *ua, enum ua_event ev,
			     struct call *call, const char *prm, void *arg)
{
	const struct list *hdrs;
	(void)prm;
	(void)arg;
	(void)ua;

	switch (ev) {

	case UA_EVENT_CALL_INCOMING:

		hdrs = call_get_custom_hdrs(call);
		(void)custom_hdrs_apply(hdrs, intercom_handler, call);

		break;

	default:
		break;
	}
}


static int uag_add_xhdr_intercom(void)
{
	struct le *le;
	int err;

	for (le = list_head(uag_list()); le; le = le->next) {
		struct ua *ua = le->data;
		err = ua_add_xhdr_filter(ua, "Intercom");
		if (err)
			return err;
	}

	return 0;
}


static const struct cmd cmdv[] = {

{"icsetadelay", 0, CMD_PRM, "Set intercom answer delay in [s] (default: 0)",
							       cmd_set_adelay},
{"icnormal",    0, CMD_PRM, "Intercom call",                   cmd_normal},
{"icannounce",  0, CMD_PRM, "Intercom announcement",           cmd_announce},
{"icforce",     0, CMD_PRM, "Intercom force during privacy",   cmd_force},
{"icsurveil",   0, CMD_PRM, "Intercom surveil peer",           cmd_surveil},

};


static int module_init(void)
{
	int err;
	struct pl met;

	memset(&st.adelay, 0, sizeof(st));
	st.met = ANSM_RFC5373;

	err = cmd_register(baresip_commands(), cmdv, ARRAY_SIZE(cmdv));
	(void)conf_get(conf_cur(), "sip_autoanswer_method", &met);
	if (!pl_strcmp(&met, "call-info"))
		st.met = ANSM_CALLINFO;
	else if (!pl_strcmp(&met, "alert-info"))
		st.met = ANSM_ALERTINFO;

	err |= uag_event_register(ua_event_handler, NULL);
	err |= uag_add_xhdr_intercom();

	info("intercom: init\n");
	return err;
}


static int module_close(void)
{

	cmd_unregister(baresip_commands(), cmdv);
	uag_event_unregister(ua_event_handler);

	return 0;
}


const struct mod_export DECL_EXPORTS(intercom) = {
	"intercom",
	"application",
	module_init,
	module_close
};
