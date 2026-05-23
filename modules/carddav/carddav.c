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


struct carddav_context
{
	struct contacts *contacts;
	uint32_t buf_len;
	struct mbuf *buf_a;
	struct mbuf *buf_b;
	char *gateway;
	const char *user;
	const char *url;
	unsigned count;
};


struct carddav
{
	struct pl fullname;
	double version;
	unsigned contacts;
};


static struct contact *in_contacts(struct contacts *contacts,
                                   const char *uri)
{
	struct pl pl;
	struct sip_addr addr;

	pl_set_str(&pl, uri);

	int err = sip_addr_decode(&addr, &pl);
	if (err)
		return NULL;

	char safe[128] = {0};

	str_ncpy(safe, addr.auri.p, addr.auri.l + 1);

	return contact_find(contacts, safe);
}


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


static int add_addr(struct carddav_context *context,
                    struct carddav *d,
                    char addr[1024])

{
	if (in_contacts(context->contacts, addr)) {
		info("carddav: Duplicate SIP %s\n", addr);
		return 0;
	}

	struct pl pl;
	pl_set_str(&pl, addr);

	info("carddav: Adding %s\n", addr);
	int e = contact_add(context->contacts, NULL, &pl);
	if (!e) {
		context->count++;
		d->contacts++;
	}
	else warning("carddav: Failed to add contact.\n");
	return e;
}


static int add_contract_tel(struct carddav_context *context,
                            struct carddav *d,
                            struct pl *params,
                            struct pl *value)
{
	char addr[1024] = {0};
	struct pl type = {0};

	re_regex(params->p, params->l, "TYPE=[A-Za-z0-9., ]*", &type);

	int len = re_snprintf(addr,
	                      sizeof(addr),
	                      "\"%r%s%r%s "CARDDAV"\" <sip:",
	                      &d->fullname,
	                      (type.l)?" (Tel:":"",
	                      &type,
	                      (type.l)?")":"");

	/* There is an extra @ > null and maybe a 0, so 4*/
	if (len < 0 ||
	    (len + strlen(context->gateway) + value->l + 4) >= sizeof(addr)) {
		warning("carddav: Failed to start tel contact \"%r\"\n",
		        &d->fullname);
		return 0;
	}

	const char * pos = value->p;
	const char * end = pos + value->l;

	/* Plus are used instead of international code 00 often, but
	 * SIP phone numbers are numeric only.
	 */
	if (*pos == '+') {
		addr[len++]='0';
		addr[len++]='0';
		++pos;
		while (*pos == '+')
			++pos;
	}

	/* Removes any seperators*/
	while (pos < end) {
		char c = *pos++;

		if (isdigit(c))
			addr[len++]=c;
		else if (isalpha(c)) {
			warning("carddav: Invalid tel contact \"%r\"\n",
			        &d->fullname);
			return 0;
		}
	}

	int e = re_snprintf(addr+len, sizeof(addr) - len,
	            "@%s>", context->gateway);

	if (e < 0 || addr[len+e-1] != '>') {
		warning("carddav: Failed to complete tel contact \"%r\"\n",
		        &d->fullname);
		return 0;
	}

	return add_addr(context, d, addr);
}


static int add_impp_contact(struct carddav_context *context,
                            struct carddav *d,
                            struct pl *params,
                            struct pl *value)
{
	char addr[1024] = {0};
	struct pl type = {0};

	re_regex(params->p, params->l, "TYPE=[A-Za-z0-9., ]*", &type);

	if (pl_strcasecmp(&type, "sip")) {
		debug("carddav: Non sip impp for \"%r\" : %r\n",
		      &d->fullname, params);
		return 0;
	}

	int len = re_snprintf(addr, sizeof(addr),
	                      "\"%r "CARDDAV"\" <sip:%r>",
	                      &d->fullname,
	                      value);

	if (len < 0) {
		warning("carddav: Failed to make sip contact \"%r\"\n",
		        &d->fullname);
		return 0;
	}

	return add_addr(context, d, addr);
}


static int process_line(struct carddav_context *context,
                        struct carddav *d,
                        const struct pl *line)
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
	if (err) /* Mostly likely base64 lines of image  */
		return 0;

	err = process_name_param(&name_param, &name, &params);
	if (err)
		return err;

	if (!pl_strcmp(&name, "VERSION")) {
		d->version = pl_float(&value);
		if (d->version < 3 || d->version > 4) {
			warning("carddav: version unsupport.\n");
			return EINVAL;
		}
		return 0;
	}
	else if (!d->version) {
		warning("carddav: field \"%r\" before version\n", &name);
		return EINVAL;
	}

	if (!pl_strcmp(&name, "FN")) {
		d->fullname = value;
		return 0;
	}

	if (!pl_strcmp(&name, "TEL"))
		return add_contract_tel(context, d, &params, &value);

	if (!pl_strcmp(&name, "IMPP"))
		return add_impp_contact(context, d, &params, &value);

	return 0;
}


static int process_vcard(struct carddav *d,
                         const struct pl *card,
                         struct carddav_context *context)
{
	if (!pl_isset(card))
		return EINVAL;
	debug("carddav: Found carddav of %zu\n", card->l);

	const char *pos = card->p;
	const char *end = card->p + card->l;

	while (pos < end) {
		struct pl tail = {pos, end - pos};
		const char *lineend = pl_strstr(&tail, "&#13");
		if (!lineend)
			break;
		if (lineend != pos) {
			struct pl line = {pos, lineend - pos};
			int err = process_line(context, d, &line);
			if (err)
				return err;
		}

		pos = lineend + 6;
	}

	return 0;
}


static size_t writefunc(const void *ptr,
                        size_t size,
                        size_t nmemb,
                        void *userdata)
{
	struct carddav_context * context = userdata;
	size_t total = size*nmemb;

	debug("carddav: Chunk of %zu\n", total);

	size_t freebuf = mbuf_get_space(context->buf_a);

	if (freebuf < total) {
		warning("carddav : chunks too big for free buffer %zu\n",
			freebuf);
		return 0;
	}
	int e = mbuf_write_mem(context->buf_a, ptr, total);
	if (e) {
		warning("carddav : buffer write failed %s\n",
		        strerror(e));
		return e;
	}

	char *pos = memmem(context->buf_a->buf,
	                   mbuf_pos(context->buf_a),
	                   "BEGIN:VCARD",
	                   11);

	if (!pos) {
		debug("carddav: No card started after %zu.\n",
		      mbuf_pos(context->buf_a));
		return total;
	}
	pos += 11;

	char *end = (char*)mbuf_buf(context->buf_a);
	char *cardend = memmem(pos,
	                       PTRDIFF(end, pos),
	                       "END:VCARD",
	                       9);

	if (!cardend) {
		debug("carddav: No card complete after %zu.\n",
		      mbuf_pos(context->buf_a));
		return total;
	}

	char * lastpos;

	while (pos && cardend) {
		struct pl card = {.p = pos, .l = PTRDIFF(cardend, pos) };
		struct carddav d = {0};
		process_vcard(&d, &card, context);

		debug("carddav: Loaded %u for %r\n",
		      d.contacts,  &d.fullname);

		lastpos = cardend + 9;

		pos = memmem(lastpos,
		             PTRDIFF(end, lastpos),
		             "BEGIN:VCARD",
		             11);
		if (!pos)
			break;
		pos += 11;

		cardend = memmem(pos,
		                 PTRDIFF(end, pos),
		                 "END:VCARD",
		                  9);
	}

	unsigned remaining = (unsigned)PTRDIFF(end, lastpos);

	debug("carddav: Buffer swap with %u\n", remaining);

	mbuf_set_pos(context->buf_b, 0);
	e = mbuf_write_mem(context->buf_b, (uint8_t*)lastpos, remaining);
	if (e) {
		warning("carddav : buffer swap write failed %s\n",
		        strerror(e));
		return e;
	}

	struct mbuf *t = context->buf_a;
	context->buf_a = context->buf_b;
	context->buf_b = t;
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
		if (contacts_b) {
			mem_ref(cur->data);
			contact_remove(contacts, cur->data);
			list_prepend(contacts_b, cur, cur->data);
		}
		else contact_remove(contacts, cur->data);
		cur = next;
	}
}


static int carddav_sync_instance(struct carddav_context *context)
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


static void extract_name(char name[64], const char *con_str)
{
	const char * pos = con_str + 1;
	const char * end = strchr(pos, '"');
	unsigned len = PTRDIFF(end, pos) + 1;
	str_ncpy(name, pos, len);
}


static void restore_unique(struct carddav_context *context,
                           struct list *contacts_org)
{
	struct contacts *contacts = context->contacts;

	struct le * cur = list_head(contacts_org);

	if (!cur) {
		debug("carddav: No contacts to restore.\n");
		return;
	}

	for (;cur; cur = cur->next) {
		struct contact *con = (struct contact*)cur->data;
		const char *con_str = contact_str(con);
		struct pl pl;
		int e = -1;

		const char *uri = contact_uri(con);

		if (strstr(con_str, CARDDAV))
			continue;

		if (in_contacts(context->contacts, uri)) {
			debug("carddav: Found \"%s\", not adding.\n",
			      uri);
			continue;
		}

		struct contact *dup = contact_find(contacts, uri);
		if (dup)
			continue;

		const char *end = strchr(uri, '@');
		if (!end) {
			warning("carddav: Contact URI %s, no @\n",
			        uri);
			continue;
		}

		const char *pos = uri;
		if (strncmp(pos, "sip:", 4)) {
			warning("carddav: Contact URI %s, no sip:\n",
			        uri);
			continue;
		}

		pos+=4;
		while (end && pos < end) {
			if (!isdigit(*pos)) {
				debug("carddav: Non-number URI %s\n",
				      uri);
				break;
			}
			++pos;
		}

		char name[64] = {0};
		extract_name(name, con_str);
		mbuf_set_pos(context->buf_a, 0);

		if (pos == end) {
			unsigned len = PTRDIFF(end, uri) - 3;
			char newaddr[128];
			char pn[16] = {0};

			str_ncpy(pn, uri+4, len);

			re_snprintf(newaddr, sizeof(newaddr),
			            "sip:%s@%s",
			            pn, context->gateway);

			dup = contact_find(contacts, newaddr);
			if (dup) {
				debug("carddav: Phone number %s"
				      " already on gateway.\n", pn);
				continue;
			}
		}

		info("carddav: Adding back %s\n", con_str);
		pl_set_str(&pl, con_str);

		e = contact_add(contacts, NULL, &pl);
		if (e)
			warning("carddav: Failed to add back %.*s : %s\n",
			        pl.l, pl.p, strerror(e));
	}
}


static int carddav_sync(void)
{
	char user[256] = {0};
	char url[1024] = {0};
	char gateway[512] = {0};

	struct carddav_context context = {0};

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
	if (!context.buf_len)
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

	context.buf_a = mbuf_alloc(context.buf_len);
	if (!context.buf_a) {
		warning("carddav: Unable to allocate carddav buffer A.\n");
		e = ENOMEM;
		goto cleanup;
	}
	context.buf_b = mbuf_alloc(context.buf_len);
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
		char entry[256];
		context.count = 0;

		debug("carddav: Loading extra %"PRIu32"\n", n);

		re_snprintf(entry, sizeof(entry),
		            "carddav_%"PRIu32"_url", n);

		if (conf_get_str(conf_cur(), entry,
	                         url, sizeof(url))) {
			warning("carddav: Failed to get %s\n", entry);
			continue;
		}

		re_snprintf(entry, sizeof(entry),
		            "carddav_%"PRIu32"_user", n);

		if (conf_get_str(conf_cur(), entry,
	                         user, sizeof(user))) {
			warning("carddav: Failed to get %s\n", entry);
			continue;
		}

		carddav_sync_instance(&context);
	}
	/* Restore main carddav for any uploads. */
	conf_get_str(conf_cur(), "carddav_user", user, sizeof(user));
	conf_get_str(conf_cur(), "carddav_url", url, sizeof(url));

	goto cleanup;
bad:
	/* Wipe any contacts added. */
	move_contacts(contacts_list, NULL, context.contacts);

cleanup:
	/* Restore any unique original contacts. (all on error)*/
	restore_unique(&context, &contacts_list_org);
	list_flush(&contacts_list_org);

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
