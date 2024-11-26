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
	char *params;
};


static void redirect_destructor(void *arg)
{
	struct redirect *r = arg;

	tmr_cancel(&r->tmr);
	mem_deref(r->reason);
	mem_deref(r->contact);
	mem_deref(r->params);
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
		uint64_t expires = tmr_get_expire(&r->tmr);
		char *expstr = NULL;
		int err = 0;
		if (expires >= 1000) {
			err = re_sdprintf(&expstr, ";expires=%u",
					  expires / 1000);
		}

		if (err)
			return;

		(void)sip_treplyf(NULL, NULL, uag_sip(), msg, false,
				  r->scode, r->reason,
				  "Contact: <%s>%s\r\n"
				  "Diversion: <%s>%s\r\n"
				  "Content-Length: 0\r\n\r\n",
				  r->contact,
				  expstr ? expstr : "",
				  account_aor(ua_account(ua)),
				  r->params);
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


static int cmd_redir_add(struct re_printf *pf, void *arg)
{
	const struct cmd_arg *carg = arg;
	struct ua *ua = carg_get_ua(carg);
	const char *usage = "usage: "
			    "/uaredirect_add <ua-idx> "
			    "[scode=<scode>] "
			    "[reason=<reason>] "
			    "[contact=<target contact>] "
			    "[expires=<expires [s]>] "
			    "[params=<diversion params>]\n"
			    "Default: scode=302 reason=\"Moved Temporarily\" "
			    "contact=\"\" params=\"\"\n";

	if (!ua) {
		re_hprintf(pf, usage);
		return EINVAL;
	}

	struct pl pl;
	struct pl v[6] = {PL_INIT,};
	struct redirect *r;
	int err;

	ua_redir_clear(ua);
	r = mem_zalloc(sizeof(*r), redirect_destructor);
	r->ua = ua;
	tmr_init(&r->tmr);
	pl_set_str(&pl, carg->prm);

	if (fmt_param_sep_get(&pl, "scode", ' ', &v[1]))
		r->scode = pl_u32(&v[1]);
	else
		r->scode = 302;

	if (fmt_param_sep_get(&pl, "reason", ' ', &v[2]))
		err = pl_strdup(&r->reason, &v[2]);
	else
		err = str_dup(&r->reason, "Moved Temporarily");

	if (fmt_param_sep_get(&pl, "contact", ' ', &v[3]))
		err = pl_strdup(&r->contact, &v[3]);

	if (fmt_param_sep_get(&pl, "expires", ' ', &v[4])) {
		uint32_t expires = pl_u32(&v[4]);
		if (expires)
			tmr_start(&r->tmr, expires*1000, redirect_expired, r);
	}

	if (fmt_param_sep_get(&pl, "params", ' ', &v[5]))
		err = pl_strdup(&r->params, &v[5]);

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


static const struct cmd cmdv[] = {
{"uaredirect_add", 0, CMD_PRM, "Adds call redirection to UA",   cmd_redir_add},
{"uaredirect_clear", 0, CMD_PRM, "Removes redirection from UA", cmd_redir_rm },
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
