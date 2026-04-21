#include <stdio.h>
#include <curl/curl.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>

#include <re.h>
#include <baresip.h>

#define PTRDIFF(a,b) ((uintptr_t)a - (uintptr_t)b)


struct carddav_context
{
	struct contacts *contacts;
	uint32_t buf_len;
	uint32_t buf_used;
	char * buf_a;
	char * buf_b;
	char * gateway;
	unsigned count;
};


static bool get_vcard_attr(const char * cardstart,
                    uintptr_t cardend,
                    const char * attr,
                    char result[512])
{
	char attr2[32];
	unsigned attr_len = re_snprintf(attr2,
	                                sizeof(attr2),
	                                "%s:", attr);

	char * found = memmem(cardstart,
	                      cardend-(uintptr_t)cardstart,
	                      attr2, attr_len);

	if (!found) {
		attr2[attr_len-1]=';';

		found = memmem(cardstart,
		               cardend-(uintptr_t)cardstart,
		               attr2, attr_len);

		if (!found)
			return false;
	}

	found+=attr_len;

	char * attrend = memmem(found,
	                        cardend-(uintptr_t)cardstart,
	                       "&#13;", 4);

	if (!attrend)
		return false;

	str_ncpy(result, found, (uintptr_t)attrend-(uintptr_t)found+1);
	return true;
}


static int process_card(const char * cardstart,
                         uintptr_t cardend,
                         struct carddav_context * context)
{
	char name[512] = {0};
	char tel[512] = {0};
	if (!get_vcard_attr(cardstart, cardend, "\nFN", name))
		return EINVAL;
	if (!get_vcard_attr(cardstart, cardend, "\nTEL", tel))
		return EINVAL;

	unsigned long pos = strlen(tel);
	while (pos) {
		if (tel[pos] == ':' || tel[pos] == ':') {
			char addr[1024] = {0};
			struct pl pl;
			unsigned len = re_snprintf(addr,
			                           sizeof(addr),
			                           "\"%s\" <sip:",
			                           name);
			++pos;
			unsigned hascode=0;
			while (tel[pos]) {
				char c = tel[pos++];
				if ( c == '+') {
					addr[len++]='0';
					++hascode;
				}
				else if (isdigit(c)) {
					if (hascode==1) {
						addr[len++]='0';
						hascode=0;
					}
					else if (hascode>2) {
						len-=(hascode-2);
						hascode=0;
					}
					addr[len++]=c;
				}
			}
			re_snprintf(addr+len, sizeof(addr)-len,
			            "@%s>", context->gateway);

			pl_set_str(&pl, addr);
			info("carddav: Adding %s\n", addr);
			int e = contact_add(context->contacts, NULL, &pl);
			if (!e)
				context->count++;
			else
				warning("carddav: Failed to add contact.\n");
			return e;
		}
		--pos;
	}
	return EINVAL;
}


static size_t writefunc(const void *ptr,
                        size_t size,
                        size_t nmemb,
                        void * userdata)
{
	struct carddav_context * context = userdata;

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

	char * pos = memmem(context->buf_a,
	                    context->buf_used,
	                    "BEGIN:VCARD",
	                    11);

	if (!pos) {
		debug("carddav: No card started after %u.\n",
		      context->buf_used);
		return total;
	}

	char * end = context->buf_a + context->buf_used;
	char * cardend = memmem(pos,
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

	char * t = context->buf_a;
	context->buf_a = context->buf_b;
	context->buf_b = t;

	return total;
}


static int cmd_sync(struct re_printf *pf, void *arg)
{
	struct carddav_context context = {0};
	char user[1024] = {0};
	char url[4096] = {0};
	char gateway[256] = {0};
	(void)pf;
	(void)arg;

	if (conf_get_str(conf_cur(), "carddav_gateway",
	                 gateway, sizeof(gateway)) ||
	    conf_get_str(conf_cur(), "carddav_user",
	                 user, sizeof(user)) ||
	    conf_get_str(conf_cur(), "carddav_url",
	                 url, sizeof(url))) {
		warning("carddav: Missing config.\n");
		return EINVAL;
	}

	info("carddav: using URL: %s\n", url);
	info("carddav: using user: %s\n", user);
	info("carddav: using gateway: %s\n", gateway);

	conf_get_u32(conf_cur(), "carddav_url", &context.buf_len);
	if (!context.buf_used)
		context.buf_len = 1024 * 32;

	info("carddav: using buffer of: %u\n", context.buf_len);
	context.contacts = baresip_contacts();
	context.gateway = gateway;

	CURLcode result = curl_global_init(CURL_GLOBAL_ALL);
	if (result != CURLE_OK)
		return (int)result;

	context.buf_a = mem_zalloc(context.buf_len, NULL);
	if (!context.buf_a) {
		warning("carddav: Unable to allocate carddav buffer 1.\n");
		return ENOMEM;
	}
	context.buf_b = mem_zalloc(context.buf_len, NULL);
	if (!context.buf_b) {
		warning("carddav: Unable to allocate carddav buffer 2.\n");
		mem_deref(context.buf_a);
		return ENOMEM;
	}

	CURL *curl = curl_easy_init();
	if (curl) {
		curl_easy_setopt(curl, CURLOPT_URL, url);
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(curl, CURLOPT_USERPWD, user);

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
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &context);

		info("carddav: wipe existing contacts.\n");
		list_flush(contact_list(context.contacts));

		result = curl_easy_perform(curl);
		if (result != CURLE_OK)
			warning("curl_easy_perform() failed: %s\n",
			        curl_easy_strerror(result));
		info("carddav: Added %u contacts.\n", context.count);
		debug("carddav: curl complete.\n");

		curl_easy_cleanup(curl);
	}
	curl_global_cleanup();

	mem_deref(context.buf_a);
	mem_deref(context.buf_b);

	return 0;
}


static const struct cmd cmdv[] = {
	{"refreshcontacts", 0, 0, "Refresh contacts from carddav", cmd_sync},
};


static int module_init(void)
{
	int err = cmd_register(baresip_commands(), cmdv, RE_ARRAY_SIZE(cmdv));
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
