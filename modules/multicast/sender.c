/**
 * @file sender.c
 *
 * Copyright (C) 2021 Commend.com - c.huber@commend.com
 */

#include <re.h>
#include <rem.h>
#include <baresip.h>

#include <stdlib.h>

#include "multicast.h"

#define DEBUG_MODULE "mcsend"
#define DEBUG_LEVEL 6
#include <re_dbg.h>


static struct list mcsenderl = LIST_INIT;


/**
 * Multicast sender struct
 *
 * Contains data to send audio stream to the network
 */
struct mcsender {
	struct le le;

	struct sa addr;
	struct rtp_sock *rtp;

	struct config_audio *cfg;
	const struct aucodec *ac;

	struct play *play;
	RE_ATOMIC uint8_t gong_eof;
	uint8_t eofmax;

	struct mcsource *src;
	bool enable;
};


static void mcsender_destructor(void *arg)
{
	struct mcsender *mcsender = arg;

	mcsource_stop(mcsender->src);

	mcsender->play   = mem_deref(mcsender->play);
	mcsender->src    = mem_deref(mcsender->src);
	mcsender->rtp    = mem_deref(mcsender->rtp);
}


/**
 * Multicast address comparison
 *
 * @param le  List element (mcsender)
 * @param arg Argument     (address)
 *
 * @return true  if mcsender->addr == address
 * @return false if mcsender->addr != address
 */
static bool mcsender_addr_cmp(struct le *le, void *arg)
{
	struct mcsender *mcsender = le->data;
	struct sa *addr = arg;

	return sa_cmp(&mcsender->addr, addr, SA_ALL);
}


/**
 * Multicast send handler
 *
 * @param ext_len RTP extension header Length
 * @param marker  RTP marker
 * @param mb      Data to send
 *
 * @return 0 if success, otherwise errorcode
 */
static int mcsender_send_handler(size_t ext_len, bool marker,
	uint32_t rtp_ts, struct mbuf *mb, void *arg)
{
	struct mcsender *mcsender = arg;
	struct pl placpt = PL_INIT;
	int err = 0;

	if (!mb)
		return EINVAL;

	if (!mcsender->enable)
		return 0;

	if (uag_call_count())
		return 0;

	pl_set_str(&placpt, mcsender->ac->pt);
	err = rtp_send(mcsender->rtp, &mcsender->addr, ext_len != 0, marker,
		pl_u32(&placpt), rtp_ts, tmr_jiffies_rt_usec(), mb);

	return err;
}


/**
 * Gong EOF handler (multicast source)
 *
 * @param arg  Handler argument
 */
static void mcsender_gong_eof_handler(void *arg) {
	struct mcsender *mcs = arg;

	if (re_atomic_acq_add(&mcs->gong_eof, 1) == (mcs->eofmax - 1)) {
		module_event("multicast", "sender gong eof", NULL, NULL,
			"addr=%J codec=%s", &mcs->addr, mcs->ac->name);
	}
}


/**
 * Gong EOF handler (baresip player)
 *
 * @param play
 * @param arg
 */
static void mcsender_gong_play_end_handler(struct play *play, void *arg)
{
	struct mcsender *mcs = arg;
	(void) play;

	if (re_atomic_acq_add(&mcs->gong_eof, 1) == (mcs->eofmax - 1)) {
		module_event("multicast", "sender gong eof", NULL, NULL,
			"addr=%J codec=%s", &mcs->addr, mcs->ac->name);
	}
}


/**
 * Setup the player for local gong
 *
 * @param mcs  Multicast sender
 * @param gong Path to gong file
 *
 * @return 0 if success, otherwise errorcode
 */
static int setup_local_gong(struct mcsender *mcs, struct pl *gong)
{
	char *file = NULL;
	int err = 0;

	if (!mcs)
		return EINVAL;

	if (!pl_isset(gong))
		return 0;

	err = pl_strdup(&file, gong);
	if (err)
		return err;

	/* TODO: make multicast gong playback module and device configurable */
	err = play_file(&mcs->play, baresip_player(), file, 0,
			mcs->cfg->alert_mod, mcs->cfg->alert_dev);
	if (err) {
		warning ("mcsender: play file (%m)\n", err);
		goto out;
	}

	play_set_finish_handler(mcs->play, mcsender_gong_play_end_handler,
				mcs);
out:
	mem_deref(file);
	return err;
}


/**
 * Enable / Disable all existing sender
 *
 * @param enable
 */
void mcsender_enable(bool enable)
{
	struct le *le;
	struct mcsender *mcsender;

	LIST_FOREACH(&mcsenderl, le) {
		mcsender = le->data;
		mcsender->enable = enable;
	}
}


/**
 * Stop all existing multicast sender
 */
void mcsender_stopall(void)
{
	list_flush(&mcsenderl);
}


/**
 * Stop the multicast sender with addr
 *
 * @param addr Address
 */
void mcsender_stop(struct sa *addr)
{
	struct mcsender *mcsender = NULL;
	struct le *le;

	le = list_apply(&mcsenderl, true, mcsender_addr_cmp, addr);
	if (!le) {
		warning ("multicast: multicast sender %J not found\n", addr);
		return;
	}

	mcsender = le->data;
	list_unlink(&mcsender->le);
	mem_deref(mcsender);
}


/**
 * Allocate a new multicast sender object
 *
 * @param addr  Destination address
 * @param codec Used audio codec
 * @param gong  Optional absolute audio path
 *
 * @return 0 if success, otherwise errorcode
 */
int mcsender_alloc(struct sa *addr, const struct aucodec *codec,
		   struct pl *gong)
{
	struct mcsender *mcsender = NULL;
	uint8_t ttl = multicast_ttl();
	int err = 0;

	if (!addr || !codec)
		return EINVAL;

	if (list_apply(&mcsenderl, true, mcsender_addr_cmp, addr)) {
		err = EADDRINUSE;
		goto out;
	}

	mcsender = mem_zalloc(sizeof(*mcsender), mcsender_destructor);
	if (!mcsender) {
		err = ENOMEM;
		goto out;
	}

	sa_cpy(&mcsender->addr, addr);
	mcsender->ac = codec;
	mcsender->enable = true;
	mcsender->cfg = &conf_config()->audio;
	mcsender->eofmax = 0;
	re_atomic_rlx_set(&mcsender->gong_eof, 0);

	err = rtp_open(&mcsender->rtp, sa_af(&mcsender->addr));
	if (err) {
		warning ("mcsender: rtp socket creation failed (%m)", err);
		goto out;
	}

	if (ttl > 1) {
		struct udp_sock *sock;
		debug ("mcsender: set RTP package TTL to %d\n", ttl);

		sock = (struct udp_sock *) rtp_sock(mcsender->rtp);
		udp_setsockopt(sock, IPPROTO_IP,
			IP_MULTICAST_TTL, &ttl, sizeof(ttl));
	}

	err = mcsource_start(&mcsender->src, mcsender->ac, gong,
			     mcsender_send_handler, mcsender_gong_eof_handler,
			     mcsender);
	if (err)
		goto out;

	mcsender->eofmax++;
	err = setup_local_gong(mcsender, gong);
	if (err) {
		warning ("mcsender: local gong playback failed. "
			"cancel multicast (%m)\n", err);
		goto out;
	}

	mcsender->eofmax++;
	list_append(&mcsenderl, &mcsender->le, mcsender);

 out:
	if (err)
		mem_deref(mcsender);

	return err;
}


/**
 * Print all available multicast sender
 *
 * @param pf Printer
 */
void mcsender_print(struct re_printf *pf)
{
	struct le *le = NULL;
	struct mcsender *mcsender = NULL;

	re_hprintf(pf, "Multicast Sender List:\n");
	LIST_FOREACH(&mcsenderl, le) {
		mcsender = le->data;
		re_hprintf(pf, "   %J - %s%s\n", &mcsender->addr,
			mcsender->ac->name,
			mcsender->enable ? " (enabled)" : " (disabled)");
	}
}
