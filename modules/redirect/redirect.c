/**
 * @file redirect.c redirect incoming calls
 *
 * Copyright (C) 2024 Commend.com - c.spielberger@commend.com
 */
#include <stdint.h>
#include <string.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>


/** Parallel call module data  */
static struct {
	struct list redirs;
} d = { LIST_INIT };

struct redirect {
	struct le le;

	struct ua *ua;
	uint16_t scode;
	char *reason;
	struct tmr tmr;
	char *contact;
	char *divparams;
};

struct redir_params {
	struct pl scode;
	struct pl reason;
	uint32_t expires;

	struct pl contact;
	struct pl divparams;
};


static void redirect_destructor(void *arg)
{
	struct redirect *r = arg;

	tmr_cancel(&r->tmr);
	mem_deref(r->reason);
	mem_deref(r->contact);
	mem_deref(r->divparams);
}


static bool redirect_search(struct le *le, void *arg)
{
	struct redirect *r = le->data;
	struct ua *ua = arg;

	return r->ua == ua;
}


static bool redirect_debug(struct le *le, void *arg)
{
	struct redirect *r = le->data;
	struct re_printf *pf = arg;

	(void)re_hprintf(pf, "%s %u %s expires in %lu [ms]",
		  account_aor(ua_account(r->ua)), r->scode, r->reason,
		  tmr_get_expire(&r->tmr));
	if (str_isset(r->contact))
		(void)re_hprintf(pf, " --> %s", r->contact);

	(void)re_hprintf(pf, "\n");
	return false;
}


static void redirect_expired(void *arg)
{
	struct redirect *r  = arg;

	list_unlink(&r->le);
	mem_deref(r);
}


static int expires_alloc(char **bufp, uint32_t expires)
{
	char *buf;
	int err;

	if (expires)
		err = re_sdprintf(&buf, ";expires=%u", expires);
	else
		err = re_sdprintf(&buf, "");

	if (!err)
		*bufp = buf;

	return err;
}


static void event_handler(enum ua_event ev, struct bevent *event, void *arg)
{
	struct le *le;
	const struct sip_msg *msg = bevent_get_msg(event);
	struct ua *ua = uag_find_msg(msg);

	(void)arg;

	switch (ev) {
	case UA_EVENT_SIPSESS_CONN:
	{
		le = list_apply(&d.redirs, true, redirect_search, ua);
		if (!le)
			break;

		struct redirect *r = le->data;
		char *expstr = NULL;
		int err = expires_alloc(&expstr,
			  (uint32_t) tmr_get_expire(&r->tmr));
		if (err)
			return;

		(void)sip_treplyf(NULL, NULL, uag_sip(), msg, false,
				  r->scode, r->reason,
				  "Contact: <%s>%s\r\n"
				  "Diversion: <%s>%s\r\n"
				  "Content-Length: 0\r\n\r\n",
				  r->contact,
				  expstr,
				  account_aor(ua_account(ua)),
				  r->divparams);
		mem_deref(expstr);
		bevent_stop(event);
	}

	break;
	default:
		break;
	}
}


static struct ua *carg_get_ua(const struct cmd_arg *carg)
{
	struct pl pl;
	int err;

	err = re_regex(carg->prm, str_len(carg->prm), "[^ ]+", &pl);
	if (err)
		return NULL;

	uint32_t i = pl_u32(&pl);
	struct le *le = uag_list()->head;
	while (le && i--)
		le = le->next;

	return le ? le->data : NULL;
}


static void ua_redir_clear(struct ua *ua)
{
	struct le *le;

	le = list_apply(&d.redirs, true, redirect_search, ua);
	if (!le)
		return;

	struct redirect *r = le->data;
	list_unlink(&r->le);
	mem_deref(r);
}


static void redirect_parse(struct redir_params *params,
			  const struct cmd_arg *carg)
{
	struct pl pl = PL_INIT;
	struct pl v = PL_INIT;
	pl_set_str(&pl, carg->prm);

	fmt_param_sep_get(&pl, "scode",   ' ', &params->scode);
	fmt_param_sep_get(&pl, "reason",  ' ', &params->reason);
	fmt_param_sep_get(&pl, "contact", ' ', &params->contact);
	fmt_param_sep_get(&pl, "params",  ' ', &params->divparams);
	fmt_param_sep_get(&pl, "expires", ' ', &v);
	if (pl_isset(&v))
		params->expires = pl_u32(&v);
}


static int cmd_redir_add(struct re_printf *pf, void *arg)
{
	const struct cmd_arg *carg = arg;
	struct ua *ua = carg_get_ua(carg);
	const char *usage = "usage: "
			    "/uaredirect_add <ua-idx> "
			    "[scode=<scode>] "
			    "[reason=<reason>] "
			    "[contact=<target contact>] "
			    "[expires=<expires/s>] "
			    "[params=<diversion params>]\n"
			    "Default: scode=302 reason=\"Moved Temporarily\" "
			    "contact=\"\" params=\"\"\n";

	if (!ua) {
		re_hprintf(pf, usage);
		return EINVAL;
	}

	struct redirect *r;
	int err;

	ua_redir_clear(ua);
	r = mem_zalloc(sizeof(*r), redirect_destructor);
	r->ua = ua;
	tmr_init(&r->tmr);

	struct redir_params params = { 0 };
	redirect_parse(&params, carg);
	r->scode = pl_isset(&params.scode) ? pl_u32(&params.scode) : 302;

	if (pl_isset(&params.reason))
		err = pl_strdup(&r->reason, &params.reason);
	else
		err = str_dup(&r->reason, "Moved Temporarily");

	if (pl_isset(&params.contact))
		err = pl_strdup(&r->contact, &params.contact);

	if (params.expires)
		tmr_start(&r->tmr, params.expires*1000, redirect_expired, r);

	if (pl_isset(&params.divparams))
		err = pl_strdup(&r->divparams, &params.divparams);

	list_append(&d.redirs, &r->le, r);
	re_hprintf(pf, "redirect: added redirection\n");
	list_apply(&d.redirs, true, redirect_debug, pf);
	return err;
}


static int cmd_redir_rm(struct re_printf *pf, void *arg)
{
	const struct cmd_arg *carg = arg;
	struct ua *ua = carg_get_ua(carg);
	const char *usage = "usage: "
			    "/uaredirect_rm <ua-idx>\n";
	if (!ua) {
		re_hprintf(pf, usage);
		return EINVAL;
	}

	ua_redir_clear(ua);
	re_hprintf(pf, "redirect: removed redirection of %s\n",
		   account_aor(ua_account(ua)));
	list_apply(&d.redirs, true, redirect_debug, pf);
	return 0;
}


static int cmd_redir_debug(struct re_printf *pf, void *arg)
{
	(void) arg;
	re_hprintf(pf, "redirect: current redirections\n");
	list_apply(&d.redirs, true, redirect_debug, pf);
	return 0;
}


static void find_first_incoming(struct call *call, void *arg)
{
	struct call **pcall = arg;

	if (!pcall)
		return;

	if (call_state(call) != CALL_STATE_INCOMING)
		return;

	if (!*pcall)
		*pcall = call;
}


static struct call *call_cur(void)
{
	struct call *call = NULL;
	uag_filter_calls(find_first_incoming, NULL, &call);

	return call;
}


static struct call *carg_get_call(const struct cmd_arg *carg)
{
	struct pl pl;
	int err;

	err = re_regex(carg->prm, str_len(carg->prm), "[^ ]+", &pl);
	if (err)
		return call_cur();

	char *id;
	pl_strdup(&id, &pl);
	struct call *call = uag_call_find(id);
	if (!call)
		call = call_cur();

	mem_deref(id);
	return call;
}


static int cmd_call_redir(struct re_printf *pf, void *arg)
{
	const struct cmd_arg *carg = arg;
	const char *usage = "usage: "
			    "/callredirect <callid> "
			    "[scode=<scode>] "
			    "[reason=<reason>] "
			    "[contact=<target contact>] "
			    "[expires=<expires/s>] "
			    "[params=<diversion params>]\n"
			    "Default: scode=302 reason=\"Moved Temporarily\" "
			    "contact=\"\" params=\"\"\n";

	if (!str_cmp(carg->prm, "-h")) {
		re_hprintf(pf, usage);
		return EINVAL;
	}

	struct call *call = carg_get_call(carg);
	if (!call) {
		re_hprintf(pf, "redirect: could not find call\n");
		return EINVAL;
	}

	struct ua *ua = call_get_ua(call);
	struct redir_params params = { 0 };
	redirect_parse(&params, carg);
	uint16_t scode = pl_isset(&params.scode) ? pl_u32(&params.scode) : 302;
	char *reason;
	char *expstr = NULL;
	int err;

	if (pl_isset(&params.reason))
		err = pl_strdup(&reason, &params.reason);
	else
		err = str_dup(&reason, "Moved Temporarily");

	err = expires_alloc(&expstr, params.expires);
	if (err)
		return err;

	re_hprintf(pf, "redirect: reject call %s\n", call_id(call));
	struct pl *dp = &params.divparams;
	ua_hangupf(ua, call, scode, reason,
	      "Contact: <%r>%s\r\n"
	      "Diversion: <%s>%s%r\r\n"
	      "Content-Length: 0\r\n\r\n",
	      &params.contact,
	      expstr,
	      account_aor(ua_account(ua)),
	      pl_isset(dp) && *(dp->p)!=';' ? ";" : "",
	      &params.divparams);

	mem_deref(reason);
	mem_deref(expstr);
	return 0;
}


static const struct cmd cmdv[] = {
{"uaredirect_add", 0, CMD_PRM, "Adds call redirection to UA",   cmd_redir_add},
{"uaredirect_clear", 0, CMD_PRM, "Removes redirection from UA", cmd_redir_rm },
{"uaredirect_debug", 0, 0, "Prints redirect list", cmd_redir_debug },
{"call_redirect", 0, CMD_PRM, "Redirects an incoming call", cmd_call_redir },
};


static int module_init(void)
{
	int err;

	err  = bevent_register(event_handler, NULL);
	err |= cmd_register(baresip_commands(), cmdv, RE_ARRAY_SIZE(cmdv));
	if (err)
		return err;

	info("redirect: module loaded\n");
	return 0;
}


static int module_close(void)
{
	bevent_unregister(event_handler);
	cmd_unregister(baresip_commands(), cmdv);
	list_flush(&d.redirs);
	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(redirect) = {
	"redirect",
	"application",
	module_init,
	module_close,
};
