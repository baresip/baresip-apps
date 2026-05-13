/**
 * @file carddav/carddav.c  CardDAV contacts plugin
 *
 *  CardDAV contacts for Baresip, pulls contacts from given CardDAV
 *
 * Copyright (C) 2026 - Joe Burmeister
 */

#include <stddef.h>
#include <stdio.h>
#include <curl/curl.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>

#include <re.h>
#include <baresip.h>


/**
 * @defgroup carddav carddav
 *
 * App module add carddav commands using libcurl
 *
 * This module adds command to refresh contacts from a "(CardDAV)".
 *
 * Example config:
 \verbatim
  carddav_gateway      sip.example.co.uk
  carddav_user         myusername:mypassword
  carddav_url          https://my.nextcloud.org/remote.php/dav/addressbooks/\
users/myuser/myshareuuid/
  carddav_buf          524288
  carddav_at_boot      true
  carddav_extras       1
  carddav_1_user       myusername:mypassword
  carddav_1_url        https://my.nextcloud2.org/remote.php/dav/addressbooks/\

 \endverbatim
 */


#define PTRDIFF(a,b) ((uintptr_t)a - (uintptr_t)b)
#define WILD "!-\\~ "
#define CARDDAV "(CardDAV)"


struct carddav
{
	struct contacts *contacts;
	uint32_t buf_len;
	uint32_t buf_used;
	char *buf_a;
	char *buf_b;
	char *gateway;
	const char *user;
	const char *url;
	unsigned count;

	struct mbuf *mb;
	const char *bpos;
	const char *epos;
	const char *vcard;
};


static int process_name_param(const struct pl *name_param,
			      struct pl *name, struct pl *params)
{
	if (!pl_isset(name_param))
		return EINVAL;

	/* parameter present */
	int err = re_regex(name_param->p, name_param->l, "[~;]*;[~:]*",
			           name, params);
	if (err)
		*name = *name_param;

	return 0;
}


static int process_line(struct carddav *d, const struct pl *line)
{
	struct pl group      = PL_INIT;
	struct pl name_param = PL_INIT;
	struct pl name       = PL_INIT;
	struct pl params     = PL_INIT;
	struct pl value      = PL_INIT;
	(void)d;
	int err;

	/* group present */
	err = re_regex(line->p, line->l, "[~\\.]+\\.[~:]*:[~]*",
		           &group, &name_param, &value);
	if (err) {
		/* without group */
		group = pl_null;
		err = re_regex(line->p, line->l, "[~:]*:[~]*",
			           &name_param, &value);
	}
	if (err) {
		warning("invalid contentline: %r\n", line);
		return EINVAL;
	}

	err = process_name_param(&name_param, &name, &params);
	if (err)
		return err;

	/* Process decoded data  (replace the re_printf here). If it is needed,
	 * store in carddav object.
	   - Ignore everything outside BEGIN:VCARD and END:VCARD!
	   - Check that VERSION follows BEGIN:VCARD!
	   - Add a contact if found a SIP line.
	 */
	re_printf("group:  %r\n", &group);
	re_printf("name:   %r\n", &name);
	re_printf("params: %r\n", &params);
	re_printf("value:  %r\n", &value);
	return 0;
}


static int process_vcard(struct carddav *d, const struct pl *card)
{
	if (!pl_isset(card))
		return EINVAL;

	const char *pos = card->p;
	const char *end = card->p + card->l;

	while (pos < end) {
		struct pl tail = {pos, end - pos};
		const char *lineend = pl_strstr(&tail, "\r\n");
		if (!lineend)
			break;

		struct pl line = {pos, lineend - pos};
		int err;
		err = process_line(d, &line);
		if (err)
			return err;

		pos = line.p + line.l + 2;
	}

	return 0;
}


static size_t writefunc(const void *ptr,
                        size_t size,
                        size_t nmemb,
                        void *userdata)
{
	struct carddav *d = userdata;
	size_t total = size*nmemb;
	int err;

	debug("carddav: Chunk of %zu\n", total);
	const char *mbend = (const char *) mbuf_buf(d->mb);
	const char *begin = "BEGIN:VCARD\r\n";
	const char *end   = "END:VCARD\r\n";

	if (!d->bpos) {
		d->bpos = mbend;
		d->epos = mbend;
	}

	err = mbuf_write_mem(d->mb, ptr, total);
	if (err)
		return 0;

	/* 1. TODO unfold: remove "/r/n " (CR LF SPACE)
	 *  start at (mbend - 3)                       */

	/* 2. look for BEGIN:VCARD, suggested like here */

	mbend = (const char *) mbuf_buf(d->mb);
	struct pl bcheck  = {d->bpos, mbend - d->bpos};
	struct pl echeck  = {d->epos, mbend - d->epos};
	const char *pos = NULL;
	if (!d->vcard && (pos = pl_strstr(&bcheck, begin))) {
		debug("carddav: Found card start.\n");
		pos += strlen(begin);
		d->vcard = pos;
		d->epos  = pos;
	}
	else if (d->vcard && (pos = pl_strstr(&echeck, end))) {
		/* 3. look for END:VCARD and rewind mbuf to save memory */
		debug("carddav: Found card end.\n");
		char *tail = NULL;

		pos += strlen(end);
		size_t tail_len = mbend - pos;
		if (tail_len > 0) {
			mbuf_set_pos(d->mb, mbuf_pos(d->mb) - tail_len);
			err = mbuf_strdup(d->mb, &tail, tail_len);
			if (err)
				return 0;
		}

		struct pl card = {d->vcard, d->vcard - pos};
		err = process_vcard(d, &card);
		mbuf_rewind(d->mb);
		d->vcard = NULL;
		d->bpos  = NULL;

		if (tail) {
			mbuf_write_str(d->mb, tail);
			mem_deref(tail);
		}
	}

	return total;
}


static void move_contacts(struct list *contacts_a,
                          struct list *contacts_b,
                          struct contacts *contacts)
{
	info("carddav: wipe existing contacts.\n");
	struct le *cur = list_head(contacts_a);

	while (cur) {
		struct le *next = cur->next;
		mem_ref(cur->data);
		contact_remove(contacts, cur->data);
		if (contacts_b)
			list_prepend(contacts_b, cur, cur->data);
		cur = next;
	}
}


static int carddav_sync_instance(struct carddav *context)
{
	info("carddav: using URL: %s\n", context->url);
	info("carddav: using user: %s\n", context->user);
	info("carddav: using gateway: %s\n", context->gateway);

	CURL *curl = curl_easy_init();
	if (!curl) {
		warning("carddav: curl easy init failed.\n");
		return EINVAL;
	}

	curl_easy_setopt(curl, CURLOPT_URL, context->url);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_USERPWD, context->user);

	struct curl_slist *hs;
	hs = curl_slist_append(NULL, "Content-Type: text/xml");

	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hs);

	curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PROPFIND");

	curl_easy_setopt(curl, CURLOPT_POSTFIELDS,
		         "<propfind xmlns='DAV:'>"
		         "<prop>"
		         "<address-data "
		         "xmlns=\"urn:ietf:params:xml:ns:carddav\"/>"
		         "</prop>"
		         "</propfind>");

	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writefunc);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, context);

	CURLcode result = curl_easy_perform(curl);
	curl_easy_cleanup(curl);

	if (result == CURLE_OK) {
		info("carddav: Added %u contacts.\n", context->count);
		debug("carddav: CardDAV pull complete.\n");
	}
	else
		warning("carddav: CardDAV pull failed: %s\n",
		        curl_easy_strerror(result));
	return (int)result;
}


static int carddav_sync(void)
{
	char user[1024] = {0};
	char url[4096] = {0};
	char gateway[256] = {0};

	struct carddav context = {0};

	if (conf_get_str(conf_cur(), "carddav_gateway",
	                 gateway, sizeof(gateway)) ||
	    conf_get_str(conf_cur(), "carddav_user",
	                 user, sizeof(user)) ||
	    conf_get_str(conf_cur(), "carddav_url",
	                 url, sizeof(url))) {
		warning("carddav: Miss⅞ing config.\n");
		return EINVAL;
	}

	conf_get_u32(conf_cur(), "carddav_buf", &context.buf_len);
	if (!context.buf_used)
		context.buf_len = 1024 * 128;

	info("carddav: using buffer of: %u\n", context.buf_len);
	context.contacts = baresip_contacts();
	context.gateway = gateway;
	context.url = url;
	context.user = user;

	CURLcode result = curl_global_init(CURL_GLOBAL_ALL);
	if (result != CURLE_OK)
		return (int)result;

	int e = 0;

	context.buf_a = mem_zalloc(context.buf_len, NULL);
	if (!context.buf_a) {
		warning("carddav: Unable to allocate carddav buffer A.\n");
		e = ENOMEM;
		goto cleanup;
	}
	context.buf_b = mem_zalloc(context.buf_len, NULL);
	if (!context.buf_b) {
		warning("carddav: Unable to allocate carddav buffer B.\n");
		mem_deref(context.buf_a);
		e = ENOMEM;
		goto cleanup;
	}

	struct list *contacts_list = contact_list(context.contacts);
	struct list contacts_list_org;
	list_init(&contacts_list_org);
	move_contacts(contacts_list,
	              &contacts_list_org,
	              context.contacts);

	e = carddav_sync_instance(&context);
	if (e)
		goto bad;

	uint32_t extras = 0;

	if (!conf_get_u32(conf_cur(), "carddav_extras", &extras))
		info("carddav: Loading %"PRIu32" extra CardDAVs.\n", extras);

	for (uint32_t n = 1; n <= extras; n++) {

		context.count = 0;
		context.buf_used = 0;

		debug("carddav: Loading extra %"PRIu32"\n", n);
		re_snprintf(context.buf_a, context.buf_len,
		            "carddav_%"PRIu32"_url", n);

		if (conf_get_str(conf_cur(), context.buf_a,
	                         url, sizeof(url))) {
			warning("carddav: Failed to get %s\n", context.buf_a);
			continue;
		}

		re_snprintf(context.buf_a, context.buf_len,
		            "carddav_%"PRIu32"_user", n);

		if (conf_get_str(conf_cur(), context.buf_a,
	                         user, sizeof(user))) {
			warning("carddav: Failed to get %s\n", context.buf_a);
			continue;
		}

		carddav_sync_instance(&context);
	}

	list_flush(&contacts_list_org);
	goto cleanup;

bad:
	move_contacts(contacts_list, NULL, context.contacts);
	list_flush(&contacts_list_org);

cleanup:
	curl_global_cleanup();

	if (context.buf_a)
		mem_deref(context.buf_a);
	if (context.buf_b)
		mem_deref(context.buf_b);

	return e;
}


static int cmd_sync(struct re_printf *pf, void *arg) {
	(void)pf;
	(void)arg;

	return carddav_sync();
}


static const struct cmd cmdv[] = {
	{"refreshcontacts", 0, 0, "Refresh contacts from carddav", cmd_sync},
};


static int module_init(void)
{
	int err = cmd_register(baresip_commands(), cmdv, RE_ARRAY_SIZE(cmdv));

	if (err)
		return err;

	bool at_boot=false;
	conf_get_bool(conf_cur(), "carddav_at_boot", &at_boot);
	if (at_boot)
		carddav_sync();

	return err;
}


static int module_close(void)
{
	cmd_unregister(baresip_commands(), cmdv);
	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(carddav) = {
	"carddav",
	"application",
	module_init,
	module_close
};
