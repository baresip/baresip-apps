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
 * Notification/Ring tones are played by module menu which is informed by
 * module events which tone to play.
 *
 * Configuration:
 * icnormal_aufile              normal.wav
 * icring_aufile                intercom-ring.wav,-1,500
 * icannounce_aufile            announce.wav
 * icforce_aufile               force.wav
 * icprivacy                    no
 * icallow_announce             yes
 * icallow_force                no
 * icallow_surveil              no
 *
 * icpreview_subject            preview
 * icpreview_aufile             preview.wav
 *
 * Custom intercom calls:
 * #iccustom                     <subject-prefix>,<dir>,<allow>,<aufile_key>
 * #  subject-prefix  ...  The prefix for the Subject header, in order to
 * #                       identify the custom intercom call.
 * #  dir             ...  The media direction. [sendrecv, sendonly, recvonly,
 * #                       inactive]
 * #  allow           ...  The callee uses this to decide if the incoming call
 * #                       is allowed or should be rejected.
 * #  aufile_key      ...  The config key for the auto answer file.
 * # e.g.:
 * iccustom                     Intercom/UID,sendrecv,true,ic_aufile
 * ic_aufile                    beep.wav
 *
 * Extra accounts address parameters:
 * The settings for icprivacy, icallow_announce, icallow_force, icallow_surveil
 * can be overwritten by specifying address parameter `extra` in accounts file.
 * The value for extra is a comma-separated list of settings. E.g.:
 *
 * <sip:A@localhost>;sip_autoanswer=yes;extra=icprivacy=yes,icallow_announce=no
 *
 */


#define DEBUG_MODULE "intercom"
#define DEBUG_LEVEL 5
#include <re_dbg.h>

#include "iccustom.h"
#include "ichidden.h"
#include "intercom.h"

struct intercom {
	int32_t adelay;           /**< Answer delay for outgoing calls     */
	char *ansval;             /**< Call-Info/Alert-Info value          */
	enum answer_method met;   /**< SIP auto answer method              */

	struct tmr tmr;           /**< Timer for mem_deref later           */
	struct list deref;        /**< List of mem objects to deref        */
	struct hash *custom;      /**< Hash for custom intercom call types */
};

static struct intercom st;

struct mem_le {
	struct le le;
	void *data;
};


static void mem_le_destructor(void *arg)
{
	struct mem_le *mle = arg;

	list_unlink(&mle->le);
	mem_deref(mle->data);
}


static void do_deref(void *arg)
{
	list_flush(arg);
}


int mem_deref_later(void *arg)
{
	struct mem_le *mle = mem_zalloc(sizeof(*mle), mem_le_destructor);
	mle->data = arg;

	list_append(&st.deref, &mle->le, mle);

	tmr_start(&st.tmr, 0, do_deref, &st.deref);
	return 0;
}


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


static int cmd_set_ansval(struct re_printf *pf, void *arg)
{
	const struct cmd_arg *carg = arg;

	st.ansval = mem_deref(st.ansval);
	if (str_isset(carg->prm))
		str_dup(&st.ansval, carg->prm);

	if (st.ansval)
		(void)re_hprintf(pf, "SIP auto answer value changed to %s\n",
				 st.ansval);
	else
		(void)re_hprintf(pf, "SIP auto answer value cleared\n");

	return 0;
}


int common_icdial(struct re_printf *pf, const char *cmd,
		  enum sdp_dir dir, const char *prm, const char *hdr,
		  struct call **callp)
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
	struct pl n   = PL("Subject");
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

	ua_set_autoanswer_value(ua, st.ansval);
	err |= ua_enable_autoanswer(ua, st.adelay, st.met);
	if (err)
		goto out;

	err = ua_connect_dir(ua, &call, NULL, uri, VIDMODE_ON, adir, vdir);
	if (err)
		goto out;

	re_hprintf(pf, "call id: %s\n", call_id(call));
	if (callp)
		*callp = call;
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
			     carg->prm, "normal", NULL);
}


static int cmd_announce(struct re_printf *pf, void *arg)
{
	const struct cmd_arg *carg = arg;

	return common_icdial(pf, "icannounce", SDP_SENDONLY,
			     carg->prm, "announcement", NULL);
}


static int cmd_force(struct re_printf *pf, void *arg)
{
	const struct cmd_arg *carg = arg;

	return common_icdial(pf, "icforce", SDP_SENDONLY,
			     carg->prm, "forcetalk", NULL);
}


static int cmd_surveil(struct re_printf *pf, void *arg)
{
	const struct cmd_arg *carg = arg;

	return common_icdial(pf, "icsurveil", SDP_RECVONLY,
			carg->prm, "surveillance", NULL);
}


static int cmd_reload(struct re_printf *pf, void *arg)
{
	int err;
	(void) arg;

	err = conf_configure();
	if (err) {
		(void)re_hprintf(pf, "icreload failed (%m)\n", err);
		return err;
	}

	hash_flush(st.custom);
	err = conf_apply(conf_cur(), "iccustom", iccustom_handler,
			 st.custom);
	return err;
}


static int uag_add_xhdr_intercom(void)
{
	struct le *le;
	int err;

	for (le = list_head(uag_list()); le; le = le->next) {
		struct ua *ua = le->data;
		err = ua_add_xhdr_filter(ua, "Subject");
		if (err)
			return err;
	}

	return 0;
}


static const struct cmd cmdv[] = {

{"icsetadelay", 0, CMD_PRM, "Set intercom answer delay in [s] (default: 0)",
							       cmd_set_adelay},
{"icsetansval", 0, CMD_PRM, "Set intercom Call-Info/Alert-Info value",
                                                               cmd_set_ansval},
{"icnormal",    0, CMD_PRM, "Intercom call",                   cmd_normal},
{"icannounce",  0, CMD_PRM, "Intercom announcement",           cmd_announce},
{"icforce",     0, CMD_PRM, "Intercom force during privacy",   cmd_force},
{"icsurveil",   0, CMD_PRM, "Intercom surveil peer",           cmd_surveil},
{"icreload",    0,       0, "Intercom reload config",          cmd_reload},

};


struct iccustom *iccustom_find(const struct pl *val)
{
	struct pl key;
	struct le *le;

	key = *val;
	le = hash_apply(st.custom, iccustom_lookup, &key);
	if (le)
		return le->data;

	return NULL;
}


static int module_init(void)
{
	int err;
	struct pl met = PL_INIT;

	memset(&st, 0, sizeof(st));
	st.met = ANSM_RFC5373;
	err = hash_alloc(&st.custom, 32);
	if (err)
		return err;

	err = cmd_register(baresip_commands(), cmdv, RE_ARRAY_SIZE(cmdv));
	(void)conf_get(conf_cur(), "sip_autoanswer_method", &met);
	if (!pl_strcmp(&met, "call-info"))
		st.met = ANSM_CALLINFO;
	else if (!pl_strcmp(&met, "alert-info"))
		st.met = ANSM_ALERTINFO;

	(void)conf_apply(conf_cur(), "iccustom", iccustom_handler, st.custom);
	err |= uag_event_register(ua_event_handler, NULL);
	err |= uag_add_xhdr_intercom();
	err |= iccustom_init();
	err |= ichidden_init();

	info("intercom: init\n");
	return err;
}


static int module_close(void)
{

	hash_flush(st.custom);
	mem_deref(st.custom);
	mem_deref(st.ansval);
	cmd_unregister(baresip_commands(), cmdv);
	uag_event_unregister(ua_event_handler);
	iccustom_close();
	ichidden_close();

	return 0;
}


const struct mod_export DECL_EXPORTS(intercom) = {
	"intercom",
	"application",
	module_init,
	module_close
};
