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
 * Audio source/player module using gstreamer 1.0
 *
 * This module implements an audio source/player which uses the
 * Gstreamer framework to bi-directionally stream audio from/to
 * an RTSP device
 *
 * Example config:
 \verbatim
  audio_source        rtsp,rtsp://someuser:somepw@someserver/ch0
  audio_player        rtsp,<channel-num>

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

	int16_t *buf;
	struct tmr tmr;

	GstElement *pipeline;
	GstElement *rtspsrc;
	GstElement *fakesink;
};

struct auplay_st {
	size_t sampc;
	size_t dsize;
	int16_t *buf;
	auplay_write_h *wh;
	void *arg;
	struct auplay_prm prm;
	RE_ATOMIC bool	run;
	pthread_t	thread;
};

struct backchannel_t {
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

static struct ausrc *ausrc;
static struct auplay *auplay;

static struct backchannel_t backchannel = {.stream_id = -1, .option=-1};


static void backchannel_unlink(void)
{
	info("rtsp: backchannel_unlink\n");
	mtx_lock(backchannel.lock);
	if (backchannel.pipeline) {
		gst_element_set_state(backchannel.pipeline, GST_STATE_NULL);
		gst_object_unref(backchannel.pipeline);
		backchannel.pipeline = NULL;
	}

	if (backchannel.src) {
		gst_object_unref(GST_OBJECT(backchannel.src));
		backchannel.src = NULL;
	}

	if (backchannel.rtsp) {
		gst_object_unref(GST_OBJECT(backchannel.rtsp));
		backchannel.rtsp = NULL;
	}

	if (backchannel.sink) {
		gst_object_unref(backchannel.sink);
		backchannel.sink = NULL;
	}

	for (unsigned n = 0; n < backchannel.options_num; n++) {
		gst_caps_unref(backchannel.options_caps[n]);
	}

	backchannel.option = -1;
	backchannel.options_num = 0;
	mtx_unlock(backchannel.lock);
}


static void ausrc_destructor(void *arg)
{
	struct ausrc_st *st = arg;

	info("rtsp: Stopping rtsp source.\n");
	re_atomic_rlx_set(&st->run, false);

	tmr_cancel(&st->tmr);

	if (st->pipeline) {
		gst_element_set_state(st->pipeline, GST_STATE_NULL);
		gst_object_unref(st->pipeline);
	}

	if (st->rtspsrc)
		gst_object_unref(st->rtspsrc);

	if (st->fakesink)
		gst_object_unref(st->fakesink);

	if (st->buf)
		mem_deref(st->buf);

	backchannel_unlink();
}


static void auplay_destructor(void *arg)
{
	struct auplay_st *st = arg;
	info("rtsp: Stopping rtsp play.\n");
	if (re_atomic_rlx(&st->run)) {
		re_atomic_rlx_set(&st->run, false);
		pthread_join(st->thread, NULL);
	}

	if (st->buf)
		mem_deref(st->buf);

	backchannel_unlink();
}


static int format_check(struct ausrc_st *st, GstStructure *s)
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

	if (strcmp(fmt, "S16LE")) {
		warning("rtsp: expected S16LE format\n");
		return EINVAL;
	}

	return 0;
}


/* Expected format: 16-bit signed PCM */
static void packet_handler(struct ausrc_st *st, GstBuffer *buffer)
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


static void handoff_handler(GstElement *sink, GstBuffer *buffer,
                            GstPad *pad, gpointer user_data)
{
	struct ausrc_st *st = user_data;
	GstCaps *caps;
	int err;
	(void)sink;
	caps = gst_pad_get_current_caps(pad);
	err = format_check(st, gst_caps_get_structure(caps, 0));
	gst_caps_unref(caps);
	if (err)
		return;

	packet_handler(st, buffer);
}


static GstFlowReturn new_out_sample(GstElement *appsink,
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

	mtx_lock(backchannel.lock);
	rtsp = (backchannel.rtsp)?gst_object_ref(
	           GST_OBJECT(backchannel.rtsp)):NULL;
	mtx_unlock(backchannel.lock);

	if (rtsp) {
		g_signal_emit_by_name (rtsp,
		                       "push-backchannel-sample",
		                       backchannel.stream_id,
		                       sample,
		                       &r);
		gst_object_unref(rtsp);
	}

	gst_sample_unref (sample);

	return GST_FLOW_OK;
}


static uint64_t take_samples(struct auplay_st *st) {
	uint64_t sample_time;
	struct auframe af;
	GstFlowReturn ret;
	GstMapInfo meminfo;
	GstBuffer *buffer;
	GstObject *src;

	mtx_lock(backchannel.lock);
	src = GST_OBJECT(backchannel.src);
	src = (src)?gst_object_ref(src):NULL;
	mtx_unlock(backchannel.lock);

	if (!src) {
		auframe_init(&af, st->prm.fmt, st->buf,
		             st->sampc, st->prm.srate, st->prm.ch);
		sample_time = tmr_jiffies();
		af.timestamp = sample_time * 1000;
		st->wh(&af, st->arg);
		return sample_time;
	}

	buffer = gst_buffer_new_allocate(NULL,
	                                 backchannel.blocksize,
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


static void *write_thread(void *arg)
{
	struct auplay_st *st = arg;
	uint32_t ptime = st->prm.ptime;
	unsigned dt;

	while (re_atomic_rlx(&st->run)) {
		dt = ptime - (unsigned)(take_samples(st) - tmr_jiffies());
		if (dt <= 2)
			continue;

		sys_msleep(dt);
	}

	info("rtsp: Stopping write thread.\n");
	return NULL;
}


static void backchannel_init(void)
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

	mtx_lock(backchannel.lock);
	info("Trying to setup backchannel.\n");
	if (!backchannel.options_num ||
	        backchannel.option < 0 ||
	        (unsigned)backchannel.option >= backchannel.options_num) {
		info("rtsp: Backchannel not ready for init.\n");
		goto out;
	}

	if (backchannel.pipeline) {
		info("rtsp: Already has backchannel.\n");
		goto out;
	}

	backchannel.stream_id =
	    backchannel.options_streams[backchannel.option];
	s = gst_caps_get_structure (
	        backchannel.options_caps[backchannel.option], 0);
	encoding = gst_structure_get_string (s, "encoding-name");
	schannels = gst_structure_get_string (s, "channels");

	if (schannels)
		channels = (guint)strtoul(schannels, NULL, 10);

	info("rtsp: Setting up backchannel %u\n", backchannel.stream_id);

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
	               "rate=(int)%u,channels=(int)%u,format=(string)S16LE,"
	               "layout=(string)interleaved ! %s ! appsink name=out",
	               backchannel.blocksize,
	               backchannel.blocksize * 2,
	               backchannel.src_rate,
	               backchannel.src_channels,
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
	                  G_CALLBACK (new_out_sample), NULL);

	backchannel.pipeline = pipeline;
	backchannel.sink = sink;
	backchannel.src = src;
	src = pipeline = sink = NULL;

	info("rtsp: Playing backchannel shoveler\n");
	gst_element_set_state (backchannel.pipeline, GST_STATE_PLAYING);
out:
	if (pipeline)
		gst_object_unref(pipeline);

	if (sink)
		gst_object_unref(sink);

	if (src)
		gst_object_unref(src);

	mtx_unlock(backchannel.lock);
}


static gboolean
remove_extra_fields (GQuark field_id, GValue *value G_GNUC_UNUSED,
                     gpointer user_data G_GNUC_UNUSED)
{
	return !g_str_has_prefix(g_quark_to_string(field_id), "a-");
}


static gboolean
find_backchannel (GstElement *rtspsrc, guint idx, GstCaps *caps,
                  gpointer user_data G_GNUC_UNUSED)
{
	GstStructure *s = gst_caps_get_structure (caps, 0);
	char *channel_details = gst_structure_to_string(s);
	(void)rtspsrc;
	info ("rtsp: Channel: %u caps: %s\n", idx, channel_details);
	g_free(channel_details);
	if (gst_structure_has_field (s, "a-sendonly")) {

		caps = gst_caps_new_empty ();
		s = gst_structure_copy (s);
		gst_structure_set_name (s, "application/x-rtp");
		gst_structure_filter_and_map_in_place(
		    s, remove_extra_fields, NULL);
		gst_caps_append_structure(caps, s);

		info("rtsp: Backchannel channel %u\n", idx);

		mtx_lock(backchannel.lock);
		backchannel.options_caps[backchannel.options_num] = caps;
		backchannel.options_streams[backchannel.options_num] = idx;
		if (backchannel.stream_id == (int)idx) {
			backchannel.option = backchannel.options_num;
			info("rtsp: Target backchannel %u found.\n", idx);
		}

		backchannel.options_num++;
		mtx_unlock(backchannel.lock);

		if (backchannel.stream_id == (int)idx)
			backchannel_init();
	}

	return TRUE;
}


static void timeout(void *arg)
{
	struct ausrc_st *st = arg;
	tmr_start(&st->tmr, st->ptime ? st->ptime : 40, timeout, st);

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

	if (g_strcmp0 ("GstRTSPSrc",
			G_OBJECT_TYPE_NAME(source)) == 0) {

		info("rtsp: Found GstRTSPSrc\n");

		st->rtspsrc = gst_object_ref(source);

		g_object_set(source, "latency", st->ptime, NULL);

		/* Enum 1 is onvif */
		g_object_set(source, "backchannel", 1, NULL);

		g_signal_connect (st->rtspsrc, "select-stream",
	                  G_CALLBACK (find_backchannel), NULL);

		backchannel.rtsp = gst_object_ref(st->rtspsrc);
	}
	else {
		warning("rtsp: GstRTSPSrc not found\n");
	}
}


static int rtsp_src_alloc(struct ausrc_st **stp, const struct ausrc *as,
                          struct ausrc_prm *prm, const char *device,
                          ausrc_read_h *rh, ausrc_error_h *errh, void *arg)
{
	struct ausrc_st *st;
	int err = 0;
	gchar *pipe_str;
	GstElement *uridecodebin3;

	info("rtsp: Trying sourcing fron rtsp : %s\n", device);
	if (!stp || !as || !prm)
		return EINVAL;

	if (!str_isset(device))
		return EINVAL;

	if (prm->fmt != AUFMT_S16LE) {
		warning("rtsp: unsupported sample format (%s)\n",
		        aufmt_name(prm->fmt));
		return ENOTSUP;
	}

	if (strncmp(device, "rtsp://", 7) &&
	        strncmp(device, "rtsps://", 8)) {
		warning("rtsp: Only rtsp(s) supported.\n");
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

	if (!prm->srate)
		prm->srate = 16000;

	if (!prm->ch)
		prm->ch = 1;

	st->prm   = *prm;
	st->sampc = prm->srate * prm->ch * st->ptime / 1000;
	st->psize = aufmt_sample_size(prm->fmt) * st->sampc;

	st->buf = mem_zalloc(st->psize, NULL);
	if (!st->buf) {
		err = ENOMEM;
		goto out;
	}

	pipe_str = g_strdup_printf (
	        "uridecodebin3 name=pipestart uri=%s"
	        " ! audioconvert ! audioresample ! "
	        "audio/x-raw,format=S16LE,rate=%u,channels=%u "
	        "! fakesink name=pipeend",
	        device, prm->srate, prm->ch);

	info("rtsp: src gst launch : %s\n", pipe_str);
	st->pipeline = gst_parse_launch (pipe_str, NULL);
	g_free (pipe_str);

	if (!st->pipeline) {
		warning("rtsp: Failed gst rtsp pipeline.\n");
		err = EINVAL;
		goto out;
	}

	uridecodebin3 = gst_bin_get_by_name(GST_BIN(st->pipeline),
	                                    "pipestart");
	if (!uridecodebin3) {
		warning("rtsp: Failed gst pipeline start.\n");
		err = EINVAL;
		goto out;
	}

	st->fakesink = gst_bin_get_by_name(GST_BIN(st->pipeline), "pipeend");
	if (!st->fakesink) {
		warning("rtsp: Failed gst pipeline end.\n");
		err = EINVAL;
		goto out;
	}

	re_atomic_rlx_set(&st->run, true);
	st->eos = false;
	g_signal_connect(st->fakesink, "handoff",
	                 G_CALLBACK(handoff_handler), st);
	/* Override audio-sink handoff handler */
	g_object_set(G_OBJECT(st->fakesink),
	             "signal-handoffs", TRUE,
	             "async", FALSE,
	             NULL);

	g_signal_connect (uridecodebin3, "source-setup",
			  G_CALLBACK (source_setup), st);

	tmr_start(&st->tmr, st->ptime, timeout, st);
	gst_element_set_state(st->pipeline, GST_STATE_PLAYING);

out:
	if (err)
		ausrc_destructor(st);
	else
		*stp = st;

	return err;
}


static int rtsp_play_alloc(struct auplay_st **stp, const struct auplay *ap,
                           struct auplay_prm *prm, const char *device,
                           auplay_write_h *wh, void *arg)
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
	err = pthread_create(&st->thread, NULL, write_thread, st);
	if (err) {
		warning("rtsp: Failed to start pipeline thread.\n");
		re_atomic_rlx_set(&st->run, false);
		goto out;
	}

	backchannel.blocksize = (unsigned)st->dsize;
	backchannel.src_rate = prm->srate;
	backchannel.src_channels = prm->ch;
	backchannel.stream_id = (gint)strtol(device, NULL, 10);
	backchannel_init();
out:
	if (err)
		auplay_destructor(st);
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

	err = mutex_alloc(&backchannel.lock);
	if (err)
		return err;

	err = ausrc_register(&ausrc, baresip_ausrcl(),
	                     "rtsp", rtsp_src_alloc);
	if (err)
		return err;

	return auplay_register(&auplay, baresip_auplayl(),
	                       "rtsp", rtsp_play_alloc);
}


static int mod_rtsp_close(void)
{
	backchannel_unlink();
	ausrc = mem_deref(ausrc);
	auplay = mem_deref(auplay);

	info("rtsp: Stopping gst\n");
	gst_deinit();
	mem_deref(backchannel.lock);
	info("rtsp unloaded\n");
	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(rtsp) = {
	"rtsp",
	"sound",
	mod_rtsp_init,
	mod_rtsp_close
};
