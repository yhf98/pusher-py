#include "pusher/pusher.hpp"
#include "pusher/camera_source.hpp"
#include "pusher/stream_push.hpp"
#include "pusher/url_utils.h"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <sys/stat.h>

extern "C" {
#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/time.h>
}

namespace pusher {

struct WorkerState {
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> running{false};
    std::atomic<bool> finished{false};
    std::mutex mutex;
    std::condition_variable cv;
    int exit_code = -1;
    std::string error;
};

namespace {

static bool is_stream_input(const std::string &input) {
    return pusher_has_prefix(input.c_str(), "rtsp://") ||
           pusher_has_prefix(input.c_str(), "rtmp://") ||
           pusher_has_prefix(input.c_str(), "http://") ||
           pusher_has_prefix(input.c_str(), "https://") ||
           pusher_has_prefix(input.c_str(), "srt://");
}

static bool is_local_file_input(const std::string &input) {
    return !input.empty() && !is_camera_input(input) && !is_stream_input(input);
}

static bool file_exists(const std::string &path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0;
}

static std::string shell_join(const std::vector<std::string> &command) {
    std::ostringstream oss;
    for (size_t i = 0; i < command.size(); ++i) {
        if (i > 0) {
            oss << ' ';
        }
        const std::string &arg = command[i];
        bool quote = arg.empty();
        for (char c : arg) {
            if (c == ' ' || c == '\'' || c == '"' || c == '&' || c == '?' || c == ';' || c == '(' || c == ')') {
                quote = true;
                break;
            }
        }
        if (!quote) {
            oss << arg;
            continue;
        }
        oss << '\'';
        for (char c : arg) {
            if (c == '\'') {
                oss << "'\\''";
            } else {
                oss << c;
            }
        }
        oss << '\'';
    }
    return oss.str();
}

static std::string av_error(int ret) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(ret, buf, sizeof(buf));
    return std::string(buf);
}

static const char *output_format_for_protocol(const std::string &protocol) {
    if (protocol == "rtmp") return "flv";
    if (protocol == "rtsp") return "rtsp";
    if (protocol == "srt") return "mpegts";
    if (protocol == "rtp") return "rtp";
    return nullptr;
}

static int libav_interrupt_callback(void *opaque) {
    auto *state = static_cast<WorkerState *>(opaque);
    return state != nullptr && state->stop_requested.load() ? 1 : 0;
}

static void log_line(const std::string &log_path, const std::string &message) {
    if (log_path.empty()) {
        return;
    }
    std::ofstream out(log_path, std::ios::app);
    if (out) {
        out << message << '\n';
    }
}

static void finish_worker_state(const std::shared_ptr<WorkerState> &state,
                                int exit_code,
                                const std::string &error) {
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->exit_code = exit_code;
        state->error = error;
        state->running.store(false);
        state->finished.store(true);
    }
    state->cv.notify_all();
}

static int64_t default_packet_duration(const AVStream *in_stream,
                                       const AVStream *out_stream) {
    if (in_stream == nullptr || out_stream == nullptr) {
        return 1;
    }

    if (in_stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        AVRational frame_rate = in_stream->avg_frame_rate;
        if (frame_rate.num <= 0 || frame_rate.den <= 0) {
            frame_rate = in_stream->r_frame_rate;
        }
        if (frame_rate.num > 0 && frame_rate.den > 0) {
            int64_t duration = av_rescale_q(1, av_inv_q(frame_rate), out_stream->time_base);
            return duration > 0 ? duration : 1;
        }
    }

    if (in_stream->time_base.num > 0 && in_stream->time_base.den > 0 &&
        out_stream->time_base.num > 0 && out_stream->time_base.den > 0) {
        int64_t duration = av_rescale_q(1, in_stream->time_base, out_stream->time_base);
        return duration > 0 ? duration : 1;
    }

    return 1;
}

static bool should_keep_stream(const AVStream *stream) {
    return stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ||
           stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO;
}

static void run_libav_camera_push(std::shared_ptr<WorkerState> state,
                                  PusherConfig config,
                                  std::string input,
                                  std::string output_url,
                                  std::string protocol) {
    state->running.store(true);
    state->finished.store(false);
    log_line(config.log_path, "[camera] start input=" + input + " output=" + output_url);

    AVFormatContext *output_ctx = nullptr;
    AVDictionary *output_opts = nullptr;
    AVPacket *pkt = nullptr;
    bool header_written = false;
    int ret = 0;
    int exit_code = 0;
    std::string error;

    CameraSourceConfig camera_config;
    camera_config.width = config.width;
    camera_config.height = config.height;
    camera_config.fps = config.fps;
    camera_config.bitrate = config.bitrate;
    camera_config.timeout_ms = config.timeout_ms;
    camera_config.analyzeduration_us = config.analyzeduration_us;
    camera_config.probesize = config.probesize;
    CameraH264Source camera(camera_config);

    avformat_network_init();

    ret = camera.open(input, state->stop_requested, error);
    if (ret != 0) {
        exit_code = 1;
        goto cleanup;
    }

    {
        const char *format_name = output_format_for_protocol(protocol);
        if (format_name == nullptr) {
            error = "unsupported camera output protocol: " + protocol;
            exit_code = 1;
            goto cleanup;
        }

        ret = avformat_alloc_output_context2(&output_ctx, nullptr, format_name, output_url.c_str());
        if (ret < 0 || output_ctx == nullptr) {
            error = "alloc output context failed: " + av_error(ret);
            exit_code = 1;
            goto cleanup;
        }
        output_ctx->interrupt_callback.callback = libav_interrupt_callback;
        output_ctx->interrupt_callback.opaque = state.get();
        output_ctx->max_interleave_delta = 0;
        output_ctx->avoid_negative_ts = AVFMT_AVOID_NEG_TS_MAKE_ZERO;

        AVStream *out_stream = avformat_new_stream(output_ctx, nullptr);
        if (out_stream == nullptr) {
            error = "create camera output stream failed";
            exit_code = 1;
            goto cleanup;
        }
        const AVCodecParameters *camera_params = camera.codec_parameters();
        if (camera_params == nullptr) {
            error = "camera H264 parameters are unavailable";
            exit_code = 1;
            goto cleanup;
        }
        ret = avcodec_parameters_copy(out_stream->codecpar, camera_params);
        if (ret < 0) {
            error = "copy camera H264 parameters failed: " + av_error(ret);
            exit_code = 1;
            goto cleanup;
        }
        out_stream->codecpar->codec_tag = 0;
        out_stream->time_base = camera.time_base();

        if (!(output_ctx->oformat->flags & AVFMT_NOFILE)) {
            ret = avio_open2(&output_ctx->pb, output_url.c_str(), AVIO_FLAG_WRITE,
                             &output_ctx->interrupt_callback, nullptr);
            if (ret < 0) {
                error = "open output failed: " + av_error(ret);
                exit_code = 1;
                goto cleanup;
            }
        }

        if (protocol == "rtsp") {
            av_dict_set(&output_opts, "rtsp_transport", "tcp", 0);
        }
        av_dict_set(&output_opts, "rw_timeout", std::to_string(config.timeout_ms * 1000).c_str(), 0);

        ret = avformat_write_header(output_ctx, &output_opts);
        av_dict_free(&output_opts);
        if (ret < 0) {
            error = "write output header failed: " + av_error(ret);
            exit_code = 1;
            goto cleanup;
        }
        header_written = true;
    }

    pkt = av_packet_alloc();
    if (pkt == nullptr) {
        error = "allocate packet failed";
        exit_code = 1;
        goto cleanup;
    }

    {
        AVStream *out_stream = output_ctx->streams[0];
        int64_t next_pts = 0;
        int64_t frame_duration = av_rescale_q(1, AVRational{1, config.fps}, out_stream->time_base);
        if (frame_duration <= 0) {
            frame_duration = 1;
        }

        while (!state->stop_requested.load()) {
            ret = camera.read_packet(pkt, error);
            if (ret == AVERROR_EXIT || state->stop_requested.load()) {
                break;
            }
            if (ret < 0) {
                if (error.empty()) {
                    error = "read camera H264 packet failed: " + av_error(ret);
                }
                exit_code = 1;
                break;
            }

            AVRational camera_time_base = camera.time_base();
            if (pkt->dts != AV_NOPTS_VALUE) {
                pkt->dts = av_rescale_q_rnd(pkt->dts, camera_time_base, out_stream->time_base,
                                            static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
            }
            if (pkt->pts != AV_NOPTS_VALUE) {
                pkt->pts = av_rescale_q_rnd(pkt->pts, camera_time_base, out_stream->time_base,
                                            static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
            }
            pkt->duration = av_rescale_q(pkt->duration, camera_time_base, out_stream->time_base);
            if (pkt->duration <= 0) {
                pkt->duration = frame_duration;
            }
            if (pkt->dts == AV_NOPTS_VALUE && pkt->pts == AV_NOPTS_VALUE) {
                pkt->dts = next_pts;
                pkt->pts = next_pts;
            } else if (pkt->dts == AV_NOPTS_VALUE) {
                pkt->dts = pkt->pts;
            } else if (pkt->pts == AV_NOPTS_VALUE) {
                pkt->pts = pkt->dts;
            }
            if (pkt->pts < pkt->dts) {
                pkt->pts = pkt->dts;
            }
            next_pts = std::max(next_pts, pkt->dts + pkt->duration);
            pkt->stream_index = 0;
            pkt->pos = -1;

            ret = av_interleaved_write_frame(output_ctx, pkt);
            av_packet_unref(pkt);
            if (ret < 0) {
                if (state->stop_requested.load()) {
                    break;
                }
                error = "write camera output packet failed: " + av_error(ret);
                exit_code = 1;
                break;
            }
        }
    }

cleanup:
    if (pkt != nullptr) {
        av_packet_free(&pkt);
    }
    if (output_ctx != nullptr) {
        if (header_written && !state->stop_requested.load()) {
            av_write_trailer(output_ctx);
        }
        if (!(output_ctx->oformat->flags & AVFMT_NOFILE) && output_ctx->pb != nullptr) {
            avio_closep(&output_ctx->pb);
        }
        avformat_free_context(output_ctx);
    }
    av_dict_free(&output_opts);

    if (state->stop_requested.load() && exit_code == 0) {
        error.clear();
    }
    if (!error.empty()) {
        log_line(config.log_path, "[camera] error: " + error);
    }
    log_line(config.log_path, "[camera] stop exit_code=" + std::to_string(exit_code));
    finish_worker_state(state, exit_code, error);
}

static void run_libav_remux(std::shared_ptr<WorkerState> state,
                            PusherConfig config,
                            std::string input,
                            std::string output_url,
                            std::string protocol) {
    state->running.store(true);
    state->finished.store(false);
    log_line(config.log_path, "[libav] start input=" + input + " output=" + output_url);

    AVFormatContext *input_ctx = nullptr;
    AVFormatContext *output_ctx = nullptr;
    AVDictionary *input_opts = nullptr;
    AVDictionary *output_opts = nullptr;
    AVPacket *pkt = nullptr;
    std::vector<int> stream_mapping;
    std::vector<int64_t> ts_base;
    std::vector<int64_t> ts_offset;
    std::vector<int64_t> last_ts;
    std::vector<int64_t> last_dts;
    std::vector<int64_t> next_dts;
    std::vector<int64_t> default_duration;
    bool header_written = false;
    int ret = 0;
    int exit_code = 0;
    std::string error;

    avformat_network_init();

    if (is_stream_input(input)) {
        av_dict_set(&input_opts, "rtsp_transport", "tcp", 0);
        av_dict_set(&input_opts, "analyzeduration", std::to_string(config.analyzeduration_us).c_str(), 0);
        av_dict_set(&input_opts, "probesize", std::to_string(config.probesize).c_str(), 0);
        av_dict_set(&input_opts, "stimeout", std::to_string(config.timeout_ms * 1000).c_str(), 0);
        av_dict_set(&input_opts, "rw_timeout", std::to_string(config.timeout_ms * 1000).c_str(), 0);
    }

    ret = avformat_open_input(&input_ctx, input.c_str(), nullptr, &input_opts);
    av_dict_free(&input_opts);
    if (ret < 0) {
        error = "open input failed: " + av_error(ret);
        exit_code = 1;
        goto cleanup;
    }
    input_ctx->interrupt_callback.callback = libav_interrupt_callback;
    input_ctx->interrupt_callback.opaque = state.get();
    input_ctx->flags |= AVFMT_FLAG_GENPTS;

    ret = avformat_find_stream_info(input_ctx, nullptr);
    if (ret < 0) {
        error = "find input stream info failed: " + av_error(ret);
        exit_code = 1;
        goto cleanup;
    }

    {
        const char *format_name = output_format_for_protocol(protocol);
        if (format_name == nullptr) {
            error = "unsupported libav output protocol: " + protocol;
            exit_code = 1;
            goto cleanup;
        }

        ret = avformat_alloc_output_context2(&output_ctx, nullptr, format_name, output_url.c_str());
        if (ret < 0 || output_ctx == nullptr) {
            error = "alloc output context failed: " + av_error(ret);
            exit_code = 1;
            goto cleanup;
        }
        output_ctx->interrupt_callback.callback = libav_interrupt_callback;
        output_ctx->interrupt_callback.opaque = state.get();
        output_ctx->max_interleave_delta = 0;
        output_ctx->avoid_negative_ts = AVFMT_AVOID_NEG_TS_MAKE_ZERO;

        stream_mapping.assign(input_ctx->nb_streams, -1);
        for (unsigned int i = 0; i < input_ctx->nb_streams; ++i) {
            AVStream *in_stream = input_ctx->streams[i];
            if (!should_keep_stream(in_stream)) {
                continue;
            }

            AVStream *out_stream = avformat_new_stream(output_ctx, nullptr);
            if (out_stream == nullptr) {
                error = "create output stream failed";
                exit_code = 1;
                goto cleanup;
            }

            ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
            if (ret < 0) {
                error = "copy codec parameters failed: " + av_error(ret);
                exit_code = 1;
                goto cleanup;
            }
            out_stream->codecpar->codec_tag = 0;
            out_stream->time_base = in_stream->time_base;
            stream_mapping[i] = static_cast<int>(out_stream->index);
        }

        if (output_ctx->nb_streams == 0) {
            error = "input has no audio/video stream";
            exit_code = 1;
            goto cleanup;
        }

        ts_base.assign(output_ctx->nb_streams, AV_NOPTS_VALUE);
        ts_offset.assign(output_ctx->nb_streams, 0);
        last_ts.assign(output_ctx->nb_streams, 0);
        last_dts.assign(output_ctx->nb_streams, AV_NOPTS_VALUE);
        next_dts.assign(output_ctx->nb_streams, 0);
        default_duration.assign(output_ctx->nb_streams, 1);

        if (!(output_ctx->oformat->flags & AVFMT_NOFILE)) {
            ret = avio_open2(&output_ctx->pb, output_url.c_str(), AVIO_FLAG_WRITE,
                             &output_ctx->interrupt_callback, nullptr);
            if (ret < 0) {
                error = "open output failed: " + av_error(ret);
                exit_code = 1;
                goto cleanup;
            }
        }

        if (protocol == "rtsp") {
            av_dict_set(&output_opts, "rtsp_transport", "tcp", 0);
        }
        av_dict_set(&output_opts, "rw_timeout", std::to_string(config.timeout_ms * 1000).c_str(), 0);

        ret = avformat_write_header(output_ctx, &output_opts);
        av_dict_free(&output_opts);
        if (ret < 0) {
            error = "write output header failed: " + av_error(ret);
            exit_code = 1;
            goto cleanup;
        }
        header_written = true;

        for (unsigned int i = 0; i < input_ctx->nb_streams; ++i) {
            int out_index = stream_mapping[i];
            if (out_index < 0) {
                continue;
            }
            default_duration[out_index] = default_packet_duration(
                input_ctx->streams[i],
                output_ctx->streams[out_index]);
        }
    }

    pkt = av_packet_alloc();
    if (pkt == nullptr) {
        error = "allocate packet failed";
        exit_code = 1;
        goto cleanup;
    }

    {
        const bool local_file = is_local_file_input(input);
        int64_t wall_start_us = 0;
        int64_t media_start_us = AV_NOPTS_VALUE;

        while (!state->stop_requested.load()) {
            ret = av_read_frame(input_ctx, pkt);
            if (ret == AVERROR_EOF && config.loop && local_file) {
                for (size_t i = 0; i < ts_offset.size(); ++i) {
                    ts_offset[i] = last_ts[i] + 1;
                    ts_base[i] = AV_NOPTS_VALUE;
                }
                media_start_us = AV_NOPTS_VALUE;
                wall_start_us = 0;
                avformat_seek_file(input_ctx, -1, INT64_MIN, 0, INT64_MAX, 0);
                avformat_flush(input_ctx);
                continue;
            }
            if (ret == AVERROR_EOF) {
                break;
            }
            if (ret < 0) {
                if (state->stop_requested.load()) {
                    break;
                }
                error = "read input packet failed: " + av_error(ret);
                exit_code = 1;
                break;
            }

            int in_index = pkt->stream_index;
            if (in_index < 0 || static_cast<size_t>(in_index) >= stream_mapping.size() || stream_mapping[in_index] < 0) {
                av_packet_unref(pkt);
                continue;
            }

            AVStream *in_stream = input_ctx->streams[in_index];
            AVStream *out_stream = output_ctx->streams[stream_mapping[in_index]];
            int out_index = stream_mapping[in_index];

            if (config.realtime && local_file) {
                int64_t media_ts_us = AV_NOPTS_VALUE;
                if (pkt->dts != AV_NOPTS_VALUE) {
                    media_ts_us = av_rescale_q(pkt->dts, in_stream->time_base, AV_TIME_BASE_Q);
                } else if (pkt->pts != AV_NOPTS_VALUE) {
                    media_ts_us = av_rescale_q(pkt->pts, in_stream->time_base, AV_TIME_BASE_Q);
                }
                if (media_ts_us != AV_NOPTS_VALUE) {
                    if (media_start_us == AV_NOPTS_VALUE) {
                        media_start_us = media_ts_us;
                        wall_start_us = av_gettime_relative();
                    } else {
                        int64_t target_us = wall_start_us + (media_ts_us - media_start_us);
                        while (!state->stop_requested.load()) {
                            int64_t delay_us = target_us - av_gettime_relative();
                            if (delay_us <= 0) {
                                break;
                            }
                            av_usleep(delay_us > 100000 ? 100000 : delay_us);
                        }
                    }
                }
            }

            if (pkt->dts != AV_NOPTS_VALUE) {
                pkt->dts = av_rescale_q_rnd(pkt->dts, in_stream->time_base, out_stream->time_base,
                                            static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
            }
            if (pkt->pts != AV_NOPTS_VALUE) {
                pkt->pts = av_rescale_q_rnd(pkt->pts, in_stream->time_base, out_stream->time_base,
                                            static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
            }
            if (ts_base[out_index] == AV_NOPTS_VALUE) {
                if (pkt->dts != AV_NOPTS_VALUE) {
                    ts_base[out_index] = pkt->dts;
                } else if (pkt->pts != AV_NOPTS_VALUE) {
                    ts_base[out_index] = pkt->pts;
                } else {
                    ts_base[out_index] = 0;
                }
            }
            if (pkt->dts != AV_NOPTS_VALUE) {
                pkt->dts = pkt->dts - ts_base[out_index] + ts_offset[out_index];
            }
            if (pkt->pts != AV_NOPTS_VALUE) {
                pkt->pts = pkt->pts - ts_base[out_index] + ts_offset[out_index];
                if (pkt->dts != AV_NOPTS_VALUE && pkt->pts < pkt->dts) {
                    pkt->pts = pkt->dts;
                }
            }
            pkt->duration = av_rescale_q(pkt->duration, in_stream->time_base, out_stream->time_base);
            if (pkt->duration <= 0) {
                pkt->duration = default_duration[out_index];
            }

            if (pkt->dts == AV_NOPTS_VALUE && pkt->pts == AV_NOPTS_VALUE) {
                pkt->dts = next_dts[out_index];
                pkt->pts = pkt->dts;
            } else if (pkt->dts == AV_NOPTS_VALUE) {
                pkt->dts = pkt->pts;
            } else if (pkt->pts == AV_NOPTS_VALUE) {
                pkt->pts = pkt->dts;
            }

            if (pkt->dts != AV_NOPTS_VALUE) {
                if (last_dts[out_index] != AV_NOPTS_VALUE && pkt->dts <= last_dts[out_index]) {
                    int64_t step = pkt->duration > 0 ? pkt->duration : 1;
                    pkt->dts = last_dts[out_index] + step;
                }
                last_dts[out_index] = pkt->dts;
            }
            if (pkt->pts != AV_NOPTS_VALUE && pkt->dts != AV_NOPTS_VALUE && pkt->pts < pkt->dts) {
                pkt->pts = pkt->dts;
            }
            pkt->pos = -1;
            pkt->stream_index = out_index;

            int64_t tail = 0;
            if (pkt->dts != AV_NOPTS_VALUE) {
                tail = pkt->dts;
            }
            if (pkt->pts != AV_NOPTS_VALUE && pkt->pts > tail) {
                tail = pkt->pts;
            }
            if (pkt->duration > 0) {
                tail += pkt->duration;
            }
            if (tail > last_ts[out_index]) {
                last_ts[out_index] = tail;
            }
            if (pkt->dts != AV_NOPTS_VALUE) {
                int64_t next = pkt->dts + (pkt->duration > 0 ? pkt->duration : default_duration[out_index]);
                if (next > next_dts[out_index]) {
                    next_dts[out_index] = next;
                }
            }

            ret = av_interleaved_write_frame(output_ctx, pkt);
            av_packet_unref(pkt);
            if (ret < 0) {
                if (state->stop_requested.load()) {
                    break;
                }
                error = "write output packet failed: " + av_error(ret);
                exit_code = 1;
                break;
            }
        }
    }

cleanup:
    if (pkt != nullptr) {
        av_packet_free(&pkt);
    }
    if (output_ctx != nullptr) {
        if (header_written && !state->stop_requested.load()) {
            av_write_trailer(output_ctx);
        }
        if (!(output_ctx->oformat->flags & AVFMT_NOFILE) && output_ctx->pb != nullptr) {
            avio_closep(&output_ctx->pb);
        }
        avformat_free_context(output_ctx);
    }
    if (input_ctx != nullptr) {
        avformat_close_input(&input_ctx);
    }
    av_dict_free(&input_opts);
    av_dict_free(&output_opts);

    if (state->stop_requested.load() && exit_code == 0) {
        error.clear();
    }
    if (!error.empty()) {
        log_line(config.log_path, "[libav] error: " + error);
    }
    log_line(config.log_path, "[libav] stop exit_code=" + std::to_string(exit_code));
    finish_worker_state(state, exit_code, error);
}

static void run_whip_worker(std::shared_ptr<WorkerState> state,
                            PusherConfig config,
                            std::string input,
                            std::string output_url) {
    state->running.store(true);
    state->finished.store(false);
    log_line(config.log_path, "[stream_push] start input=" + input + " output=" + output_url);

    StreamPushConfig whip_config;
    whip_config.loop = config.loop;
    whip_config.realtime = config.realtime;
    whip_config.width = config.width;
    whip_config.height = config.height;
    whip_config.fps = config.fps;
    whip_config.bitrate = config.bitrate;
    whip_config.timeout_ms = config.timeout_ms;
    whip_config.analyzeduration_us = config.analyzeduration_us;
    whip_config.probesize = config.probesize;
    whip_config.log_path = config.log_path;

    std::string error;
    int exit_code = run_whip_push(whip_config, input, output_url, state->stop_requested, error);
    if (!error.empty()) {
        log_line(config.log_path, "[stream_push] error: " + error);
    }
    log_line(config.log_path, "[stream_push] stop exit_code=" + std::to_string(exit_code));
    finish_worker_state(state, exit_code, error);
}

}  // namespace

NativePusher::NativePusher(PusherConfig config) : config_(std::move(config)) {
    if (config_.name.empty()) {
        throw std::invalid_argument("pusher name cannot be empty");
    }
    if (config_.timeout_ms <= 0) {
        throw std::invalid_argument("timeout_ms must be greater than 0");
    }
    if (config_.engine != "auto" && config_.engine != "libav" && config_.engine != "stream_push") {
        throw std::invalid_argument("engine must be one of: auto, libav, stream_push");
    }
    if (config_.width <= 0 || config_.height <= 0 || config_.fps <= 0) {
        throw std::invalid_argument("width, height and fps must be greater than 0");
    }
    if (config_.bitrate <= 0) {
        throw std::invalid_argument("bitrate must be greater than 0");
    }
}

NativePusher::~NativePusher() {
    try {
        stop(1500);
    } catch (...) {
    }
}

std::vector<std::string> NativePusher::build_command(const std::string &input,
                                                     const std::string &output_url,
                                                     const std::string &protocol) const {
    std::string engine = config_.engine;
    if (engine == "auto") {
        engine = protocol == "whip" ? "stream_push" : "libav";
    }

    if (engine == "libav") {
        if (protocol == "whip") {
            throw std::invalid_argument("libav engine does not support WHIP; use engine='stream_push'");
        }
        if (is_camera_input(input)) {
            return {
                "native-camera-h264",
                input,
                "->",
                output_url,
                "protocol=" + protocol,
                "width=" + std::to_string(config_.width),
                "height=" + std::to_string(config_.height),
                "fps=" + std::to_string(config_.fps),
                "bitrate=" + std::to_string(config_.bitrate),
            };
        }
        return {
            "libav-remux",
            input,
            "->",
            output_url,
            "protocol=" + protocol,
            config_.loop ? "loop=1" : "loop=0",
            config_.realtime ? "realtime=1" : "realtime=0",
        };
    }

    if (engine == "stream_push") {
        if (protocol != "whip") {
            throw std::invalid_argument("stream_push engine only supports WHIP output");
        }
        return {
            is_camera_input(input) ? "native-camera-whip" : "embedded-whip",
            input,
            "->",
            output_url,
            config_.loop ? "loop=1" : "loop=0",
            config_.realtime ? "realtime=1" : "realtime=0",
            "width=" + std::to_string(config_.width),
            "height=" + std::to_string(config_.height),
            "fps=" + std::to_string(config_.fps),
            "bitrate=" + std::to_string(config_.bitrate),
            whip_push_available() ? "whip=enabled" : "whip=disabled",
        };
    }

    throw std::invalid_argument("unsupported engine");
}

bool NativePusher::worker_running_locked() const {
    return worker_state_ != nullptr && worker_state_->running.load() && !worker_state_->finished.load();
}

bool NativePusher::worker_finished_locked() const {
    return worker_state_ != nullptr && worker_state_->finished.load();
}

void NativePusher::join_worker_if_finished_locked() {
    if (worker_finished_locked()) {
        exit_code_ = worker_state_->exit_code;
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }
    }
}

void NativePusher::start_libav_locked(const std::string &input,
                                      const std::string &output_url,
                                      const std::string &protocol) {
    auto state = std::make_shared<WorkerState>();
    state->running.store(true);
    state->finished.store(false);
    worker_state_ = state;
    exit_code_ = -1;
    pid_ = -1;
    PusherConfig config = config_;
    if (is_camera_input(input)) {
        worker_thread_ = std::thread(run_libav_camera_push, state, config, input, output_url, protocol);
    } else {
        worker_thread_ = std::thread(run_libav_remux, state, config, input, output_url, protocol);
    }
}

void NativePusher::start_whip_locked(const std::string &input,
                                     const std::string &output_url) {
    auto state = std::make_shared<WorkerState>();
    state->running.store(true);
    state->finished.store(false);
    worker_state_ = state;
    exit_code_ = -1;
    pid_ = -1;
    PusherConfig config = config_;
    worker_thread_ = std::thread(run_whip_worker, state, config, input, output_url);
}

void NativePusher::start(const std::string &input, const std::string &output_url) {
    std::lock_guard<std::mutex> lock(mutex_);
    join_worker_if_finished_locked();
    if (worker_running_locked()) {
        throw std::runtime_error("pusher is already running");
    }
    if (input.empty()) {
        throw std::invalid_argument("input cannot be empty");
    }
    if (output_url.empty()) {
        throw std::invalid_argument("output_url cannot be empty");
    }
    if (is_local_file_input(input) && !file_exists(input)) {
        throw std::invalid_argument("input file does not exist: " + input);
    }

    const char *detected = pusher_detect_protocol(output_url.c_str());
    if (std::strcmp(detected, "unknown") == 0) {
        throw std::invalid_argument("unsupported output url protocol");
    }

    std::vector<std::string> command = build_command(input, output_url, detected);

    input_ = input;
    output_url_ = output_url;
    protocol_ = detected;
    active_engine_ = config_.engine == "auto" ? (protocol_ == "whip" ? "stream_push" : "libav") : config_.engine;
    command_ = command;
    if (active_engine_ == "libav") {
        start_libav_locked(input_, output_url_, protocol_);
    } else if (active_engine_ == "stream_push") {
        start_whip_locked(input_, output_url_);
    }
}

void NativePusher::stop(int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    join_worker_if_finished_locked();
    if (worker_running_locked()) {
        auto state = worker_state_;
        state->stop_requested.store(true);
        int wait_ms = timeout_ms >= 0 ? timeout_ms : config_.timeout_ms;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
        while (!state->finished.load() && std::chrono::steady_clock::now() < deadline) {
            state->cv.wait_for(lock, std::chrono::milliseconds(50));
        }
        if (worker_thread_.joinable()) {
            lock.unlock();
            worker_thread_.join();
            lock.lock();
        }
        exit_code_ = state->exit_code >= 0 ? state->exit_code : 255;
        return;
    }
}

bool NativePusher::wait(int timeout_ms, int &exit_code) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (true) {
        if (worker_state_ != nullptr) {
            if (worker_state_->finished.load()) {
                exit_code_ = worker_state_->exit_code;
                if (worker_thread_.joinable()) {
                    lock.unlock();
                    worker_thread_.join();
                    lock.lock();
                }
                exit_code = exit_code_;
                return true;
            }
            if (timeout_ms >= 0 && std::chrono::steady_clock::now() >= deadline) {
                exit_code = -1;
                return false;
            }
            worker_state_->cv.wait_for(lock, std::chrono::milliseconds(50));
            continue;
        }

        exit_code = exit_code_;
        return true;
    }
}

bool NativePusher::is_running() {
    std::lock_guard<std::mutex> lock(mutex_);
    join_worker_if_finished_locked();
    if (worker_running_locked()) {
        return true;
    }
    return false;
}

std::string NativePusher::status() {
    std::lock_guard<std::mutex> lock(mutex_);
    join_worker_if_finished_locked();
    bool running = worker_running_locked();

    std::ostringstream oss;
    oss << "name=" << config_.name
        << ", state=" << (running ? "running" : "stopped");

    if (pid_ > 0) {
        oss << ", pid=" << pid_;
    }
    if (worker_running_locked()) {
        oss << ", worker=thread";
    }
    if (exit_code_ >= 0) {
        oss << ", exit_code=" << exit_code_;
    }
    if (worker_state_ != nullptr && !worker_state_->error.empty()) {
        oss << ", error=" << worker_state_->error;
    }
    if (!active_engine_.empty()) {
        oss << ", engine=" << active_engine_;
    }
    if (!protocol_.empty()) {
        oss << ", protocol=" << protocol_;
    }
    if (!input_.empty()) {
        oss << ", input=" << input_;
    }
    if (!output_url_.empty()) {
        oss << ", output_url=" << output_url_;
    }
    if (!command_.empty()) {
        oss << ", command=" << shell_join(command_);
    }

    return oss.str();
}

std::string NativePusher::protocol() const {
    return protocol_;
}

std::string NativePusher::name() const {
    return config_.name;
}

std::string NativePusher::engine() const {
    return active_engine_.empty() ? config_.engine : active_engine_;
}

long NativePusher::pid() {
    std::lock_guard<std::mutex> lock(mutex_);
    join_worker_if_finished_locked();
    return pid_;
}

int NativePusher::exit_code() const {
    return exit_code_;
}

std::vector<std::string> NativePusher::command() const {
    return command_;
}

std::vector<std::string> NativePusher::preview_command(const std::string &input,
                                                       const std::string &output_url) const {
    if (input.empty()) {
        throw std::invalid_argument("input cannot be empty");
    }
    if (output_url.empty()) {
        throw std::invalid_argument("output_url cannot be empty");
    }
    const char *detected = pusher_detect_protocol(output_url.c_str());
    if (std::strcmp(detected, "unknown") == 0) {
        throw std::invalid_argument("unsupported output url protocol");
    }
    return build_command(input, output_url, detected);
}

static std::string with_secret(const std::string &url, const std::string &secret, char separator) {
    if (secret.empty()) {
        return url;
    }
    return url + separator + "secret=" + secret;
}

std::string build_output_url(const std::string &protocol,
                             const std::string &host,
                             const std::string &app,
                             const std::string &stream,
                             const std::string &secret,
                             int port,
                             bool use_tls) {
    if (host.empty()) {
        throw std::invalid_argument("host cannot be empty");
    }
    if (app.empty()) {
        throw std::invalid_argument("app cannot be empty");
    }
    if (stream.empty()) {
        throw std::invalid_argument("stream cannot be empty");
    }

    std::ostringstream oss;

    if (protocol == "rtmp") {
        int resolved_port = port > 0 ? port : (use_tls ? 443 : 1935);
        oss << (use_tls ? "rtmps" : "rtmp") << "://" << host << ":" << resolved_port
            << "/" << app << "/" << stream;
        return with_secret(oss.str(), secret, '?');
    }

    if (protocol == "rtsp") {
        int resolved_port = port > 0 ? port : (use_tls ? 322 : 8554);
        oss << (use_tls ? "rtsps" : "rtsp") << "://" << host << ":" << resolved_port
            << "/" << app << "/" << stream;
        return with_secret(oss.str(), secret, '?');
    }

    if (protocol == "srt") {
        int resolved_port = port > 0 ? port : 10080;
        oss << "srt://" << host << ":" << resolved_port
            << "?streamid=" << app << "/" << stream;
        return with_secret(oss.str(), secret, '&');
    }

    if (protocol == "rtp") {
        int resolved_port = port > 0 ? port : 5004;
        oss << "rtp://" << host << ":" << resolved_port;
        return oss.str();
    }

    if (protocol == "whip") {
        int resolved_port = port > 0 ? port : (use_tls ? 443 : 80);
        oss << (use_tls ? "https" : "http") << "://" << host << ":" << resolved_port
            << "/rtc/v1/whip/?app=" << app << "&stream=" << stream;
        return with_secret(oss.str(), secret, '&');
    }

    throw std::invalid_argument("protocol must be one of: rtmp, rtsp, srt, rtp, whip");
}

}  // namespace pusher
