#include "decoder.h"
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <glib.h>
#include <stdint.h>
#include "types.h"
#include "log.h"
#include "str.h"


struct decoder_s {
	AVCodecContext *avcctx;
	AVPacket avpkt;
	AVFrame *frame;
	unsigned long rtp_ts;
	uint64_t pts;
};


struct output_s {
	char *filename;

	// format params
	int clockrate;
	int channels;

	AVCodecContext *avcctx;
	AVFormatContext *fmtctx;
	AVStream *avst;
	AVPacket avpkt;
	AVAudioFifo *fifo;
	int64_t fifo_pts; // pts of first data in fifo
	int64_t mux_dts; // last dts passed to muxer
	AVFrame *frame;
};


struct decoder_def_s {
	const char *rtpname;
	int clockrate_mult;
	int avcodec_id;
	const char *avcodec_name;
};


#define DECODER_DEF_MULT_NAME(ref, id, mult, name) { \
	.rtpname = #ref, \
	.avcodec_id = AV_CODEC_ID_ ## id, \
	.clockrate_mult = mult, \
	.avcodec_name = #name, \
}
#define DECODER_DEF_MULT(ref, id, mult) DECODER_DEF_MULT_NAME(ref, id, mult, NULL)
#define DECODER_DEF_NAME(ref, id, name) DECODER_DEF_MULT_NAME(ref, id, 1, name)
#define DECODER_DEF(ref, id) DECODER_DEF_MULT(ref, id, 1)

static const struct decoder_def_s decoders[] = {
	DECODER_DEF(PCMA, PCM_ALAW),
	DECODER_DEF(PCMU, PCM_MULAW),
	DECODER_DEF(G723, G723_1),
	DECODER_DEF_MULT(G722, ADPCM_G722, 2),
	DECODER_DEF(QCELP, QCELP),
	DECODER_DEF(G729, G729),
	DECODER_DEF(speex, SPEEX),
	DECODER_DEF(GSM, GSM),
	DECODER_DEF(iLBC, ILBC),
	DECODER_DEF_NAME(opus, OPUS, libopus),
	DECODER_DEF_NAME(vorbis, VORBIS, libvorbis),
	DECODER_DEF(ac3, AC3),
	DECODER_DEF(eac3, EAC3),
	DECODER_DEF(ATRAC3, ATRAC3),
	DECODER_DEF(ATRAC-X, ATRAC3P),
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57, 0, 0)
	DECODER_DEF(EVRC, EVRC),
	DECODER_DEF(EVRC0, EVRC),
	DECODER_DEF(EVRC1, EVRC),
#endif
	DECODER_DEF(AMR, AMR_NB),
	DECODER_DEF(AMR-WB, AMR_WB),
};
typedef struct decoder_def_s decoder_def_t;



static int output_codec_id;
static const char *output_file_format;



static void output_shutdown(output_t *output);
static int output_config(output_t *output, unsigned int clockrate, unsigned int channels);



static const decoder_def_t *decoder_find(const str *name) {
	for (int i = 0; i < G_N_ELEMENTS(decoders); i++) {
		if (!str_cmp(name, decoders[i].rtpname))
			return &decoders[i];
	}
	return NULL;
}


decoder_t *decoder_new(const char *payload_str) {
	str name;
	char *slash = strchr(payload_str, '/');
	if (!slash) {
		ilog(LOG_WARN, "Invalid payload format: %s", payload_str);
		return NULL;
	}

	str_init_len(&name, (char *) payload_str, slash - payload_str);
	int clockrate = atoi(slash + 1);

	int channels = 1;
	slash = strchr(slash + 1, '/');
	if (slash) {
		channels = atoi(slash + 1);
		if (!channels)
			channels = 1;
	}

	const decoder_def_t *def = decoder_find(&name);
	if (!def) {
		ilog(LOG_WARN, "No decoder for payload %s", payload_str);
		return NULL;
	}
	clockrate *= def->clockrate_mult;

	decoder_t *ret = g_slice_alloc0(sizeof(*ret));

	// XXX error reporting
	AVCodec *codec = NULL;
	if (def->avcodec_name)
		codec = avcodec_find_decoder_by_name(def->avcodec_name);
	if (!codec)
		codec = avcodec_find_decoder(def->avcodec_id);

	ret->avcctx = avcodec_alloc_context3(codec);
	if (!ret->avcctx)
		goto err;
	ret->avcctx->channels = channels;
	ret->avcctx->sample_rate = clockrate;
	int i = avcodec_open2(ret->avcctx, codec, NULL);
	if (i)
		goto err;

	av_init_packet(&ret->avpkt);
	ret->frame = av_frame_alloc();
	if (!ret->frame)
		goto err;

	ret->pts = (uint64_t) -1LL;
	ret->rtp_ts = (unsigned long) -1L;

	return ret;

err:
	decoder_close(ret);
	return NULL;
}


static int output_flush(output_t *output) {
	while (av_audio_fifo_size(output->fifo) >= output->frame->nb_samples) {

		if (av_audio_fifo_read(output->fifo, (void **) output->frame->data,
					output->frame->nb_samples) <= 0)
			abort();

		dbg("%p output fifo pts %lu", output, (unsigned long) output->fifo_pts);
		output->frame->pts = output->fifo_pts;

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57, 0, 0)
		int ret = avcodec_send_frame(output->avcctx, output->frame);
		dbg("%p send frame ret %i", output, ret);
		if (ret)
			return -1;

		ret = avcodec_receive_packet(output->avcctx, &output->avpkt);
		dbg("%p receive packet ret %i", output, ret);
		if (ret)
			return -1;
#else
		int got_packet = 0;
		int ret = avcodec_encode_audio2(output->avcctx, &output->avpkt, output->frame, &got_packet);
		dbg("%p encode frame ret %i, got packet %i", output, ret, got_packet);
		if (!got_packet)
			return 0;
#endif

		dbg("%p output avpkt size is %i", output, (int) output->avpkt.size);
		dbg("%p output pkt pts/dts is %li/%li", output, (long) output->avpkt.pts,
				(long) output->avpkt.dts);
		dbg("%p output dts %li", output, (long) output->mux_dts);

		// the encoder may return frames with the same dts multiple consecutive times.
		// the muxer may not like this, so ensure monotonically increasing dts.
		if (output->mux_dts > output->avpkt.dts)
			output->avpkt.dts = output->mux_dts;
		if (output->avpkt.pts < output->avpkt.dts)
			output->avpkt.pts = output->avpkt.dts;

		av_write_frame(output->fmtctx, &output->avpkt);

		output->fifo_pts += output->frame->nb_samples;
		output->mux_dts = output->avpkt.dts + 1; // min next expected dts
	}

	return 0;
}


static int output_add(output_t *output, AVFrame *frame) {
	if (!output)
		return -1;

	dbg("%p output fifo size %u fifo_pts %lu", output, (unsigned int) av_audio_fifo_size(output->fifo),
			(unsigned long) output->fifo_pts);
	// fix up output pts
	if (av_audio_fifo_size(output->fifo) == 0)
		output->fifo_pts = frame->pts;

	if (av_audio_fifo_write(output->fifo, (void **) frame->extended_data, frame->nb_samples) < 0)
		return -1;

	return output_flush(output);
}


int decoder_input(decoder_t *dec, const str *data, unsigned long ts, output_t *output) {
	if (G_UNLIKELY(!dec))
		return -1;

	dbg("%p dec pts %lu rtp_ts %lu incoming ts %lu", dec, (unsigned long) dec->pts,
			(unsigned long) dec->rtp_ts, (unsigned long) ts);

	if (G_UNLIKELY(dec->rtp_ts == (unsigned long) -1L)) {
		// initialize pts
		dec->pts = 0;
	}
	else {
		// shift pts according to rtp ts shift
		dec->pts += (ts - dec->rtp_ts) /* * output->avst->time_base.num * 8000 / output->avst->time_base.den */ ;
		// XXX handle lost packets here if timestamps don't line up?
	}
	dec->rtp_ts = ts;

	dec->avpkt.data = (unsigned char *) data->s;
	dec->avpkt.size = data->len;
	dec->avpkt.pts = dec->pts;

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57, 0, 0)
	int ret = avcodec_send_packet(dec->avcctx, &dec->avpkt);
	dbg("send packet ret %i", ret);
	if (ret)
		return -1;

	ret = avcodec_receive_frame(dec->avcctx, dec->frame);
	dbg("receive frame ret %i", ret);
	if (ret)
		return -1;
#else
	int got_frame = 0;
	int ret = avcodec_decode_audio4(dec->avcctx, dec->frame, &got_frame, &dec->avpkt);
	dbg("decode frame ret %i, got frame %i", ret, got_frame);
	if (!got_frame)
		return 0;
#endif

	dec->frame->pts = dec->frame->pkt_pts;
	dbg("%p dec frame pts %lu pkt_pts %lu", dec, (unsigned long) dec->frame->pts,
			(unsigned long) dec->frame->pkt_dts);

	output_config(output, dec->avcctx->sample_rate, dec->avcctx->channels);
	if (output_add(output, dec->frame))
		return -1;

	return 0;
}


output_t *output_new(const char *filename) {
	output_t *ret = g_slice_alloc0(sizeof(*ret));
	if (asprintf(&ret->filename, "%s.%s", filename, output_file_format) <= 0)
		abort();
	ret->clockrate = -1;
	ret->channels = -1;
	ret->frame = av_frame_alloc();
	return ret;
}


static int output_config(output_t *output, unsigned int clockrate, unsigned int channels) {
	// anything to do?
	if (G_UNLIKELY(output->clockrate != clockrate))
		goto format_mismatch;
	if (G_UNLIKELY(output->channels != channels))
		goto format_mismatch;

	// all good
	return 0;

format_mismatch:
	// XXX support reset/config change

	// copy params
	output->clockrate = clockrate;
	output->channels = channels;

	// XXX error reporting
	output->fmtctx = avformat_alloc_context();
	if (!output->fmtctx)
		goto err;
	output->fmtctx->oformat = av_guess_format(output_file_format, NULL, NULL);
	if (!output->fmtctx->oformat)
		goto err;

	AVCodec *codec = avcodec_find_encoder(output_codec_id);
	// XXX error handling
	output->avst = avformat_new_stream(output->fmtctx, codec);
	if (!output->avst)
		goto err;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57, 0, 0)
	output->avcctx = avcodec_alloc_context3(codec);
	if (!output->avcctx)
		goto err;
#else
	output->avcctx = output->avst->codec;
#endif

	output->avcctx->channels = output->channels;
	output->avcctx->channel_layout = av_get_default_channel_layout(output->channels);
	output->avcctx->sample_rate = output->clockrate;
	output->avcctx->sample_fmt = AV_SAMPLE_FMT_S16;
	output->avcctx->time_base = (AVRational){output->clockrate,1};
	output->avst->time_base = output->avcctx->time_base;

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57, 0, 0)
	avcodec_parameters_from_context(output->avst->codecpar, output->avcctx);
#endif

	int i = avcodec_open2(output->avcctx, codec, NULL);
	if (i)
		goto err;
	i = avio_open(&output->fmtctx->pb, output->filename, AVIO_FLAG_WRITE);
	if (i < 0)
		goto err;
	i = avformat_write_header(output->fmtctx, NULL);
	if (i)
		goto err;

	av_init_packet(&output->avpkt);

	// output frame and fifo
	output->frame->nb_samples = output->avcctx->frame_size ? : 256;
	output->frame->format = output->avcctx->sample_fmt;
	output->frame->sample_rate = output->avcctx->sample_rate;
	output->frame->channel_layout = output->avcctx->channel_layout;
	if (!output->frame->channel_layout)
		output->frame->channel_layout = av_get_default_channel_layout(output->avcctx->channels);
	if (av_frame_get_buffer(output->frame, 0) < 0)
		abort();

	output->fifo = av_audio_fifo_alloc(output->avcctx->sample_fmt, output->avcctx->channels,
			output->frame->nb_samples);

	return 0;

err:
	output_shutdown(output);
	return -1;
}


void decoder_close(decoder_t *dec) {
	if (!dec)
		return;
	avcodec_free_context(&dec->avcctx);
	av_frame_free(&dec->frame);
	g_slice_free1(sizeof(*dec), dec);
}


static void output_shutdown(output_t *output) {
	if (!output)
		return;
	av_write_trailer(output->fmtctx);
	avcodec_close(output->avcctx);
	avio_closep(&output->fmtctx->pb);
	avformat_free_context(output->fmtctx);
	av_audio_fifo_free(output->fifo);
	av_frame_free(&output->frame);

	output->avcctx = NULL;
	output->fmtctx = NULL;
	output->avst = NULL;
	output->fifo = NULL;
}


void output_close(output_t *output) {
	if (!output)
		return;
	output_shutdown(output);
	free(output->filename);
	g_slice_free1(sizeof(*output), output);
}


void output_init(const char *format) {
	if (!strcmp(format, "wav")) {
		output_codec_id = AV_CODEC_ID_PCM_S16LE;
		output_file_format = "wav";
	}
	else if (!strcmp(format, "mp3")) {
		output_codec_id = AV_CODEC_ID_MP3;
		output_file_format = "mp3";
	}
	else
		die("Unknown output format '%s'", format);
}