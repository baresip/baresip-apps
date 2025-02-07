/**
 * @file parcall.c  Parallel call module
 *
 * Copyright (C) 2023 Commend.com - c.spielberger@commend.com
 */
#include <string.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>


/**
 * @defgroup parcall parcall
 *
 * Parallel call module
 *
 * Parallel call groups can be defined with multiple call targets. If a
 * parallel call is started outgoing calls to all the targets in the given
 * group are initiated.
 * When the first call is answered by the peer, all other calls in the group
 * are terminated.
 *
 *
 * The following commands are available:
 \verbatim
 /mkpar <name>                   create a parallel call group with given name
 /rmpar <name>                   remove a parallel call group
 /paradd <name> <SIP address>    add a call target to a parallel group
 /parcall <name>                 initiate a parallel call of given group
 /pardebug                       print parallel call data
 \endverbatim
 */

/** Parallel call module data  */
static struct {
	struct hash *pargroups;
	struct hash *parcalls;
} d;

struct pargroup {
	struct le hle;

	char *name;
	struct list peers;       /**< List of parallel call peers          */
};

struct parpeer {
	struct le le;

	struct ua *ua;
	char *addr;
	const struct pargroup *group;
};

struct parcall {
	struct le hle;

	struct call *call;
	const struct pargroup *group;
	struct tmr tmr;
};

struct callarg {
	struct re_printf *pf;
	enum sdp_dir adir;
	enum sdp_dir vdir;
};


static void pargroup_destructor(void *arg)
{
	struct pargroup *g = arg;

	mem_deref(g->name);
	list_flush(&g->peers);
	hash_unlink(&g->hle);
}


static void parpeer_destructor(void *arg)
{
	struct parpeer *p = arg;

	list_unlink(&p->le);
	mem_deref(p->addr);
}


static void parcall_destructor(void *arg)
{
	struct parcall *c = arg;

	tmr_cancel(&c->tmr);
	hash_unlink(&c->hle);
}


static bool pargroup_search(struct le *le, void *arg)
{
	struct pargroup *g = le->data;
	const struct pl *name = arg;

	return 0 == pl_strcmp(name, g->name);
}


static bool parpeer_debug(struct le *le, void *arg)
{
	struct parpeer *peer = le->data;
	struct re_printf *pf = arg;

	(void)re_hprintf(pf, "  peer: %s\n", peer->addr);
	return false;
}


static bool pargroup_debug(struct le *le, void *arg)
{
	struct pargroup *g = le->data;
	struct re_printf *pf = arg;

	(void)re_hprintf(pf, "Group: %s\n", g->name);
	list_apply(&g->peers, true, parpeer_debug, pf);

	return false;
}


static bool pargroup_hangup(struct le *le, void *arg)
{
	struct parcall *c   = le->data;
	struct pargroup *g0 = arg;

	if (c->group != g0)
		return false;

	struct call *call = c->call;
	(void)ua_hangup(call_get_ua(call), call, 0, NULL);

	return false;
}


static bool parcall_hangup(struct le *le, void *arg)
{
	struct parcall *c  = le->data;
	struct parcall *c0 = arg;
	struct call *call;

	if (c->group != c0->group)
		return false;

	call = c->call;
	if (call != c0->call)
		(void)call_hangup(call, 0, NULL);

	return false;
}


static void cleanup_parcall(void *arg)
{
	struct parcall *c = arg;
	mem_ref(c);
	bevent_call_emit(BEVENT_CALL_CLOSED, c->call, "Rejected locally");
	mem_deref(c->call);
	mem_deref(c);
}


static bool parcall_cleanup(struct le *le, void *arg)
{
	struct parcall *c  = le->data;
	struct parcall *c0 = arg;
	struct call *call;

	if (c->group != c0->group)
		return false;

	call = c->call;
	if (call != c0->call)
		tmr_start(&c->tmr, 0, cleanup_parcall, c);

	return false;
}


static bool parcall_debug(struct le *le, void *arg)
{
	struct parcall *c = le->data;
	const struct pargroup *g = c->group;
	const struct call *call  = c->call;

	struct re_printf *pf = arg;

	(void)re_hprintf(pf, "  %s group %s peer %s\n", call_id(call),
			 g->name, call_peeruri(call));

	return false;
}


static bool parcall_find(struct le *le, void *arg)
{
	struct parcall *c = le->data;
	const struct call *call = arg;

	if (c->call == call)
		return true;

	return false;
}


static struct parcall *find_parcall(const struct call *call)
{
	struct le *le;

	le = hash_lookup(d.parcalls, hash_fast_str(call_id(call)),
			 parcall_find, (void *) call);
	return le ? le->data : NULL;
}


static void event_handler(enum bevent_ev ev, struct bevent *event, void *arg)
{
	struct call *call = bevent_get_call(event);

	(void)arg;

	switch (ev) {
	case BEVENT_CALL_ESTABLISHED:
	{
		struct parcall *pc = find_parcall(call);
		if (!pc)
			break;

		hash_apply(d.parcalls, parcall_hangup, pc);
		hash_apply(d.parcalls, parcall_cleanup, pc);
	}

	break;
	case BEVENT_CALL_CLOSED:
	{
		struct parcall *pc = find_parcall(call);
		if (!pc)
			break;

		mem_deref(pc);
	}
	break;
	default:
		break;
	}
}


/*
 * Create a parallel call group with given name
 *
 * @param pf   Print handler
 * @param arg  Command arguments (carg)
 *             carg->prm holds the name of the group
 *
 * @return 0 if success, otherwise errorcode
 */
static int cmd_mkpar(struct re_printf *pf, void *arg)
{
	struct cmd_arg *carg = arg;
	struct pargroup *g;
	struct pl name;
	int err;

	const char *usage = "usage: /mkpar <name>\n";

	if (!str_isset(carg->prm)) {
		(void)re_hprintf(pf, usage);
		return EINVAL;
	}

	pl_set_str(&name, carg->prm);
	if (hash_lookup(d.pargroups, hash_fast_str(carg->prm), pargroup_search,
			&name)) {
		(void)re_hprintf(pf, "mkpar: call group %r already exists\n",
				 &name);
		return EINVAL;
	}

	g = mem_zalloc(sizeof(*g), pargroup_destructor);
	if (!g)
		return ENOMEM;

	err = str_dup(&g->name, carg->prm);
	if (err) {
		mem_deref(g);
		return err;
	}

	hash_append(d.pargroups, hash_fast_str(g->name), &g->hle, g);
	return 0;
}


static struct pargroup *find_pargroup(struct re_printf *pf, struct pl *name,
			       const char *cmd)
{
	struct le *le;

	le = hash_lookup(d.pargroups, hash_fast(name->p, name->l),
			 pargroup_search, name);
	if (!le) {
		(void)re_hprintf(pf, "%s: call group %r does not exist\n",
				 cmd, name);
		return NULL;
	}

	return le->data;
}


static bool parpeer_find(struct le *le, void *arg)
{
	struct parpeer *peer = le->data;

	return !str_cmp(peer->addr, arg);
}


static bool parpeer_call(struct le *le, void *arg)
{
	struct parpeer *peer = le->data;
	struct callarg *callarg = arg;
	struct call *call;
	struct parcall *c;
	int err;

	err = ua_connect_dir(peer->ua, &call, NULL, peer->addr, VIDMODE_ON,
			     callarg->adir, callarg->vdir);
	if (err)
		return false;

	re_hprintf(callarg->pf, "parallel call uri: %s id: %s "
		   "audio=%s video=%s\n",
		   peer->addr, call_id(call),
		   sdp_dir_name(callarg->adir), sdp_dir_name(callarg->vdir));

	c = mem_zalloc(sizeof(*c), parcall_destructor);
	if (!c)
		return true;

	c->call  = call;
	c->group = peer->group;
	hash_append(d.parcalls, hash_fast_str(call_id(call)), &c->hle, c);
	return false;
}


/*
 * Remove the parallel call group with given name
 *
 * @param pf   Print handler
 * @param arg  Command arguments (carg)
 *             carg->prm holds the name of the group
 *
 * @return 0 if success, otherwise errorcode
 */
static int cmd_rmpar(struct re_printf *pf, void *arg)
{
	struct cmd_arg *carg = arg;
	struct pargroup *g;
	struct pl name;
	const char *usage = "usage: /rmpar <name>\n";

	if (!str_isset(carg->prm)) {
		(void)re_hprintf(pf, usage);
		return EINVAL;
	}

	pl_set_str(&name, carg->prm);
	g = find_pargroup(pf, &name, "rmpar");
	if (g)
		mem_deref(g);

	return 0;
}


/**
 * Clear list of parallel call groups
 *
 * @param pf   Print handler
 * @param arg  not used
 *
 * @return 0 if success, otherwise errorcode
 */
static int cmd_clrpar(struct re_printf *pf, void *arg)
{
	(void)arg;

	hash_flush(d.pargroups);
	(void)re_hprintf(pf, "parcall: cleared parallel call groups\n");

	return 0;
}


/**
 * Add a parallel call target to a group
 *
 * @param pf   Print handler
 * @param arg  Command arguments (carg)
 *             carg->prm holds: <name> <SIP address>
 *
 * @return 0 if success, otherwise errorcode
 */
static int cmd_paradd(struct re_printf *pf, void *arg)
{
	struct cmd_arg *carg = arg;
	struct pl name, addr;
	char *addrstr;
	struct pl dname = PL_INIT;
	struct pargroup *g;
	struct parpeer *peer;
	struct ua *ua;
	int err;
	const char *usage = "usage: /paradd <name> <URI>\n"
			    "       /paradd <name> <display name> <sip:uri>\n";

	/* full form with display name */
	err = re_regex(carg->prm, str_len(carg->prm),
		"[^ ]+ [~ \t\r\n<]*[ \t\r\n]*<[^>]+>[ \t\r\n]*",
		&name, &dname, NULL, &addr);
	if (err) {
		dname = pl_null;
		err = re_regex(carg->prm, str_len(carg->prm), "[^ ]+ [^ ]+",
			       &name, &addr);
	}

	if (err) {
		(void)re_hprintf(pf, usage);
		return err;
	}

	if (!pl_isset(&name) || !pl_isset(&addr)) {
		(void)re_hprintf(pf, usage);
		return EINVAL;
	}

	g = find_pargroup(pf, &name, "paradd");
	if (!g)
		return EINVAL;

	ua = uag_find_requri_pl(&addr);
	if (!ua) {
		(void)re_hprintf(pf, "paradd: could not find UA for %r\n",
				 &addr);
		return EINVAL;
	}

	if (pl_isset(&dname)) {
		err = re_sdprintf(&addrstr, "\"%r\" <%r>", &dname, &addr);
	}
	else {
		err = account_uri_complete_strdup(ua_account(ua), &addrstr,
						  &addr);
	}

	if (err)
		goto out;

	if (list_apply(&g->peers, true, parpeer_find, addrstr)) {
		(void)re_hprintf(pf, "paradd: %s already a target of %r\n",
				 addrstr, &name);
		err = EINVAL;
		goto out;
	}

	peer = mem_zalloc(sizeof(*peer), parpeer_destructor);
	if (!peer) {
		err = ENOMEM;
		goto out;
	}

	peer->ua    = ua;
	peer->group = g;
	peer->addr  = addrstr;
	list_append(&g->peers, &peer->le, peer);

out:
	if (err)
		mem_deref(addrstr);

	return err;
}


/**
 * Initiate a parallel call to the group given by its name
 *
 * @param pf   Print handler
 * @param arg  Command arguments (carg)
 *             carg->prm holds: <name>
 *
 * @return 0 if success, otherwise errorcode
 */
static int cmd_parcall(struct re_printf *pf, void *arg)
{
	struct cmd_arg *carg = arg;
	struct pargroup *g;
	struct pl name;
	struct pl pldir[2] = {PL_INIT, PL_INIT};
	struct callarg callarg = {
		.pf = pf,
		.adir = SDP_SENDRECV,
		.vdir = SDP_SENDRECV,
	};

	const char *usage = "usage: /parcall <name>\n"
			"/parcall <name>"
			" audio=<inactive, sendonly, recvonly, sendrecv>"
			" video=<inactive, sendonly, recvonly, sendrecv>\n"
			"/parcall <name>"
			" <sendonly, recvonly, sendrecv>\n"
			"Audio & video must not be"
			" inactive at the same time\n";

	if (!str_isset(carg->prm)) {
		(void)re_hprintf(pf, usage);
		return EINVAL;
	}

	int err = re_regex(carg->prm, str_len(carg->prm),
			   "[^ ]+ audio=[^ ]* video=[^ ]*",
			   &name, &pldir[0], &pldir[1]);
	if (err) {
		err = re_regex(carg->prm, str_len(carg->prm),
			       "[^ ]* [^ ]*",
			       &name, &pldir[0]);
	}

	if (err)
		pl_set_str(&name, carg->prm);

	g = find_pargroup(pf, &name, "parcall");
	if (!g)
		return EINVAL;

	if (!pl_isset(&pldir[1]))
		pldir[1] = pldir[0];

	callarg.adir = sdp_dir_decode(&pldir[0]);
	callarg.vdir = sdp_dir_decode(&pldir[1]);
	if (callarg.adir == SDP_INACTIVE && callarg.vdir == SDP_INACTIVE) {
		(void)re_hprintf(pf, usage);
		return EINVAL;
	}

	(void)list_apply(&g->peers, true, parpeer_call, &callarg);
	return 0;
}


/**
 * Hangup parallel call group with given name. All calls in the group are
 * terminated
 *
 * @param pf   Print handler
 * @param arg  Command arguments (carg)
 *             carg->prm holds: <name>
 *
 * @return 0 if success, otherwise errorcode
 */
static int cmd_parhangup(struct re_printf *pf, void *arg)
{
	struct cmd_arg *carg = arg;
	struct pargroup *g;
	struct pl name;

	const char *usage = "usage: /parhangup <name>\n";

	if (!str_isset(carg->prm)) {
		(void)re_hprintf(pf, usage);
		return EINVAL;
	}

	pl_set_str(&name, carg->prm);
	g = find_pargroup(pf, &name, "parhangup");
	if (!g)
		return EINVAL;

	hash_apply(d.parcalls, pargroup_hangup, g);
	return 0;
}


/**
 * Debug output of parallel call groups and parallel calls
 *
 * @param pf   Print handler
 * @param arg  not used
 *
 * @return 0 if success, otherwise errorcode
 */
static int cmd_pardebug(struct re_printf *pf, void *arg)
{
	(void)arg;

	(void)re_hprintf(pf, "Parallel call groups\n");
	(void)hash_apply(d.pargroups, pargroup_debug, pf);
	(void)re_hprintf(pf, "\n");

	(void)re_hprintf(pf, "Active calls\n");
	(void)hash_apply(d.parcalls,  parcall_debug, pf);
	(void)re_hprintf(pf, "\n");
	return 0;
}


static const struct cmd cmdv[] = {
	{"mkpar",    0,CMD_PRM, "Create parallel call group",    cmd_mkpar   },
	{"rmpar",    0,CMD_PRM, "Remove parallel call group",    cmd_rmpar   },
	{"clrpar",   0,      0, "Clear parallel call groups",    cmd_clrpar  },
	{"paradd",   0,CMD_PRM, "Add a call target to a group",  cmd_paradd  },
	{"parcall",  0,CMD_PRM, "Initiate parallel call to given group",
								 cmd_parcall },
	{"parhangup",0,CMD_PRM, "Hangup parallel call group",   cmd_parhangup},
	{"pardebug", 0,      0, "Print parallel call data",	 cmd_pardebug},
};


static int module_init(void)
{
	int err;

	memset(&d, 0, sizeof(d));
	err  = hash_alloc(&d.pargroups, 32);
	err |= hash_alloc(&d.parcalls,  32);
	if (err)
		return err;

	err  = bevent_register(event_handler, NULL);
	err |= cmd_register(baresip_commands(), cmdv, RE_ARRAY_SIZE(cmdv));
	if (err)
		return err;

	info("parcall: module loaded\n");
	return 0;
}


static int module_close(void)
{
	bevent_unregister(event_handler);
	cmd_unregister(baresip_commands(), cmdv);
	hash_flush(d.pargroups);
	hash_flush(d.parcalls);
	d.pargroups = mem_deref(d.pargroups);
	d.parcalls  = mem_deref(d.parcalls);
	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(parcall) = {
	"parcall",
	"application",
	module_init,
	module_close,
};
