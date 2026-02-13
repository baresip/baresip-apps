/**
 * @file rtsp/rtsp.c  Gstreamer 1.0 playbin pipeline
 *
 *  RTSP bidirectional audio for Baresip based on the gst/gst.c.
 *
 * Copyright (C) 2010 - 2015 Alfred E. Heggestad
 * Copyright (C) 2025 - Joe Burmeister
 */
#define _DEFAULT_SOURCE 1
#define _POSIX_C_SOURCE 199309L
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <gst/gst.h>
#include <unistd.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>
#include <sys/ioctl.h>
#include <termios.h>


/**
 * @defgroup rtsp rtsp
 *
 * Audio/Video source/player module using gstreamer 1.0
 *
 * This module implements an audio/video source/player which uses the
 * Gstreamer framework to bi-directionally stream audio from/to an RTSP device
 * and provides option rotatable video.
 *
 * Example config:
 \verbatim
  audio_source        rtsp,rtsp://someuser:somepw@someserver/ch0
  audio_player        rtsp,<backchannel-channel-index>
  video_source        rtsp,rtsp://someuser:somepw@someserver/ch0
  rtsp_rotation       (1=90 degrees clockwise,
                       2=180 degrees,
                       3=90 degrees counterclockwise)
 \endverbatim
 */

struct ausrc_st {
	RE_ATOMIC bool run;         /**< Running flag            */
	bool eos;                   /**< Reached end of stream   */
	ausrc_read_h *rh;           /**< Read handler            */
	ausrc_error_h *errh;        /**< Error handler           */
	void *arg;                  /**< Handler argument        */
	struct ausrc_prm prm;       /**< Read parameters         */
	size_t psize;               /**< Packet size in bytes    */
	size_t sampc;
	uint32_t ptime;
	char * device;

	struct tmr tmr;

	GstElement *pipeline;
	GstElement *rtspsrc;
	GstElement *sink;
};

struct auplay_st {
	thrd_t thread;
	size_t sampc;
	size_t dsize;
	int16_t *buf;
	auplay_write_h *wh;
	void *arg;
	struct auplay_prm prm;
	RE_ATOMIC bool run;
};

struct vidsrc_st {
	RE_ATOMIC bool run;
	uint64_t ts;
	struct vidsrc_prm prm;
	struct vidsz size;
	vidsrc_frame_h *frameh;
	void *arg;
	GstElement *sink;
	char * device;
};

struct au_backchannel_t {
	mtx_t * lock;
	GstElement *pipeline;
	GstElement *sink;
	volatile GstElement *src;
	volatile GstElement *rtsp;
	gint stream_id;
	unsigned options_num;
	GstCaps *options_caps[8];
	int options_streams[8];
	int option;
	unsigned src_rate;
	unsigned src_channels;
	unsigned blocksize;
};


static struct ausrc *ausrc = NULL;
static struct auplay *auplay = NULL;
static struct vidsrc *vidsrc = NULL;

static struct vidsrc_st *vidsrcst = NULL;
static struct ausrc_st *ausrcst = NULL;

static struct au_backchannel_t au_backchannel = {.stream_id = -1, .option=-1};


static void au_backchannel_unlink(void)
{
	info("rtsp: au_backchannel_unlink\n");
	mtx_lock(au_backchannel.lock);
	if (au_backchannel.pipeline) {
		gst_element_set_state(au_backchannel.pipeline, GST_STATE_NULL);
		gst_object_unref(au_backchannel.pipeline);
		au_backchannel.pipeline = NULL;
	}

	if (au_backchannel.src) {
		gst_object_unref(GST_OBJECT(au_backchannel.src));
		au_backchannel.src = NULL;
	}

	if (au_backchannel.rtsp) {
		gst_object_unref(GST_OBJECT(au_backchannel.rtsp));
		au_backchannel.rtsp = NULL;
	}

	if (au_backchannel.sink) {
		gst_object_unref(au_backchannel.sink);
		au_backchannel.sink = NULL;
	}

	for (unsigned n = 0; n < au_backchannel.options_num; n++) {
		gst_caps_unref(au_backchannel.options_caps[n]);
	}

	au_backchannel.option = -1;
	au_backchannel.options_num = 0;
	mtx_unlock(au_backchannel.lock);
}


static void ausrc_st_clear(struct ausrc_st *st)
{
	info("rtsp: Stopping rtsp source.\n");
	re_atomic_rlx_set(&st->run, false);

	tmr_cancel(&st->tmr);

	if (st->pipeline) {
		gst_element_set_state(st->pipeline, GST_STATE_NULL);
		gst_object_unref(st->pipeline);
		st->pipeline = NULL;
	}

	if (st->rtspsrc) {
		gst_object_unref(st->rtspsrc);
		st->rtspsrc = NULL;
	}

	if (st->sink) {
		gst_object_unref(st->sink);
		st->sink = NULL;
	}

	au_backchannel_unlink();

}


static void ausrc_destructor(void *arg)
{
	struct ausrc_st *st = arg;

	if (st->device)
		mem_deref(st->device);

	ausrcst = NULL;
	ausrc_st_clear(st);
}


static void auplay_destructor(void *arg)
{
	struct auplay_st *st = arg;
	info("rtsp: Stopping rtsp play.\n");
	if (re_atomic_rlx(&st->run)) {
		re_atomic_rlx_set(&st->run, false);
		thrd_join(st->thread, NULL);
	}

	if (st->buf)
		mem_deref(st->buf);

	au_backchannel_unlink();
}


static void vidsrc_destructor(void *arg)
{
	struct vidsrc_st *st = arg;

	vidsrcst = NULL;

	debug("rtsp: stopping video src read thread\n");
	re_atomic_rlx_set(&st->run, false);

	if (st->device)
		mem_deref(st->device);

	if (st->sink)
		gst_object_unref(st->sink);
}


static int au_format_check(struct ausrc_st *st, GstStructure *s)
{
	int rate, channels;
	const char *fmt = NULL;

	if (!st || !s)
		return EINVAL;

	fmt = gst_structure_get_string(s, "format");
	gst_structure_get_int(s, "rate", &rate);
	gst_structure_get_int(s, "channels", &channels);

	if ((int)st->prm.srate != rate) {
		warning("rtsp: expected %u Hz (got %u Hz)\n", st->prm.srate,
		        rate);
		return EINVAL;
	}

	if (st->prm.ch != channels) {
		warning("rtsp: expected %d channels (got %d)\n",
		        st->prm.ch, channels);
		return EINVAL;
	}

	if (str_cmp(fmt, "S16LE")) {
		warning("rtsp: expected S16LE format\n");
		return EINVAL;
	}

	return 0;
}


static int vid_format_check(struct vidsrc_st *st, GstStructure *s)
{
	int w, h;
	const char *fmt = NULL;

	if (!st || !s)
		return EINVAL;

	fmt = gst_structure_get_string(s, "format");
	gst_structure_get_int(s, "width", &w);
	gst_structure_get_int(s, "height", &h);

	if (w != (int)st->size.w) {
		warning("rtsp: unexpected video width\n");
		return EINVAL;
	}

	if (h != (int)st->size.h) {
		warning("rtsp: unexpected video height\n");
		return EINVAL;
	}

	if (str_cmp(fmt, "I420")) {
		warning("rtsp: unexpected video format\n");
		return EINVAL;
	}

	return 0;
}


/* Expected format: 16-bit signed PCM */
static void au_packet_handler(struct ausrc_st *st, GstBuffer *buffer)
{
	GstMapInfo info;
	struct auframe af;

	if (!re_atomic_rlx(&st->run))
		return;

	/* NOTE: When streaming from files, the buffer will be filled up
	 *       pretty quickly..
	 */

	if (!gst_buffer_map(buffer, &info, GST_MAP_READ)) {
		warning("rtsp: gst_buffer_map failed\n");
		return;
	}

	auframe_init(&af,
	             st->prm.fmt,
	             info.data,
	             info.size / aufmt_sample_size(st->prm.fmt),
	             st->prm.srate,
	             st->prm.ch);

	if (st->rh)
		st->rh(&af, st->arg);

	gst_buffer_unmap(buffer, &info);
}


static void vid_packet_handler(struct vidsrc_st *st, GstBuffer *buffer)
{
	struct vidframe frame;
	GstMapInfo info;

	if (!re_atomic_rlx(&st->run))
		return;

	if (!gst_buffer_map(buffer, &info, GST_MAP_READ)) {
		warning("rtsp: gst_buffer_map failed\n");
		return;
	}

	vidframe_init_buf(&frame, st->prm.fmt, &st->size, info.data);

	st->ts = GST_BUFFER_PTS(buffer) / 1000U;
	st->frameh(&frame, st->ts, st->arg);

	gst_buffer_unmap(buffer, &info);
}


static void au_handoff_handler(GstElement *sink,
                               GstBuffer *buffer,
                               GstPad *pad,
                               gpointer user_data)
{
	struct ausrc_st *st = user_data;
	GstCaps *caps;
	int err;
	(void)sink;
	caps = gst_pad_get_current_caps(pad);
	err = au_format_check(st, gst_caps_get_structure(caps, 0));
	gst_caps_unref(caps);
	if (err)
		return;

	au_packet_handler(st, buffer);
}


static void vid_handoff_handler(GstElement *sink,
				GstBuffer *buffer,
				GstPad *pad,
				gpointer user_data)
{
	struct vidsrc_st *st = (struct vidsrc_st*)user_data;
	GstCaps *caps;
	int err;
	(void)sink;
	caps = gst_pad_get_current_caps(pad);
	err = vid_format_check(st, gst_caps_get_structure(caps, 0));
	gst_caps_unref(caps);
	if (err)
		return;

	vid_packet_handler(st, buffer);
}


static GstFlowReturn au_new_out_sample(GstElement *appsink,
                                       void *userdata)
{
	GstObject *rtsp;
	GstSample *sample;
	GstFlowReturn r; /* Avoid warning, but we don't need use. */
	(void)userdata;

	g_signal_emit_by_name (appsink, "pull-sample", &sample);

	if (!sample) {
		warning("rtsp: No sample??\n");
		return GST_FLOW_OK;
	}

	mtx_lock(au_backchannel.lock);
	rtsp = (au_backchannel.rtsp)?gst_object_ref(
	           GST_OBJECT(au_backchannel.rtsp)):NULL;
	mtx_unlock(au_backchannel.lock);

	if (rtsp) {
		g_signal_emit_by_name (rtsp,
		                       "push-backchannel-sample",
		                       au_backchannel.stream_id,
		                       sample,
		                       &r);
		gst_object_unref(rtsp);
	}

	gst_sample_unref (sample);

	return GST_FLOW_OK;
}


static uint64_t au_take_samples(struct auplay_st *st)
{
	uint64_t sample_time;
	struct auframe af;
	GstFlowReturn ret;
	GstMapInfo meminfo;
	GstBuffer *buffer;
	GstObject *src;

	mtx_lock(au_backchannel.lock);
	src = GST_OBJECT(au_backchannel.src);
	src = (src)?gst_object_ref(src):NULL;
	mtx_unlock(au_backchannel.lock);

	if (!src) {
		auframe_init(&af, st->prm.fmt, st->buf,
		             st->sampc, st->prm.srate, st->prm.ch);
		sample_time = tmr_jiffies();
		af.timestamp = sample_time * 1000;
		st->wh(&af, st->arg);
		return sample_time;
	}

	buffer = gst_buffer_new_allocate(NULL,
	                                 au_backchannel.blocksize,
	                                 NULL);
	gst_buffer_map(buffer, &meminfo, GST_MAP_WRITE);
	auframe_init(&af, st->prm.fmt, meminfo.data,
	             st->sampc, st->prm.srate, st->prm.ch);
	sample_time = tmr_jiffies();
	af.timestamp = sample_time * 1000;
	st->wh(&af, st->arg);
	gst_buffer_unmap(buffer, &meminfo);
	g_signal_emit_by_name(src, "push-buffer",
	                      buffer, &ret);
	gst_buffer_unref(buffer);
	gst_object_unref(src);
	return sample_time;
}


static int au_write_thread(void *arg)
{
	struct auplay_st *st = arg;
	uint32_t ptime = st->prm.ptime;
	unsigned dt;

	while (re_atomic_rlx(&st->run)) {
		dt = ptime - (unsigned)(au_take_samples(st) - tmr_jiffies());
		if (dt <= 2)
			continue;

		sys_msleep(dt);
	}

	info("rtsp: Stopping write thread.\n");
	return 0;
}


static void au_backchannel_init(void)
{
	const gchar *outfmt = NULL;
	gchar *pipe_str;
	gint rate = 8000;
	guint channels = 1;
	const gchar *schannels = NULL;

	GstElement *src = NULL, *sink = NULL, *pipeline = NULL;

	GError *error = NULL;
	const GstStructure *s;

	const gchar *encoding = NULL;

	const char * encodings[][2] = {
		{"MPEG4-GENERIC", "voaacenc ! aacparse ! rtpmp4gpay"},
		{"MPEG4GENERIC", "voaacenc ! aacparse ! rtpmp4gpay"},
		{"PCMU", "mulawenc ! rtppcmupay"},
		{"PCMA", "alawenc ! rtppcmapay"}
	};

	mtx_lock(au_backchannel.lock);
	info("Trying to setup backchannel.\n");
	if (!au_backchannel.options_num ||
	    au_backchannel.option < 0 ||
	    (unsigned)au_backchannel.option >= au_backchannel.options_num) {
		info("rtsp: Backchannel not ready for init.\n");
		goto out;
	}

	if (au_backchannel.pipeline) {
		info("rtsp: Already has backchannel.\n");
		goto out;
	}

	au_backchannel.stream_id =
	    au_backchannel.options_streams[au_backchannel.option];
	s = gst_caps_get_structure (
	        au_backchannel.options_caps[au_backchannel.option], 0);
	encoding = gst_structure_get_string (s, "encoding-name");
	schannels = gst_structure_get_string (s, "channels");

	if (schannels)
		channels = (guint)strtoul(schannels, NULL, 10);

	info("rtsp: Setting up backchannel %u\n", au_backchannel.stream_id);

	if (encoding == NULL) {
		warning("rtsp: Could not setup backchannel pipeline: "
		        "Missing encoding-name field");
		goto out;
	}

	if (!gst_structure_get_int (s, "clock-rate", &rate)) {
		warning("rtsp: Could not setup backchannel pipeline: "
		        "Missing clock-rate field");
		goto out;
	}

	for (unsigned n = 0; n < RE_ARRAY_SIZE(encodings); n++) {
		if (g_str_equal(encoding, encodings[n][0])) {
			outfmt = encodings[n][1];
			break;
		}
	}

	if (!outfmt) {
		warning("rtsp: Could not setup backchannel pipeline: "
		        "Unsupported encoding %s", encoding);
		goto out;
	}

	pipe_str = g_strdup_printf(
	               "appsrc name=datawell blocksize=%u max-bytes=%u "
	               "caps=audio/x-raw,rate=(int)%u,channels=(int)%u,"
	               "format=(string)S16LE,layout=(string)interleaved ! "
	               "audioconvert ! audioresample ! audio/x-raw,"
	               "rate=(int)%u,channels=(int)%u,"
	               "format=(string)S16LE,layout=(string)interleaved "
	               "! %s ! appsink name=out",
	               au_backchannel.blocksize,
	               au_backchannel.blocksize * 2,
	               au_backchannel.src_rate,
	               au_backchannel.src_channels,
	               rate,
	               channels,
	               outfmt);
	info("rtsp: Backchannel : %s\n", pipe_str);
	pipeline = gst_parse_launch(pipe_str, &error);
	g_free (pipe_str);

	if (!pipeline) {
		warning("rtsp: Could not setup backchannel pipeline");
		if (error != NULL) {
			warning("rtsp: Error: %s", error->message);
			g_clear_error (&error);
		}
		goto out;
	}

	src = gst_bin_get_by_name(GST_BIN(pipeline), "datawell");
	sink = gst_bin_get_by_name (GST_BIN (pipeline), "out");

	if (!src || !sink) {
		warning("rtsp: Failed to get sink of pipeline.\n");
		goto out;
	}

	g_object_set(G_OBJECT(sink), "emit-signals", TRUE, NULL);
	g_signal_connect (G_OBJECT(sink), "new-sample",
	                  G_CALLBACK (au_new_out_sample), NULL);

	au_backchannel.pipeline = pipeline;
	au_backchannel.sink = sink;
	au_backchannel.src = src;
	src = pipeline = sink = NULL;

	info("rtsp: Playing backchannel shoveler\n");
	gst_element_set_state (au_backchannel.pipeline, GST_STATE_PLAYING);
out:
	if (pipeline)
		gst_object_unref(pipeline);

	if (sink)
		gst_object_unref(sink);

	if (src)
		gst_object_unref(src);

	mtx_unlock(au_backchannel.lock);
}


static gboolean au_remove_extra_fields(GQuark field_id,
                                       GValue *value G_GNUC_UNUSED,
                                       gpointer user_data G_GNUC_UNUSED)
{
	return !g_str_has_prefix(g_quark_to_string(field_id), "a-");
}


static gboolean au_find_backchannel(GstElement *rtspsrc,
                                    guint idx,
                                    GstCaps *caps,
                                    gpointer user_data G_GNUC_UNUSED)
{
	GstStructure *s = gst_caps_get_structure (caps, 0);
	char *channel_details = gst_structure_to_string(s);
	(void)rtspsrc;
	info ("rtsp: Channel: %u caps: %s\n", idx, channel_details);
	g_free(channel_details);
	if (gst_structure_has_field (s, "a-sendonly")) {

		unsigned options_num = au_backchannel.options_num;
		caps = gst_caps_new_empty ();
		s = gst_structure_copy (s);
		gst_structure_set_name (s, "application/x-rtp");
		gst_structure_filter_and_map_in_place(
		    s, au_remove_extra_fields, NULL);
		gst_caps_append_structure(caps, s);

		info("rtsp: Backchannel channel %u\n", idx);

		mtx_lock(au_backchannel.lock);
		au_backchannel.options_caps[options_num] = caps;
		au_backchannel.options_streams[options_num] = idx;
		if (au_backchannel.stream_id == (int)idx) {
			au_backchannel.option = options_num;
			info("rtsp: Target backchannel %u found.\n", idx);
		}

		au_backchannel.options_num++;
		mtx_unlock(au_backchannel.lock);

		if (au_backchannel.stream_id == (int)idx)
			au_backchannel_init();
	}

	return TRUE;
}


static void au_timeout(void *arg)
{
	struct ausrc_st *st = arg;
	tmr_start(&st->tmr, st->ptime ? st->ptime : 40, au_timeout, st);

	/* check if source is still running */
	if (!re_atomic_rlx(&st->run)) {
		tmr_cancel(&st->tmr);

		if (st->eos) {
			info("rtsp: end of file\n");
			/* error handler must be called from re_main thread */
			if (st->errh)
				st->errh(0, "end of file", st->arg);
		}
	}
}


static void source_setup(GstElement * bin,
                         GstElement * source,
                         gpointer udata)
{
	struct ausrc_st *st = (struct ausrc_st *)udata;
	(void)bin;

	if (str_cmp("GstRTSPSrc",
	            G_OBJECT_TYPE_NAME(source)) == 0) {

		info("rtsp: Found GstRTSPSrc\n");

		st->rtspsrc = gst_object_ref(source);

		g_object_set(source, "latency", st->ptime, NULL);

		/* Enum 1 is onvif */
		g_object_set(source, "backchannel", 1, NULL);

		g_signal_connect(st->rtspsrc, "select-stream",
		                 G_CALLBACK (au_find_backchannel), NULL);

		au_backchannel.rtsp = gst_object_ref(st->rtspsrc);
	}
	else {
		warning("rtsp: GstRTSPSrc not found\n");
	}
}


static int create_pipeline(GstElement ** pipeline,
                           struct vidsrc_st *vst,
                           struct ausrc_st *ast)
{
	char * pipe_str;
	char * video_str = NULL;
	GError *error = NULL;
	int err = 0;
	if (!pipeline || !ast)
		return EINVAL;

	if (vst) {
		const char * rotstr = "";
		unsigned rotation = 0;

		if (vst->prm.fmt != VID_FMT_YUV420P) {
			warning("Require YUV420P video format.\n");
			return EINVAL;
		}

		if (str_cmp(ast->device, vst->device)) {
			warning("rtsp: audio_source and video_source must "
		                "match for  video.\n");
			warning("rtsp: \"%s\" != \"%s\"\n",
			        ast->device, vst->device);
			return EINVAL;
		}

		conf_get_u32(conf_cur(), "rtsp_rotation", &rotation);

		if (rotation == 1)
			rotstr = "! videoflip method=clockwise";
		else if (rotation == 2)
			rotstr = "! videoflip method=rotate-180";
		else if (rotation == 3)
			rotstr = "! videoflip method=counterclockwise";

		err = re_sdprintf(&video_str,
		                  " pipestart. "
		                  "! queue %s ! videoconvert ! "
		                  "videorate ! videoscale ! "
		                  "video/x-raw,framerate=%u/1,"
		                  "format=I420,width=%u,height=%u "
		                  "! fakesink name=vidsink ",
		                  rotstr, (unsigned)vst->prm.fps,
		                  vst->size.w, vst->size.h);

		if (err) {
			warning("rtsp: video string creation failed : %s\n",
			        strerror(err));
			return err;
		}
	}

	err = re_sdprintf(&pipe_str,
	                  "uridecodebin3 uri=%s name=pipestart "
	                  "pipestart. "
	                  "! queue name=rtspaudioqueue "
	                  "! audioconvert ! audioresample "
	                  "! audio/x-raw,format=S16LE,"
	                  "rate=%u,channels=%u "
	                  "! fakesink name=ausink"
	                  "%s",
	                  ast->device,
	                  ast->prm.srate,
	                  ast->prm.ch,
	                  video_str ? video_str : "");

	if (video_str)
		mem_deref(video_str);

	if (err) {
		warning("rtsp: launch string creation failed : %s\n",
		        strerror(err));
		return err;
	}

	info("rtsp: src gst launch : %s\n", pipe_str);
	*pipeline = gst_parse_launch(pipe_str, &error);
	mem_deref(pipe_str);

	if (!*pipeline) {
		warning("rtsp: Could not setup pipeline\n");
		if (error != NULL) {
			warning("rtsp: Error: %s\n", error->message);
			g_clear_error (&error);
		}
		err = EINVAL;
	}

	return err;
}


static int rtsp_au_vid_src_setup(GstElement * pipeline,
                                 struct vidsrc_st *vst,
                                 struct ausrc_st *ast)
{
	GstElement * uridecodebin3 = gst_bin_get_by_name(GST_BIN(pipeline),
	                                                 "pipestart");
	GstElement * ausrcsink = gst_bin_get_by_name(GST_BIN(pipeline),
	                                             "ausink");
	GstElement * vidsrcsink = gst_bin_get_by_name(GST_BIN(pipeline),
	                                              "vidsink");

	if (!uridecodebin3) {
		warning("rtsp: Could not find src element of pipeline\n");
		goto badexit;
	}

	if (!ausrcsink) {
		warning("rtsp: Could not find sink element of pipeline\n");
		goto badexit;
	}

	if (vidsrcsink) {
		g_signal_connect(vidsrcsink, "handoff",
		                 G_CALLBACK(vid_handoff_handler), vst);
		/* Override audio-sink handoff handler */
		g_object_set(G_OBJECT(vidsrcsink),
		             "signal-handoffs", TRUE,
		             "async", FALSE,
		             NULL);
		vst->sink = vidsrcsink;
	}
	else if (vst) {
		warning("rtsp: Could not find video element of pipeline\n");
		goto badexit;
	}

	ast->sink = ausrcsink;
	re_atomic_rlx_set(&ast->run, true);
	ast->eos = false;
	g_signal_connect(ausrcsink, "handoff",
			 G_CALLBACK(au_handoff_handler), ast);
	/* Override audio-sink handoff handler */
	g_object_set(G_OBJECT(ausrcsink),
	             "signal-handoffs", TRUE,
	             "async", FALSE,
	             NULL);

	g_signal_connect (uridecodebin3, "source-setup",
	                  G_CALLBACK (source_setup), ast);
	tmr_start(&ast->tmr, ast->ptime, au_timeout, ast);
	gst_element_set_state(pipeline, GST_STATE_PLAYING);
	ast->pipeline = pipeline;

	return 0;
badexit:
	if (ausrcsink)
		gst_object_unref(ausrcsink);

	if (vidsrcsink)
		gst_object_unref(vidsrcsink);

	if (uridecodebin3)
		gst_object_unref(uridecodebin3);

	return EINVAL;
}


static void rtsp_au_vid_src_init(struct vidsrc_st *vst,
                                 struct ausrc_st *ast)
{
	int     err = 0;
	GstElement * pipeline = NULL;

	if (ast)
		ausrc_st_clear(ast);
	else
		return;

	err = create_pipeline(&pipeline, vst, ast);
	if (err)
		return;

	err = rtsp_au_vid_src_setup(pipeline, vst, ast);
	if (err)
		gst_object_unref(pipeline);

	return;
}


static int rtsp_check(const char *device) {
	if (!str_isset(device))
		return EINVAL;

	if (str_ncmp(device, "rtsp://", 7) &&
	    str_ncmp(device, "rtsps://", 8)) {
		warning("rtsp: Only rtsp(s) supported, given: %s\n",
		        device);
		return ENOTSUP;
	}
	return 0;
}


static int rtsp_ausrc_alloc(struct ausrc_st **stp,
                            const struct ausrc *as,
                            struct ausrc_prm *prm,
                            const char *device,
                            ausrc_read_h *rh,
                            ausrc_error_h *errh,
                            void *arg)
{
	struct ausrc_st *st;
	int err = 0;

	info("rtsp: Trying sourcing fron rtsp : %s\n", device);
	if (!stp || !as || !prm)
		return EINVAL;

	err = rtsp_check(device);
	if (err)
		return err;

	if (prm->fmt != AUFMT_S16LE) {
		warning("rtsp: unsupported sample format (%s)\n",
		        aufmt_name(prm->fmt));
		return ENOTSUP;
	}

	if (!prm->ptime)
		return EINVAL;

	st = mem_zalloc(sizeof(*st), ausrc_destructor);
	if (!st)
		return ENOMEM;

	st->rh   = rh;
	st->errh = errh;
	st->arg  = arg;

	st->ptime = prm->ptime;

	err = str_dup(&st->device, device);
	if (err)
		goto out;

	if (!prm->srate)
		prm->srate = 16000;

	if (!prm->ch)
		prm->ch = 1;

	st->prm   = *prm;
	st->sampc = prm->srate * prm->ch * st->ptime / 1000;
	st->psize = aufmt_sample_size(prm->fmt) * st->sampc;

	ausrcst = st;

	if (vidsrcst)
		info("rtsp: audio init and video init.\n");
	else
		info("rtsp: audio only init.\n");

	rtsp_au_vid_src_init(vidsrcst, ausrcst);
	if (!st->pipeline || !st->sink) {
		warning("rtsp: init failed.\n");
		err = EINVAL;
		goto out;
	}

out:
	if (err) {
		ausrc_destructor(st);
		mem_deref(st);
		ausrcst = NULL;
	}
	else
		*stp = st;

	return err;
}


static int rtsp_auplay_alloc(struct auplay_st **stp,
                             const struct auplay *ap,
                             struct auplay_prm *prm,
                             const char *device,
                             auplay_write_h *wh,
                             void *arg)
{
	struct auplay_st *st;
	int err = 0;

	info("rtsp: Trying backchannel %s of src rtsp.\n", device);
	if (!stp || !ap || !prm || !wh)
		return EINVAL;

	if (prm->fmt != AUFMT_S16LE) {
		warning("rtsp: unsupported sample format (%s)\n",
		        aufmt_name(prm->fmt));
		return ENOTSUP;
	}

	st = mem_zalloc(sizeof(*st), auplay_destructor);
	if (!st)
		return ENOMEM;

	st->prm = *prm;
	st->wh  = wh;
	st->arg = arg;

	st->sampc = prm->srate * prm->ch * prm->ptime / 1000;
	st->dsize = aufmt_sample_size(prm->fmt) * st->sampc;

	st->buf = mem_zalloc(st->dsize, NULL);
	if (!st->buf) {
		err = ENOMEM;
		goto out;
	}

	re_atomic_rlx_set(&st->run, true);
	err = thread_create_name(&st->thread, "ausrc", au_write_thread, st);
	if (err) {
		warning("rtsp: Failed to start pipeline thread.\n");
		re_atomic_rlx_set(&st->run, false);
		goto out;
	}

	au_backchannel.blocksize = (unsigned)st->dsize;
	au_backchannel.src_rate = prm->srate;
	au_backchannel.src_channels = prm->ch;
	au_backchannel.stream_id = (gint)strtol(device, NULL, 10);
	au_backchannel_init();
out:
	if (err) {
		auplay_destructor(st);
		mem_deref(st);
	}
	else
		*stp = st;

	return err;
}


static int rtsp_vidsrc_alloc(struct vidsrc_st **stp,
                             const struct vidsrc *vs,
                             struct vidsrc_prm *prm,
                             const struct vidsz *size,
                             const char *fmt,
                             const char *device, vidsrc_frame_h *frameh,
                             vidsrc_packet_h *packeth,
                             vidsrc_error_h *errorh, void *arg)
{
	struct vidsrc_st *st;
	int err = 0;

	(void)fmt;
	(void)packeth;
	(void)errorh;
	(void)vs;

	info("rtsp: Trying video from rtsp : %s\n", device);
	if (!stp || !prm || !size || !frameh)
		return EINVAL;

	err = rtsp_check(device);
	if (err)
		return err;

	st = mem_zalloc(sizeof(*st), vidsrc_destructor);
	if (!st)
		return ENOMEM;

	st->prm    = *prm;
	st->frameh = frameh;
	st->arg    = arg;
	st->size   = *size;
	re_atomic_rlx_set(&st->run, true);

	err = str_dup(&st->device, device);
	if (err)
		goto out;

	vidsrcst = st;

	rtsp_au_vid_src_init(vidsrcst, ausrcst);
	if (ausrcst && !vidsrcst->sink) {
		warning("rtsp: Failure during reinit with video.\n");
		mem_deref(st);
		st = NULL;
		err = EINVAL;
	}
out:
	if (err) {
		mem_deref(st);
		vidsrcst = NULL;
	}
	else
		*stp = st;

	return err;
}


static int mod_rtsp_init(void)
{
	int err;
	gchar *s;

	gst_init(0, NULL);

	s = gst_version_string();

	info("rtsp: gst version : %s\n", s);
	g_free(s);

	err = mutex_alloc(&au_backchannel.lock);
	if (err)
		return err;

	err = ausrc_register(&ausrc, baresip_ausrcl(),
	                     "rtsp", rtsp_ausrc_alloc);
	if (err)
		return err;

	err = auplay_register(&auplay, baresip_auplayl(),
	                      "rtsp", rtsp_auplay_alloc);
	if (err)
		return err;

	return vidsrc_register(&vidsrc, baresip_vidsrcl(),
	                       "rtsp", rtsp_vidsrc_alloc, NULL);
}


static int mod_rtsp_close(void)
{
	au_backchannel_unlink();
	ausrc = mem_deref(ausrc);
	auplay = mem_deref(auplay);
	vidsrc = mem_deref(vidsrc);

	info("rtsp: Stopping gst\n");
	gst_deinit();
	mem_deref(au_backchannel.lock);
	info("rtsp unloaded\n");
	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(rtsp) = {
	"rtsp",
	"sound/video",
	mod_rtsp_init,
	mod_rtsp_close
};
