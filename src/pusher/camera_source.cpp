#include "pusher/camera_source.hpp"
#include "pusher/url_utils.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <cctype>
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace pusher {
namespace {

static std::string av_error(int ret) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(ret, buf, sizeof(buf));
    return std::string(buf);
}

#ifdef _WIN32
static bool starts_with_case_insensitive(const std::string &value, const std::string &prefix) {
    if (value.size() < prefix.size()) {
        return false;
    }
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(value[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}
#endif

static const AVInputFormat *camera_input_format(const std::string &input) {
#ifdef _WIN32
    if (starts_with_case_insensitive(input, "video=") ||
        starts_with_case_insensitive(input, "audio=")) {
        return av_find_input_format("dshow");
    }
    return nullptr;
#else
    if (pusher_has_prefix(input.c_str(), "/dev/video") ||
        pusher_has_prefix(input.c_str(), "/dev/v4l/")) {
        const AVInputFormat *format = av_find_input_format("v4l2");
        if (format == nullptr) {
            format = av_find_input_format("video4linux2");
        }
        return format;
    }
    return nullptr;
#endif
}

static bool is_raw_like_video(enum AVCodecID codec_id) {
    return codec_id == AV_CODEC_ID_RAWVIDEO ||
           codec_id == AV_CODEC_ID_MJPEG ||
           codec_id == AV_CODEC_ID_MJPEGB ||
           codec_id == AV_CODEC_ID_H264;
}

static std::vector<const AVCodec *> h264_encoder_candidates() {
    std::vector<const AVCodec *> candidates;
    const char *preferred[] = {
        "libx264",
        "libopenh264",
        "h264_mf",
        "h264_v4l2m2m",
        "h264_nvenc",
        "h264_qsv",
    };
    for (const char *name : preferred) {
        const AVCodec *codec = avcodec_find_encoder_by_name(name);
        if (codec != nullptr) {
            candidates.push_back(codec);
        }
    }
    const AVCodec *native = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (native != nullptr &&
        std::find(candidates.begin(), candidates.end(), native) == candidates.end()) {
        candidates.push_back(native);
    }
    return candidates;
}

static int interrupt_callback(void *opaque) {
    auto *stop_requested = static_cast<std::atomic<bool> *>(opaque);
    return stop_requested != nullptr && stop_requested->load() ? 1 : 0;
}

static void set_camera_options(AVDictionary **opts,
                               const CameraSourceConfig &config,
                               bool prefer_h264) {
    av_dict_set(opts, "video_size",
                (std::to_string(config.width) + "x" + std::to_string(config.height)).c_str(), 0);
    av_dict_set(opts, "framerate", std::to_string(config.fps).c_str(), 0);
    av_dict_set(opts, "analyzeduration", std::to_string(config.analyzeduration_us).c_str(), 0);
    av_dict_set(opts, "probesize", std::to_string(config.probesize).c_str(), 0);
    av_dict_set(opts, "stimeout", std::to_string(config.timeout_ms * 1000).c_str(), 0);
    av_dict_set(opts, "rw_timeout", std::to_string(config.timeout_ms * 1000).c_str(), 0);
    if (prefer_h264) {
        av_dict_set(opts, "input_format", "h264", 0);
        av_dict_set(opts, "pixel_format", "h264", 0);
        av_dict_set(opts, "vcodec", "h264", 0);
    }
}

}  // namespace

bool is_camera_input(const std::string &input) {
#ifdef _WIN32
    return starts_with_case_insensitive(input, "video=") ||
           starts_with_case_insensitive(input, "audio=");
#else
    return pusher_has_prefix(input.c_str(), "/dev/video") ||
           pusher_has_prefix(input.c_str(), "/dev/v4l/");
#endif
}

struct CameraH264Source::Impl {
    explicit Impl(CameraSourceConfig cfg) : config(cfg) {}

    CameraSourceConfig config;
    std::atomic<bool> *stop_requested = nullptr;
    AVFormatContext *input_ctx = nullptr;
    AVCodecContext *decoder_ctx = nullptr;
    AVCodecContext *encoder_ctx = nullptr;
    SwsContext *sws_ctx = nullptr;
    AVPacket *read_pkt = nullptr;
    AVFrame *decoded_frame = nullptr;
    AVFrame *encode_frame = nullptr;
    AVCodecParameters *encoded_params = nullptr;
    int video_index = -1;
    bool direct_h264 = false;
    int64_t frame_index = 0;

    ~Impl() {
        close();
    }

    void close() {
        if (read_pkt != nullptr) {
            av_packet_free(&read_pkt);
        }
        if (decoded_frame != nullptr) {
            av_frame_free(&decoded_frame);
        }
        if (encode_frame != nullptr) {
            av_frame_free(&encode_frame);
        }
        if (sws_ctx != nullptr) {
            sws_freeContext(sws_ctx);
            sws_ctx = nullptr;
        }
        if (encoded_params != nullptr) {
            avcodec_parameters_free(&encoded_params);
        }
        if (encoder_ctx != nullptr) {
            avcodec_free_context(&encoder_ctx);
        }
        if (decoder_ctx != nullptr) {
            avcodec_free_context(&decoder_ctx);
        }
        if (input_ctx != nullptr) {
            avformat_close_input(&input_ctx);
        }
        video_index = -1;
        direct_h264 = false;
        frame_index = 0;
    }

    int open_input(const std::string &input, bool prefer_h264, std::string &error) {
        AVFormatContext *ctx = avformat_alloc_context();
        if (ctx == nullptr) {
            error = "allocate camera input context failed";
            return 1;
        }
        ctx->interrupt_callback.callback = interrupt_callback;
        ctx->interrupt_callback.opaque = stop_requested;
        ctx->flags |= AVFMT_FLAG_GENPTS;

        AVDictionary *opts = nullptr;
        set_camera_options(&opts, config, prefer_h264);
        const AVInputFormat *format = camera_input_format(input);
        if (format == nullptr) {
            avformat_free_context(ctx);
            av_dict_free(&opts);
            error = "camera input format is unavailable for: " + input;
            return 1;
        }

        int ret = avformat_open_input(&ctx, input.c_str(), format, &opts);
        av_dict_free(&opts);
        if (ret < 0) {
            avformat_free_context(ctx);
            error = "open camera input failed: " + av_error(ret);
            return 1;
        }

        ret = avformat_find_stream_info(ctx, nullptr);
        if (ret < 0) {
            avformat_close_input(&ctx);
            error = "find camera stream info failed: " + av_error(ret);
            return 1;
        }

        int found_video = -1;
        for (unsigned int i = 0; i < ctx->nb_streams; ++i) {
            if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                found_video = static_cast<int>(i);
                break;
            }
        }
        if (found_video < 0) {
            avformat_close_input(&ctx);
            error = "camera input has no video stream";
            return 1;
        }

        AVCodecParameters *codecpar = ctx->streams[found_video]->codecpar;
        if (!is_raw_like_video(codecpar->codec_id)) {
            avformat_close_input(&ctx);
            error = "unsupported camera codec: " + std::to_string(codecpar->codec_id);
            return 1;
        }

        input_ctx = ctx;
        video_index = found_video;
        direct_h264 = codecpar->codec_id == AV_CODEC_ID_H264;
        return 0;
    }

    int setup_passthrough_or_codec(std::string &error) {
        AVStream *video_stream = input_ctx->streams[video_index];
        AVCodecParameters *codecpar = video_stream->codecpar;
        if (direct_h264) {
            return 0;
        }

        const AVCodec *decoder = avcodec_find_decoder(codecpar->codec_id);
        if (decoder == nullptr) {
            error = "camera decoder is unavailable for codec id: " + std::to_string(codecpar->codec_id);
            return 1;
        }

        decoder_ctx = avcodec_alloc_context3(decoder);
        if (decoder_ctx == nullptr) {
            error = "allocate camera decoder failed";
            return 1;
        }
        int ret = avcodec_parameters_to_context(decoder_ctx, codecpar);
        if (ret < 0) {
            error = "copy camera decoder parameters failed: " + av_error(ret);
            return 1;
        }
        ret = avcodec_open2(decoder_ctx, decoder, nullptr);
        if (ret < 0) {
            error = "open camera decoder failed: " + av_error(ret);
            return 1;
        }

        std::vector<const AVCodec *> encoders = h264_encoder_candidates();
        if (encoders.empty()) {
            error = "no native H264 encoder is available; rebuild FFmpeg SDK with libx264, openh264, or platform H264 encoder";
            return 1;
        }

        int width = codecpar->width > 0 ? codecpar->width : config.width;
        int height = codecpar->height > 0 ? codecpar->height : config.height;
        std::string encoder_errors;
        for (const AVCodec *encoder : encoders) {
            AVCodecContext *candidate_ctx = avcodec_alloc_context3(encoder);
            if (candidate_ctx == nullptr) {
                encoder_errors += std::string(encoder->name) + ": allocate failed; ";
                continue;
            }
            candidate_ctx->width = width;
            candidate_ctx->height = height;
            candidate_ctx->time_base = AVRational{1, config.fps};
            candidate_ctx->framerate = AVRational{config.fps, 1};
            candidate_ctx->bit_rate = config.bitrate;
            candidate_ctx->gop_size = std::max(1, config.fps * 2);
            candidate_ctx->max_b_frames = 0;
            candidate_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
            candidate_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            av_opt_set(candidate_ctx->priv_data, "preset", "veryfast", 0);
            av_opt_set(candidate_ctx->priv_data, "tune", "zerolatency", 0);
            av_opt_set(candidate_ctx->priv_data, "profile", "baseline", 0);

            ret = avcodec_open2(candidate_ctx, encoder, nullptr);
            if (ret == 0) {
                encoder_ctx = candidate_ctx;
                break;
            }
            encoder_errors += std::string(encoder->name) + ": " + av_error(ret) + "; ";
            avcodec_free_context(&candidate_ctx);
        }
        if (encoder_ctx == nullptr) {
            error = "open H264 encoder failed. Tried: " + encoder_errors;
            return 1;
        }
        encoded_params = avcodec_parameters_alloc();
        if (encoded_params == nullptr) {
            error = "allocate H264 encoder parameters failed";
            return 1;
        }
        ret = avcodec_parameters_from_context(encoded_params, encoder_ctx);
        if (ret < 0) {
            error = "copy H264 encoder parameters failed: " + av_error(ret);
            return 1;
        }

        decoded_frame = av_frame_alloc();
        encode_frame = av_frame_alloc();
        if (decoded_frame == nullptr || encode_frame == nullptr) {
            error = "allocate camera frames failed";
            return 1;
        }

        encode_frame->format = encoder_ctx->pix_fmt;
        encode_frame->width = encoder_ctx->width;
        encode_frame->height = encoder_ctx->height;
        ret = av_frame_get_buffer(encode_frame, 32);
        if (ret < 0) {
            error = "allocate H264 encode frame buffer failed: " + av_error(ret);
            return 1;
        }
        return 0;
    }

    int open(const std::string &input, std::atomic<bool> &stop, std::string &error) {
        close();
        stop_requested = &stop;
        avdevice_register_all();
        avformat_network_init();

        std::string direct_error;
        if (open_input(input, true, direct_error) != 0) {
            close();
            std::string raw_error;
            if (open_input(input, false, raw_error) != 0) {
                error = direct_error + "; fallback raw camera open failed: " + raw_error;
                return 1;
            }
        }

        read_pkt = av_packet_alloc();
        if (read_pkt == nullptr) {
            error = "allocate camera packet failed";
            return 1;
        }
        return setup_passthrough_or_codec(error);
    }

    int send_frame_to_encoder(AVFrame *frame, std::string &error) {
        int ret = av_frame_make_writable(encode_frame);
        if (ret < 0) {
            error = "make H264 encode frame writable failed: " + av_error(ret);
            return ret;
        }

        if (frame->format != encoder_ctx->pix_fmt ||
            frame->width != encoder_ctx->width ||
            frame->height != encoder_ctx->height) {
            sws_ctx = sws_getCachedContext(
                sws_ctx,
                frame->width,
                frame->height,
                static_cast<AVPixelFormat>(frame->format),
                encoder_ctx->width,
                encoder_ctx->height,
                encoder_ctx->pix_fmt,
                SWS_BILINEAR,
                nullptr,
                nullptr,
                nullptr);
            if (sws_ctx == nullptr) {
                error = "create camera pixel converter failed";
                return AVERROR(EINVAL);
            }
            sws_scale(sws_ctx,
                      frame->data,
                      frame->linesize,
                      0,
                      frame->height,
                      encode_frame->data,
                      encode_frame->linesize);
        } else {
            const uint8_t *src_data[4] = {
                frame->data[0],
                frame->data[1],
                frame->data[2],
                frame->data[3],
            };
            av_image_copy(encode_frame->data,
                          encode_frame->linesize,
                          src_data,
                          frame->linesize,
                          encoder_ctx->pix_fmt,
                          encoder_ctx->width,
                          encoder_ctx->height);
        }

        encode_frame->pts = frame->best_effort_timestamp != AV_NOPTS_VALUE
            ? av_rescale_q(frame->best_effort_timestamp,
                           input_ctx->streams[video_index]->time_base,
                           encoder_ctx->time_base)
            : frame_index++;
        return avcodec_send_frame(encoder_ctx, encode_frame);
    }

    int read_direct_packet(AVPacket *out, std::string &error) {
        while (stop_requested == nullptr || !stop_requested->load()) {
            int ret = av_read_frame(input_ctx, out);
            if (ret < 0) {
                error = "read camera packet failed: " + av_error(ret);
                return ret;
            }
            if (out->stream_index == video_index) {
                return 0;
            }
            av_packet_unref(out);
        }
        return AVERROR_EXIT;
    }

    int read_encoded_packet(AVPacket *out, std::string &error) {
        while (stop_requested == nullptr || !stop_requested->load()) {
            int ret = avcodec_receive_packet(encoder_ctx, out);
            if (ret == 0) {
                out->stream_index = 0;
                return 0;
            }
            if (ret != AVERROR(EAGAIN)) {
                if (ret == AVERROR_EOF) {
                    return ret;
                }
                error = "receive H264 encoder packet failed: " + av_error(ret);
                return ret;
            }

            ret = av_read_frame(input_ctx, read_pkt);
            if (ret < 0) {
                error = "read camera frame packet failed: " + av_error(ret);
                return ret;
            }
            if (read_pkt->stream_index != video_index) {
                av_packet_unref(read_pkt);
                continue;
            }

            ret = avcodec_send_packet(decoder_ctx, read_pkt);
            av_packet_unref(read_pkt);
            if (ret < 0) {
                error = "send camera packet to decoder failed: " + av_error(ret);
                return ret;
            }

            while ((ret = avcodec_receive_frame(decoder_ctx, decoded_frame)) == 0) {
                ret = send_frame_to_encoder(decoded_frame, error);
                av_frame_unref(decoded_frame);
                if (ret < 0 && ret != AVERROR(EAGAIN)) {
                    if (error.empty()) {
                        error = "send frame to H264 encoder failed: " + av_error(ret);
                    }
                    return ret;
                }
            }
            if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
                error = "receive camera decoded frame failed: " + av_error(ret);
                return ret;
            }
        }
        return AVERROR_EXIT;
    }
};

CameraH264Source::CameraH264Source(CameraSourceConfig config) : impl_(new Impl(config)) {}

CameraH264Source::~CameraH264Source() {
    delete impl_;
}

int CameraH264Source::open(const std::string &input,
                           std::atomic<bool> &stop_requested,
                           std::string &error) {
    return impl_->open(input, stop_requested, error);
}

int CameraH264Source::read_packet(AVPacket *out, std::string &error) {
    if (impl_->direct_h264) {
        return impl_->read_direct_packet(out, error);
    }
    return impl_->read_encoded_packet(out, error);
}

const AVCodecParameters *CameraH264Source::codec_parameters() const {
    if (impl_->direct_h264) {
        return impl_->input_ctx->streams[impl_->video_index]->codecpar;
    }
    return impl_->encoded_params;
}

AVRational CameraH264Source::time_base() const {
    if (impl_->direct_h264) {
        return impl_->input_ctx->streams[impl_->video_index]->time_base;
    }
    return impl_->encoder_ctx == nullptr ? AVRational{1, impl_->config.fps} : impl_->encoder_ctx->time_base;
}

bool CameraH264Source::copy_mode() const {
    return impl_->direct_h264;
}

}  // namespace pusher
