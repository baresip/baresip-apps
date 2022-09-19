/**
 * @file ichidden.c Hidden DTMF calls
 *
 * Copyright (C) 2022 Commend.com - c.spielberger@commend.com
 */

#include <string.h>
#include <re.h>
#include <baresip.h>


#define DEBUG_MODULE "intercom"
#define DEBUG_LEVEL 5
#include <re_dbg.h>

#include "intercom.h"
#include "ichidden.h"

enum hidden_state {
	HIDDEN_ESTABLISHED  = 0,
	HIDDEN_SEND,
	HIDDEN_CLOSE,
};

struct hidden_call {
	struct le le;

	struct call *call;
	enum hidden_state state;
	struct tmr tmr;
	char *code;
};

struct list hcalls;


static void hidden_call_destructor(void *arg)
{
	struct hidden_call *hc = arg;

	list_unlink(&hc->le);
	tmr_cancel(&hc->tmr);
	mem_deref(hc->code);
}


int hidden_call_append(struct call *call, const struct pl *code)
{
	struct hidden_call *hc;
	hc = mem_zalloc(sizeof(*hc), hidden_call_destructor);
	if (!hc)
		return ENOMEM;

	hc->call = call;
	pl_strdup(&hc->code, code);
	list_append(&hcalls, &hc->le, hc);
	return 0;
}


static bool find_hidden_call(struct le *le, void *arg)
{
	struct call *call = arg;
	struct hidden_call *hc = le->data;

	if (hc->call == call)
		return true;

	return false;
}


static int call_send_code(struct call *call, const char *code)
{
	size_t i;
	int err = 0;

	for (i = 0; i < str_len(code) && !err; i++)
		err = call_send_digit(call, code[i]);

	if (!err)
		err = call_send_digit(call, KEYCODE_REL);

	return err;
}


static void proc_hidden_call(void *arg)
{
	struct hidden_call *hc = arg;
	struct call *call = hc->call;

	switch (hc->state) {
		case HIDDEN_SEND:
			hc->state = HIDDEN_CLOSE;
			call_send_code(hc->call, hc->code);
			tmr_start(&hc->tmr, 20, proc_hidden_call, hc);
			break;
		case HIDDEN_CLOSE:
			if (audio_txtelev_empty(call_audio(call))) {
				call_hangup(hc->call, 0, NULL);
				mem_deref(hc);
			}
			else {
				tmr_start(&hc->tmr, 20, proc_hidden_call, hc);
			}
			break;
		default:
			break;
	}
}


static struct hidden_call *call_hidden_find(struct call *call)
{
	struct le *le = list_apply(&hcalls, true, find_hidden_call,
				   call);
	if (le)
		return le->data;

	return NULL;
}


static int cmd_dtmf(struct re_printf *pf, void *arg)
{
	const struct cmd_arg *carg = arg;
	struct pl to   = PL_INIT;
	struct pl code = PL_INIT;
	struct call *call;
	struct mbuf mb;
	char *buf;
	int err;
	const char *usage = "usage: icdtmf <address/number> <dtmfcode>\n";

	mbuf_init(&mb);
	err = re_regex(carg->prm, str_len(carg->prm),
		       "[^ ]* [^ ]*",
		       &to, &code);

	if (err) {
		warning("intercom: could not parse %s\n", carg->prm);
		re_hprintf(pf, usage);
		return EINVAL;
	}

	mbuf_printf(&mb, "%r audio=on video=off", &to);
	mbuf_set_pos(&mb, 0);
	mbuf_strdup(&mb, &buf, mbuf_get_left(&mb));
	mbuf_reset(&mb);

	if (!buf)
		return ENOMEM;

	err = common_icdial(pf, "icdtmf", SDP_SENDONLY, buf, "hidden",
			    &call);
	if (err)
		goto out;

	err = hidden_call_append(call, &code);
out:
	mem_deref(buf);
	return err;
}


static const struct cmd cmdv[] = {

{"icdtmf",      0, CMD_PRM, "Intercom send DTMF via hidden call",    cmd_dtmf},

};


int call_hidden_start(struct call *call)
{
	struct hidden_call *hc = call_hidden_find(call);
	if (!hc)
		return EINVAL;

	if (hc->state != HIDDEN_ESTABLISHED)
		return EINVAL;

	hc->state = HIDDEN_SEND;
	tmr_start(&hc->tmr, 20, proc_hidden_call, hc);
	return 0;
}


void call_hidden_close(struct call *call)
{
	struct hidden_call *hc = call_hidden_find(call);
	if (!hc)
		return;

	mem_deref(hc);
}


int ichidden_init(void)
{
	return cmd_register(baresip_commands(), cmdv, ARRAY_SIZE(cmdv));
}


void ichidden_close(void)
{
	cmd_unregister(baresip_commands(), cmdv);
}
