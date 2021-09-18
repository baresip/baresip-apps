/**
 * @file vidloop.c  Video loop
 *
 * Copyright (C) 2010 Alfred E. Heggestad
 */
#define _DEFAULT_SOURCE 1
#define _BSD_SOURCE 1
#include <string.h>
#include <time.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>


/**
 * @defgroup vidloop vidloop
 *
 * A video-loop module for testing
 *
 * Simple test module that loops back the video frames from a
 * video-source to a video-display, optionally via a video codec.
 *
 * Example usage without codec:
 \verbatim
  baresip -e/vidloop
 \endverbatim
 *
 * Example usage with codec:
 \verbatim
  baresip -e"/vidloop h264"
 \endverbatim
 */


enum {
	VIDEO_SRATE = 90000
};


/** Video Statistics */
struct vstat {
	uint64_t tsamp;
	uint32_t frames;
	size_t bytes;
	uint32_t bitrate;
	double efps;
	size_t n_keyframe;
};


struct timestamp_state {
	uint64_t base;  /* lowest timestamp */
	uint64_t last;  /* most recent timestamp */
	bool is_set;
};


/** Video loop */
struct video_loop {
	const struct vidcodec *vc_enc;
	const struct vidcodec *vc_dec;
	struct config_video cfg;
	struct videnc_state *enc;
	struct viddec_state *dec;
	const struct vidisp *vd;
	struct vidisp_st *vidisp;
	struct vidsrc *vs;
	struct vidsrc_st *vsrc;
	struct vidsrc_prm srcprm;
	struct list filtencl;
	struct list filtdecl;
	struct vstat stat;
	struct tmr tmr_bw;
	struct tmr tmr_display;
	struct tmr tmr_update_src;
	struct vidsz src_size;
	struct vidsz disp_size;
	enum vidfmt src_fmt;
	enum vidfmt disp_fmt;
	struct vidframe *frame;
	uint64_t frame_timestamp;
	struct lock *frame_mutex;
	bool new_frame;
	uint64_t ts_start;      /* usec */
	uint64_t ts_last;       /* usec */
	uint16_t seq;
	bool need_conv;
	bool started;
	int err;

	struct {
		uint64_t src_frames;
		uint64_t enc_bytes;
		uint64_t enc_packets;
		uint64_t disp_frames;
	} stats;

	struct timestamp_state ts_src;
	struct timestamp_state ts_rtp;
};


static struct video_loop *gvl;


static int enable_decoder(struct video_loop *vl, const char *name);


static void timestamp_state_update(struct timestamp_state *st,
				   uint64_t ts)
{
	if (st->is_set) {
		if (ts < st->base) {
			warning("vidloop: timestamp wrapped -- reset base"
				" (base=%llu, current=%llu)\n",
				st->base, ts);
			st->base = ts;
		}
	}
	else {
		st->base = ts;
		st->is_set = true;
	}

	st->last = ts;
}


static double timestamp_state_duration(const struct timestamp_state *ts,
				       uint32_t clock_rate)
{
	uint64_t dur;

	if (ts->is_set)
		dur = ts->last - ts->base;
	else
		dur = 0;

	return (double)dur / (double)clock_rate;
}


static void vidframe_clear(struct vidframe *frame)
{
	frame->data[0] = NULL;
}


static void display_handler(void *arg)
{
	struct video_loop *vl = arg;
	int err;

	tmr_start(&vl->tmr_display, 10, display_handler, vl);

	lock_write_get(vl->frame_mutex);

	if (!vl->new_frame)
		goto out;

	/* display frame */
	err = vl->vd->disph(vl->vidisp, "Video Loop",
			     vl->frame, vl->frame_timestamp);
	vl->new_frame = false;

	if (err == ENODEV) {
		info("vidloop: video-display was closed\n");
		vl->vidisp = mem_deref(vl->vidisp);
		vl->err = err;
	}
	++vl->stats.disp_frames;

 out:
	lock_rel(vl->frame_mutex);
}


static int display(struct video_loop *vl, struct vidframe *frame,
		   uint64_t timestamp)
{
	struct vidframe *frame_filt = NULL;
	struct le *le;
	int err = 0;

	if (!vidframe_isvalid(frame))
		return 0;

	if (!list_isempty(&vl->filtdecl)) {

		/* Some video decoders keeps the displayed video frame
		 * in memory and we should not write to that frame.
		 */

		err = vidframe_alloc(&frame_filt, frame->fmt, &frame->size);
		if (err)
			return err;

		vidframe_copy(frame_filt, frame);

		frame = frame_filt;
	}

	/* Process video frame through all Video Filters */
	for (le = vl->filtdecl.head; le; le = le->next) {

		struct vidfilt_dec_st *st = le->data;

		if (st->vf->dech)
			err |= st->vf->dech(st, frame, &timestamp);
	}

	if (err) {
		warning("vidloop: error in decode video-filter (%m)\n", err);
	}

	/* save the displayed frame info */
	vl->disp_size = frame->size;
	vl->disp_fmt = frame->fmt;

	lock_write_get(vl->frame_mutex);

	if (vl->frame && ! vidsz_cmp(&vl->frame->size, &frame->size)) {

		info("vidloop: resolution changed:  %u x %u\n",
		     frame->size.w, frame->size.h);

		vl->frame = mem_deref(vl->frame);
	}

	if (!vl->frame) {
		err = vidframe_alloc(&vl->frame, frame->fmt, &frame->size);
		if (err)
			goto out;
	}

	vidframe_copy(vl->frame, frame);
	vl->frame_timestamp = timestamp;
	vl->new_frame = true;

 out:
	lock_rel(vl->frame_mutex);

	mem_deref(frame_filt);

	return err;
}


static int packet_handler(bool marker, uint64_t rtp_ts,
			  const uint8_t *hdr, size_t hdr_len,
			  const uint8_t *pld, size_t pld_len,
			  void *arg)
{
	struct video_loop *vl = arg;
	struct vidframe frame;
	struct mbuf *mb;
	uint64_t timestamp;
	bool keyframe;
	int err = 0;

	++vl->stats.enc_packets;
	vl->stats.enc_bytes += (hdr_len + pld_len);

	timestamp_state_update(&vl->ts_rtp, rtp_ts);

	mb = mbuf_alloc(hdr_len + pld_len);
	if (!mb)
		return ENOMEM;

	if (hdr_len)
		mbuf_write_mem(mb, hdr, hdr_len);
	mbuf_write_mem(mb, pld, pld_len);

	mb->pos = 0;

	vl->stat.bytes += mbuf_get_left(mb);

	/* decode */
	vidframe_clear(&frame);
	if (vl->vc_dec && vl->dec) {
		err = vl->vc_dec->dech(vl->dec, &frame, &keyframe,
				       marker, vl->seq++, mb);
		if (err) {
			warning("vidloop: codec decode: %m\n", err);
			goto out;
		}

		if (keyframe)
			++vl->stat.n_keyframe;
	}

	/* convert the RTP timestamp to VIDEO_TIMEBASE timestamp */
	timestamp = video_calc_timebase_timestamp(rtp_ts);

	if (vidframe_isvalid(&frame)) {

		display(vl, &frame, timestamp);
	}

 out:
	mem_deref(mb);

	return 0;
}


static void vidsrc_frame_handler(struct vidframe *frame, uint64_t timestamp,
				 void *arg)
{
	struct video_loop *vl = arg;
	struct vidframe *f2 = NULL;
	struct le *le;
	const uint64_t now = tmr_jiffies_usec();
	int err = 0;

	/* save the timing info */
	if (!gvl->ts_start)
		gvl->ts_start = now;
	gvl->ts_last = now;

	/* save the video frame info */
	vl->src_size = frame->size;
	vl->src_fmt = frame->fmt;
	++vl->stats.src_frames;

	timestamp_state_update(&vl->ts_src, timestamp);

	++vl->stat.frames;

	if (frame->fmt != (enum vidfmt)vl->cfg.enc_fmt) {

		if (!vl->need_conv) {
			info("vidloop: NOTE: pixel-format conversion"
			     " needed: %s  -->  %s\n",
			     vidfmt_name(frame->fmt),
			     vidfmt_name(vl->cfg.enc_fmt));
			vl->need_conv = true;
		}

		if (vidframe_alloc(&f2, vl->cfg.enc_fmt, &frame->size))
			return;

		vidconv(f2, frame, 0);

		frame = f2;
	}

	/* Process video frame through all Video Filters */
	for (le = vl->filtencl.head; le; le = le->next) {

		struct vidfilt_enc_st *st = le->data;

		if (st->vf->ench)
			err |= st->vf->ench(st, frame, &timestamp);
	}

	if (vl->vc_enc && vl->enc) {

		err = vl->vc_enc->ench(vl->enc, false, frame, timestamp);
		if (err) {
			warning("vidloop: encoder error (%m)\n", err);
			goto out;
		}
	}
	else {
		vl->stat.bytes += vidframe_size(frame->fmt, &frame->size);
		(void)display(vl, frame, timestamp);
	}

 out:
	mem_deref(f2);
}


static void vidsrc_packet_handler(struct vidpacket *packet, void *arg)
{
	struct video_loop *vl = arg;
	uint64_t rtp_ts;

	rtp_ts = video_calc_rtp_timestamp_fix(packet->timestamp);

	/* todo: hardcoded codecid, get from packet */
	if (!vl->vc_dec) {
		enable_decoder(vl, "h264");
	}

	h264_packetize(rtp_ts, packet->buf, packet->size,
		       1480, packet_handler, vl);
}


static int print_stats(struct re_printf *pf, const struct video_loop *vl)
{
	const struct config_video *cfg = &vl->cfg;
	double src_dur, real_dur = .0;
	int err = 0;

	src_dur = timestamp_state_duration(&vl->ts_src, VIDEO_TIMEBASE);

	if (vl->ts_start)
		real_dur = (vl->ts_last - vl->ts_start) * .000001;

	err |= re_hprintf(pf, "~~~~~ Videoloop summary: ~~~~~\n");

	/* Source */
	if (vl->vsrc) {
		struct vidsrc *vs = vl->vs;
		double avg_fps = .0;

		if (vl->stats.src_frames >= 2)
			avg_fps = (vl->stats.src_frames-1) / src_dur;

		err |= re_hprintf(pf,
				  "* Source\n"
				  "  module      %s\n"
				  "  resolution  %u x %u (actual %u x %u)\n"
				  "  pixformat   %s\n"
				  "  frames      %llu\n"
				  "  framerate   %.2f fps  (avg %.2f fps)\n"
				  "  duration    %.3f sec  (real %.3f sec)\n"
				  "\n"
				  ,
				  vs->name,
				  cfg->width, cfg->height,
				  vl->src_size.w, vl->src_size.h,
				  vidfmt_name(vl->src_fmt),
				  vl->stats.src_frames,
				  vl->srcprm.fps, avg_fps,
				  src_dur, real_dur);
	}

	/* Video conversion */
	if (vl->need_conv) {
		err |= re_hprintf(pf,
				  "* Vidconv\n"
				  "  pixformat   %s\n"
				  "\n"
				  ,
				  vidfmt_name(cfg->enc_fmt));
	}

	/* Filters */
	if (!list_isempty(baresip_vidfiltl())) {
		struct le *le;

		err |= re_hprintf(pf,
				  "* Filters (%u):",
				  list_count(baresip_vidfiltl()));

		for (le = list_head(baresip_vidfiltl()); le; le = le->next) {
			struct vidfilt *vf = le->data;
			err |= re_hprintf(pf, " %s", vf->name);
		}
		err |= re_hprintf(pf, "\n\n");
	}

	/* Encoder */
	if (vl->vc_enc) {
		double avg_bitrate;
		double avg_pktrate;
		double dur;

		dur = timestamp_state_duration(&vl->ts_rtp, VIDEO_SRATE);
		avg_bitrate = 8.0 * (double)vl->stats.enc_bytes / dur;
		avg_pktrate = (double)vl->stats.enc_packets / dur;

		err |= re_hprintf(pf,
				  "* Encoder\n"
				  "  module      %s\n"
				  "  bitrate     %u bit/s (avg %.1f bit/s)\n"
				  "  packets     %llu     (avg %.1f pkt/s)\n"
				  "  duration    %.3f sec\n"
				  "\n"
				  ,
				  vl->vc_enc->name,
				  cfg->bitrate, avg_bitrate,
				  vl->stats.enc_packets, avg_pktrate,
				  dur);
	}

	/* Decoder */
	if (vl->vc_dec) {
		err |= re_hprintf(pf,
				  "* Decoder\n"
				  "  module      %s\n"
				  "  key-frames  %zu\n"
				  "\n"
				  ,
				  vl->vc_dec->name,
				  vl->stat.n_keyframe);
	}

	/* Display */
	if (vl->vidisp) {
		const struct vidisp *vd = vl->vd;

		err |= re_hprintf(pf,
				  "* Display\n"
				  "  module      %s\n"
				  "  resolution  %u x %u\n"
				  "  pixformat   %s\n"
				  "  frames      %llu\n"
				  "\n"
				  ,
				  vd->name,
				  vl->disp_size.w, vl->disp_size.h,
				  vidfmt_name(vl->disp_fmt),
				  vl->stats.disp_frames);
	}

	return err;
}


static void vidloop_destructor(void *arg)
{
	struct video_loop *vl = arg;

	if (vl->started)
		re_printf("%H\n", print_stats, vl);

	tmr_cancel(&vl->tmr_bw);
	mem_deref(vl->vsrc);
	mem_deref(vl->enc);
	mem_deref(vl->dec);
	tmr_cancel(&vl->tmr_update_src);

	lock_write_get(vl->frame_mutex);
	mem_deref(vl->vidisp);
	mem_deref(vl->frame);
	tmr_cancel(&vl->tmr_display);
	lock_rel(vl->frame_mutex);

	list_flush(&vl->filtencl);
	list_flush(&vl->filtdecl);
	mem_deref(vl->frame_mutex);
}


static int enable_encoder(struct video_loop *vl, const char *name)
{
	struct list *vidcodecl = baresip_vidcodecl();
	struct videnc_param prm;
	int err;

	prm.fps     = vl->cfg.fps;
	prm.pktsize = 1480;
	prm.bitrate = vl->cfg.bitrate;
	prm.max_fs  = -1;

	vl->vc_enc = vidcodec_find_encoder(vidcodecl, name);
	if (!vl->vc_enc) {
		warning("vidloop: could not find encoder (%s)\n", name);
		return ENOENT;
	}

	info("vidloop: enabled encoder %s (%.2f fps, %u bit/s)\n",
	     vl->vc_enc->name, prm.fps, prm.bitrate);

	err = vl->vc_enc->encupdh(&vl->enc, vl->vc_enc, &prm, NULL,
				  packet_handler, vl);
	if (err) {
		warning("vidloop: update encoder failed: %m\n", err);
		return err;
	}

	return 0;
}


static int enable_decoder(struct video_loop *vl, const char *name)
{
	struct list *vidcodecl = baresip_vidcodecl();
	int err;

	vl->vc_dec = vidcodec_find_decoder(vidcodecl, name);
	if (!vl->vc_dec) {
		warning("vidloop: could not find decoder (%s)\n", name);
		return ENOENT;
	}

	info("vidloop: enabled decoder %s\n", vl->vc_dec->name);

	if (vl->vc_dec->decupdh) {
		err = vl->vc_dec->decupdh(&vl->dec, vl->vc_dec, NULL);
		if (err) {
			warning("vidloop: update decoder failed: %m\n", err);
			return err;
		}
	}

	return 0;
}


static void print_status(struct video_loop *vl)
{
	re_printf("\rstatus:"
		  " %.3f sec [%s] [%s]  fmt=%s "
		  " EFPS=%.1f      %u kbit/s",
		  timestamp_state_duration(&vl->ts_src, VIDEO_TIMEBASE),
		  vl->vc_enc ? vl->vc_enc->name : "",
		  vl->vc_dec ? vl->vc_dec->name : "",
		  vidfmt_name(vl->cfg.enc_fmt),
		  vl->stat.efps, vl->stat.bitrate);

	if (vl->enc || vl->dec)
		re_printf("  key-frames=%zu", vl->stat.n_keyframe);

	re_printf("       \r");

	fflush(stdout);
}


static void calc_bitrate(struct video_loop *vl)
{
	const uint64_t now = tmr_jiffies();

	if (now > vl->stat.tsamp) {

		const uint32_t dur = (uint32_t)(now - vl->stat.tsamp);

		vl->stat.efps = 1000.0f * vl->stat.frames / dur;

		vl->stat.bitrate = (uint32_t) (8 * vl->stat.bytes / dur);
	}

	vl->stat.frames = 0;
	vl->stat.bytes = 0;
	vl->stat.tsamp = now;
}


static void timeout_bw(void *arg)
{
	struct video_loop *vl = arg;

	if (vl->err) {
		info("error in video-loop -- closing (%m)\n", vl->err);
		gvl = mem_deref(gvl);
		return;
	}

	tmr_start(&vl->tmr_bw, 100, timeout_bw, vl);

	calc_bitrate(vl);
	print_status(vl);
}


static int vsrc_reopen(struct video_loop *vl, const struct vidsz *sz)
{
	int err;

	info("vidloop: %s,%s: open video source: %u x %u at %.2f fps\n",
	     vl->cfg.src_mod, vl->cfg.src_dev,
	     sz->w, sz->h, vl->cfg.fps);

	vl->srcprm.fps    = vl->cfg.fps;
	vl->srcprm.fmt    = vl->cfg.enc_fmt;

	vl->vsrc = mem_deref(vl->vsrc);
	err = vidsrc_alloc(&vl->vsrc, baresip_vidsrcl(),
			   vl->cfg.src_mod, NULL, &vl->srcprm, sz,
			   NULL, vl->cfg.src_dev, vidsrc_frame_handler,
			   vidsrc_packet_handler,
			   NULL, vl);
	if (err) {
		warning("vidloop: vidsrc '%s' failed: %m\n",
			vl->cfg.src_dev, err);
		return err;
	}

	vl->vs = (struct vidsrc *)vidsrc_find(baresip_vidsrcl(),
					      vl->cfg.src_mod);

	return 0;
}


static void update_vidsrc(void *arg)
{
	struct video_loop *vl = arg;
	struct vidsz size;
	struct config *cfg = conf_config();
	int err;

	tmr_start(&vl->tmr_update_src, 100, update_vidsrc, vl);

	if (!strcmp(vl->cfg.src_mod, cfg->video.src_mod) &&
	    !strcmp(vl->cfg.src_dev, cfg->video.src_dev))
		return;

	str_ncpy(vl->cfg.src_mod, cfg->video.src_mod, sizeof(vl->cfg.src_mod));
	str_ncpy(vl->cfg.src_dev, cfg->video.src_dev, sizeof(vl->cfg.src_dev));

	size.w = cfg->video.width;
	size.h = cfg->video.height;

	err = vsrc_reopen(gvl, &size);
	if (err)
		gvl = mem_deref(gvl);
}


static int video_loop_alloc(struct video_loop **vlp)
{
	struct vidisp_prm disp_prm;
	struct video_loop *vl;
	struct config *cfg;
	struct le *le;
	int err = 0;

	cfg = conf_config();
	if (!cfg)
		return EINVAL;

	vl = mem_zalloc(sizeof(*vl), vidloop_destructor);
	if (!vl)
		return ENOMEM;

	vl->cfg = cfg->video;
	tmr_init(&vl->tmr_bw);
	tmr_init(&vl->tmr_display);
	tmr_init(&vl->tmr_update_src);

	vl->src_fmt = -1;
	vl->disp_fmt = -1;

	err = lock_alloc(&vl->frame_mutex);
	if (err)
		goto out;

	vl->new_frame = false;
	vl->frame = NULL;

	/* Video filters */
	for (le = list_head(baresip_vidfiltl()); le; le = le->next) {
		struct vidfilt *vf = le->data;
		struct vidfilt_prm prmenc, prmdec;
		void *ctx = NULL;

		prmenc.width  = vl->cfg.width;
		prmenc.height = vl->cfg.height;
		prmenc.fmt    = vl->cfg.enc_fmt;
		prmenc.fps    = vl->cfg.fps;

		prmdec.width  = 0;
		prmdec.height = 0;
		prmdec.fmt    = -1;
		prmdec.fps    = .0;

		info("vidloop: added video-filter '%s'\n", vf->name);

		err |= vidfilt_enc_append(&vl->filtencl, &ctx, vf, &prmenc, 0);
		err |= vidfilt_dec_append(&vl->filtdecl, &ctx, vf, &prmdec, 0);
		if (err) {
			warning("vidloop: vidfilt error: %m\n", err);
		}
	}

	info("vidloop: open video display (%s.%s)\n",
	     vl->cfg.disp_mod, vl->cfg.disp_dev);

	disp_prm.fullscreen = cfg->video.fullscreen;

	err = vidisp_alloc(&vl->vidisp, baresip_vidispl(),
			   vl->cfg.disp_mod, &disp_prm,
			   vl->cfg.disp_dev, NULL, vl);
	if (err) {
		warning("vidloop: video display failed: %m\n", err);
		goto out;
	}

	vl->vd = vidisp_find(baresip_vidispl(), vl->cfg.disp_mod);

	tmr_start(&vl->tmr_bw, 1000, timeout_bw, vl);

	/* NOTE: usually (e.g. SDL2),
			 video frame must be rendered from main thread */
	tmr_start(&vl->tmr_display, 10, display_handler, vl);
	tmr_start(&vl->tmr_update_src, 10, update_vidsrc, vl);

 out:
	if (err)
		mem_deref(vl);
	else
		*vlp = vl;

	return err;
}


/**
 * Start the video loop (for testing)
 */
static int vidloop_start(struct re_printf *pf, void *arg)
{
	const struct cmd_arg *carg = arg;
	struct vidsz size;
	struct config *cfg = conf_config();
	const char *codec_name = carg->prm;
	int err = 0;

	size.w = cfg->video.width;
	size.h = cfg->video.height;

	if (gvl) {
		return re_hprintf(pf, "video-loop already running.\n");
	}

	(void)re_hprintf(pf, "Enable video-loop on %s,%s: %u x %u\n",
			 cfg->video.src_mod, cfg->video.src_dev,
			 size.w, size.h);

	err = video_loop_alloc(&gvl);
	if (err) {
		warning("vidloop: alloc: %m\n", err);
		return err;
	}

	if (str_isset(codec_name)) {

		err  = enable_encoder(gvl, codec_name);
		err |= enable_decoder(gvl, codec_name);
		if (err) {
			gvl = mem_deref(gvl);
			return err;
		}

		(void)re_hprintf(pf, "%sabled codec: %s\n",
				 gvl->vc_enc ? "En" : "Dis",
				 gvl->vc_enc ? gvl->vc_enc->name : "");
	}

	/* Start video source, after codecs are created */
	err = vsrc_reopen(gvl, &size);
	if (err) {
		gvl = mem_deref(gvl);
		return err;
	}

	gvl->started = true;

	return err;
}


static int vidloop_stop(struct re_printf *pf, void *arg)
{
	(void)arg;

	if (gvl)
		(void)re_hprintf(pf, "Disable video-loop\n");
	gvl = mem_deref(gvl);
	return 0;
}


static const struct cmd cmdv[] = {
	{"vidloop",     0, CMD_PRM, "Start video-loop <codec>", vidloop_start},
	{"vidloop_stop",0, 0,       "Stop video-loop",          vidloop_stop },
};


static int module_init(void)
{
	return cmd_register(baresip_commands(), cmdv, ARRAY_SIZE(cmdv));
}


static int module_close(void)
{
	gvl = mem_deref(gvl);
	cmd_unregister(baresip_commands(), cmdv);
	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(vidloop) = {
	"vidloop",
	"application",
	module_init,
	module_close,
};
