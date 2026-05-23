/**
 * @file carddav/carddav.c  CardDAV contacts plugin
 *
 *  CardDAV contacts for Baresip, pulls contacts from given CardDAV
 *
 * Copyright (C) 2026 - Joe Burmeister
 */

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
  carddav_upload       true
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
	uint32_t buf_used;
	char *buf_a;
	char *buf_b;
	char *gateway;
	const char *user;
	const char *url;
	unsigned count;
	bool upload;
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


static int add_addr(struct carddav_context *context,
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
	if (!e)
		context->count++;
	else
		warning("carddav: Failed to add contact.\n");
	return e;
}


static void rstrip(struct pl * pl)
{
	/* There may well be escaped carriage return to remove.*/
	const char * cr = pl_strstr(pl, "&#13");
	if (cr)
		pl->l = PTRDIFF(cr, pl->p);

}


static int process_tel_card(const char *cardpos,
                            uintptr_t cardend,
                            struct carddav_context *context,
                            struct pl * name,
                            char * addr, size_t addr_len)
{
	unsigned telcount = 0;
	int e = 0;

	while ((uintptr_t)cardpos < cardend) {
		struct pl telline = {0};
		e = re_regex(cardpos,
		             PTRDIFF(cardend, cardpos),
		             "\nTEL["WILD"]+\n",
		             &telline);

		struct pl pn = {0}, tel = PL("");

		if (e) {
			if (e == ENOENT)
				e = 0;
			goto out;
		}

		rstrip(&telline);

		if (telline.p[0] == ';')
			re_regex(telline.p, telline.l, "TYPE=[a-zA-Z]+", &tel);
		else if (telline.p[0] != ':') {
			/* Not a TEL entry */
			cardpos = telline.p + telline.l;
			continue;
		}
		e = re_regex(telline.p, telline.l, ":[+0-9\\- ]+", &pn);

		if (e) {
			warning("carddav: No number found in TEL entry.\n");
			cardpos = telline.p + telline.l;
			continue;
		}

		int len = re_snprintf(addr,
		                      addr_len,
		                      "\"%r%s%r%s "CARDDAV"\" <sip:",
		                      name,
		                      (tel.l)?" (Tel:":"",
		                      &tel,
		                      (tel.l)?")":"");

		if (len <= 0) {
			warning("carddav: Failed to write out addr.\n");
			cardpos = telline.p + telline.l;
			continue;
		}

		const char * pos = pn.p;
		const char * end = pn.p + pn.l;

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

		while (pos < end) {
			char c = *pos++;

			if (isdigit(c))
				addr[len++]=c;
		}

		re_snprintf(addr+len, addr_len - len,
		            "@%s>", context->gateway);

		e = add_addr(context, addr);
		if (e)
			goto out;

		++telcount;

		cardpos = telline.p + telline.l;
	}

out:
	if (telcount)
		debug("carddav: found %u tel numbers.\n", telcount);
	else
		debug("carddav: No tel numbers.\n");

	return e;
}


static int process_card(const char *cardstart,
                         uintptr_t cardend,
                         struct carddav_context *context)
{
	char addr[1024] = {0};

	struct pl name;

	int e = re_regex(cardstart,
	                 PTRDIFF(cardend, cardstart),
	                 "\nFN[:;]+["WILD"]+\n",
	                 NULL, &name);
	if (e) {
		warning("carddav: No fullname : %s\n", strerror(e));
		return e;
	}

	rstrip(&name);

	debug("carddav: Found: %r\n", &name);

	struct pl impp;

	/* RE's regex is only basic. A block will continue while characters
	match. So the end must be non matching charcters. */
	e = re_regex(cardstart,
	             PTRDIFF(cardend, cardstart),
	             "\nIMPP[:;]+["WILD"]+", NULL, &impp);

	if (!e) {
		struct pl impphost;
		struct pl imppuser;

		e = re_regex(impp.p, impp.l, "TYPE=SIP");
		if (!e)
			e = re_regex(impp.p, impp.l,
			             ":[A-Za-z0-9]+@[a-z0-9\\-.]+",
			             &imppuser, &impphost);
		if (!e) {
			re_snprintf(addr,
			            sizeof(addr),
			            "\"%r "CARDDAV"\" <sip:%r@%r>",
			            &name, &imppuser, &impphost);
			debug("carddav: IMPP SIP found\n");

			e = add_addr(context, addr);
			if (e)
				return e;
		}
	}

	if (!addr[0]) {
		return process_tel_card(cardstart, cardend,
		                        context, &name,
		                        addr, sizeof(addr));
	}

	return 0;
}


static size_t writefunc(const void *ptr,
                        size_t size,
                        size_t nmemb,
                        void *userdata)
{
	struct carddav_context *context = userdata;

	size_t total = size*nmemb;

	debug("carddav: Chunk of %zu\n", total);

	unsigned freebuf = context->buf_len - context->buf_used;

	if (freebuf < total) {
		warning("carddav : chunks too big for free buffer %u\n",
		        freebuf);
		return 0;
	}

	str_ncpy(context->buf_a + context->buf_used, ptr, total);

	context->buf_used += total;

	char *pos = memmem(context->buf_a,
	                   context->buf_used,
	                   "BEGIN:VCARD",
	                   11);

	if (!pos) {
		debug("carddav: No card started after %u.\n",
		      context->buf_used);
		return total;
	}

	pos += 11;

	char *end = context->buf_a + context->buf_used;
	char *cardend = memmem(pos,
	                       PTRDIFF(end, pos),
	                       "END:VCARD",
	                       9);

	if (!cardend) {
		debug("carddav: No card complete after %u.\n",
		      context->buf_used);
		return total;
	}

	char * lastpos;

	while (pos && cardend) {
		process_card(pos,
		             (uintptr_t)cardend,
		             context);

		lastpos = cardend + 9;

		pos = memmem(lastpos,
		             PTRDIFF(end, lastpos),
		             "BEGIN:VCARD",
		             11);
		if (!pos)
			break;

		cardend = memmem(pos,
		                 PTRDIFF(end, pos),
		                 "END:VCARD",
		                  9);
	}

	unsigned remaining = (unsigned)PTRDIFF(end, lastpos);

	debug("carddav: Buffer swap with %u\n", remaining);

	str_ncpy(context->buf_b, lastpos, remaining);
	context->buf_used = remaining;

	char *t = context->buf_a;
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
		mem_ref(cur->data);
		contact_remove(contacts, cur->data);
		if (contacts_b)
			list_prepend(contacts_b, cur, cur->data);
		cur = next;
	}
}


static int upload(struct carddav_context *context, const char *name)
{
	CURL *curl = curl_easy_init();
	if (!curl)
		return EINVAL;
	unsigned len = re_snprintf(context->buf_b,
	                           context->buf_len,
	                           "%s/",
	                           context->url);
	unsigned namelen = strlen(name);

	if (len + namelen + 5 > context->buf_len) {
		warning("carddav: buffer to small for upload!\n");
		return EINVAL;
	}

	for (unsigned n = 0; n < namelen; n++) {
		char c = name[n];
		if (isalnum(c))
			context->buf_b[len++] = tolower(c);
		else
			context->buf_b[len++] = '_';
	}

	str_ncpy(&context->buf_b[len], ".vcf", context->buf_len - len);
	debug("carddav: uploading %s\n", context->buf_b);

	curl_easy_setopt(curl, CURLOPT_URL, context->buf_b);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_USERPWD, context->user);

	struct curl_slist *hs;
	hs = curl_slist_append(NULL, "Content-Type: text/vcf");

	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hs);
	curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");

	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, context->buf_a);

	CURLcode result = curl_easy_perform(curl);
	if (result == CURLE_OK)
		debug("carddav: Uploaded %s\n", name);
	else
		warning("carddav: Upload %s failed: %s\n",
		        name, curl_easy_strerror(result));
	curl_easy_cleanup(curl);
	return (int)result;
}


static int upload_phone_contact(struct carddav_context *context,
                                const char *name,
                                const char *phonenumber)
{
	re_snprintf(context->buf_a,
	            context->buf_len,
	            "BEGIN:VCARD\n"
	            "VERSION:3.0\n"
	            "FN:%s\n"
	            "TEL:%s\n"
	            "END:VCARD\n",
	            name, phonenumber);

	return upload(context, name);
}


static int upload_sip_contact(struct carddav_context *context,
                              const char *name,
                              const char *uri)
{
/*
  Good note:
  https://github.com/basepeak/roundcube-carddav/blob/main/doc/devdoc/IMPP.md
  IMPP;TYPE=SIP:johndoe@aol.com
*/

	re_snprintf(context->buf_a,
	            context->buf_len,
	            "BEGIN:VCARD\n"
	            "VERSION:3.0\n"
	            "FN:%s\n"
	            "IMPP;TYPE=%s\n"
	            "END:VCARD\n",
	            name, uri);

	return upload(context, name);
}


static void extract_name(char name[64], const char *con_str)
{
	const char * pos = con_str + 1;
	const char * end = strchr(pos, '"');
	unsigned len = PTRDIFF(end, pos) + 1;
	str_ncpy(name, pos, len);
}


static void restore_and_upload_unique(struct carddav_context *context,
                                      struct list *contacts_org,
                                      bool just_restore)
{
	struct contacts *contacts = context->contacts;

	for (struct le * cur = list_head(contacts_org);
	    cur;
	    cur = cur->next) {
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

		if (pos == end) {
			unsigned len = PTRDIFF(end, uri) - 3;
			char pn[16] = {0};

			str_ncpy(pn, uri+4, len);

			re_snprintf(context->buf_a,
			            context->buf_len,
			            "sip:%s@%s",
			            pn, context->gateway);

			dup = contact_find(contacts, context->buf_a);
			if (dup) {
				debug("carddav: Phone number %s"
				      " already on gateway.\n", pn);
				continue;
			}

			if (!just_restore && context->upload) {
				debug("carddav: Uploading \"%s\" "
				      "Phone number %s\n",
				      name,  pn);
				e = upload_phone_contact(context, name, pn);
			}
		}
		else {
			if (!just_restore && context->upload) {
				debug("carddav: Uploading \"%s\" "
				      "Non-number %s\n",
				      name,  uri);
				e = upload_sip_contact(context, name, uri);
			}
		}

		if (!e) {
			re_snprintf(context->buf_a,
			            context->buf_len,
			            "\"%s "CARDDAV"\" <%s>",
			            name, uri);

			info("carddav: Adding as back %s\n", context->buf_a);
			pl_set_str(&pl, context->buf_a);
		}
		else {
			info("carddav: Adding back %s\n", con_str);
			pl_set_str(&pl, con_str);
		}

		e = contact_add(contacts, NULL, &pl);
		if (e)
			warning("carddav: Failed to add back %.*s : %s\n",
			        pl.l, pl.p, strerror(e));
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


static int carddav_sync(void)
{
	char user[1024] = {0};
	char url[4096] = {0};
	char gateway[256] = {0};

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

	conf_get_bool(conf_cur(), "carddav_upload", &context.upload);
	info("carddav: upload : %s\n", (context.upload)?"true":"false");

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

	/* Restore main carddav for any uploads. */
	conf_get_str(conf_cur(), "carddav_user", user, sizeof(user));
	conf_get_str(conf_cur(), "carddav_url", url, sizeof(url));

	restore_and_upload_unique(&context, &contacts_list_org, false);
	list_flush(&contacts_list_org);
	goto cleanup;

bad:
	move_contacts(contacts_list, NULL, context.contacts);
	restore_and_upload_unique(&context, &contacts_list_org, true);
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
