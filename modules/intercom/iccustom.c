/**
 * @file iccustom.c Custom Intercom call
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
#include "iccustom.h"

struct iccustom {
	struct le le;

	struct pl subject;
	enum sdp_dir dir;
	bool allowed;
	struct pl auffile;
};


static void iccustom_destructor(void *arg)
{
	struct iccustom *d = arg;

	list_unlink(&d->le);
}


static int cmd_custom(struct re_printf *pf, void *arg)
{
	const struct cmd_arg *carg = arg;
	const char *p = carg->prm;
	struct pl subject;
	struct iccustom *c;
	char *sub;
	int err;
	const char *usage = "usage: /iccustom <subject> <address/number>"
			" audio=<on,off>"
			" video=<on,off>\n";


	err = re_regex(p, str_len(p), "[^ ]* [^ ]* audio=[onf]* video=[onf]*",
		       &subject, NULL, NULL, NULL);
	if (err) {
		warning("iccustom: could not parse %s (%m)\n", p, err);
		re_hprintf(pf, usage);
		return EINVAL;
	}

	c = iccustom_find(&subject);
	if (!c) {
		re_hprintf(pf, "iccustom: subject %r not configured\n",
			   &subject);
		return EINVAL;
	}

	err = pl_strdup(&sub, &subject);
	if (err)
		return err;

	err = common_icdial(pf, "iccustom", c->dir, carg->prm, sub, NULL);
	mem_deref(sub);
	return err;
}


static const struct cmd cmdv[] = {

{"iccustom",    0, CMD_PRM, "Intercom custom call",            cmd_custom},

};


int iccustom_handler(const struct pl *pl, void *arg)
{
	struct pl subject, dir, allowed, aufile;
	struct iccustom *c;
	struct hash *hash = arg;
	int err;

	err = re_regex(pl->p, pl->l, "[^,]*,[^,]*,[^,]*,[^,]*",
		       &subject, &dir, &allowed, &aufile);
	if (err)
		return 0;

	c = mem_zalloc(sizeof(*c), iccustom_destructor);
	if (!c)
		return ENOMEM;

	c->subject = subject;
	err = pl_bool(&c->allowed, &allowed);
	c->dir = sdp_dir_decode(&dir);
	c->auffile = aufile;

	info("intercom: add custom %r\n", &subject);
	hash_append(hash, hash_joaat_pl(&subject), &c->le, c);
	return 0;
}


bool iccustom_lookup(struct le *le, void *arg)
{
	struct pl *pl = arg;
	struct iccustom *c = le->data;

	if (pl->l < c->subject.l)
		return false;

	return !strncmp(pl->p, c->subject.p, c->subject.l);
}


bool ic_is_custom(const struct pl *val)
{
	if (!val)
		return false;

	return iccustom_find(val) != NULL;
}


enum sdp_dir iccustom_dir(const struct pl *val)
{
	struct iccustom *c = iccustom_find(val);
	if (!c)
		return SDP_INACTIVE;

	return c->dir;
}


bool iccustom_allowed(const struct pl *val)
{
	struct iccustom *c = iccustom_find(val);
	if (!c)
		return false;

	return c->allowed;
}


struct pl *iccustom_aufile(const struct pl *val)
{
	struct iccustom *c = iccustom_find(val);
	if (!c)
		return NULL;

	return &c->auffile;
}


int iccustom_init(void)
{
	return cmd_register(baresip_commands(), cmdv, RE_ARRAY_SIZE(cmdv));
}


void iccustom_close(void)
{
	cmd_unregister(baresip_commands(), cmdv);
}
