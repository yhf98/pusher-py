#include "pusher/pusher.hpp"
#include "pusher/url_utils.h"

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <sys/stat.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#ifndef S_IFDIR
#define S_IFDIR _S_IFDIR
#endif
#else
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#endif

extern "C" {
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/time.h>
}

namespace pusher {

struct LibavState {
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> running{false};
    std::atomic<bool> finished{false};
    std::mutex mutex;
    std::condition_variable cv;
    int exit_code = -1;
    std::string error;
};

namespace {

static bool is_camera_path(const std::string &input) {
    return pusher_has_prefix(input.c_str(), "/dev/video");
}

static bool is_stream_input(const std::string &input) {
    return pusher_has_prefix(input.c_str(), "rtsp://") ||
           pusher_has_prefix(input.c_str(), "rtmp://") ||
           pusher_has_prefix(input.c_str(), "http://") ||
           pusher_has_prefix(input.c_str(), "https://") ||
           pusher_has_prefix(input.c_str(), "srt://");
}

static bool is_local_file_input(const std::string &input) {
    return !input.empty() && !is_camera_path(input) && !is_stream_input(input);
}

static bool file_exists(const std::string &path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0;
}

static bool has_path_separator(const std::string &path) {
#ifdef _WIN32
    return path.find('/') != std::string::npos || path.find('\\') != std::string::npos;
#else
    return path.find('/') != std::string::npos;
#endif
}

#ifdef _WIN32
static std::string lowercase_ascii(std::string value) {
    for (char &ch : value) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return value;
}

static bool has_windows_executable_extension(const std::string &path) {
    size_t slash = path.find_last_of("/\\");
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
        return false;
    }

    std::string ext = lowercase_ascii(path.substr(dot));
    return ext == ".exe" || ext == ".cmd" || ext == ".bat" || ext == ".com";
}

static bool is_regular_file(const std::string &path) {
    struct stat st {};
    return !path.empty() && stat(path.c_str(), &st) == 0 && !(st.st_mode & S_IFDIR);
}

static bool is_executable(const std::string &path) {
    if (is_regular_file(path)) {
        return true;
    }
    if (has_windows_executable_extension(path)) {
        return false;
    }

    static const char *extensions[] = {".exe", ".cmd", ".bat", ".com"};
    for (const char *ext : extensions) {
        if (is_regular_file(path + ext)) {
            return true;
        }
    }
    return false;
}
#else
static bool is_executable(const std::string &path) {
    return !path.empty() && access(path.c_str(), X_OK) == 0;
}
#endif

static bool executable_exists(const std::string &program) {
    if (has_path_separator(program)) {
        return is_executable(program);
    }

    const char *path_env = std::getenv("PATH");
    if (path_env == nullptr || path_env[0] == '\0') {
        return false;
    }

    std::string path(path_env);
    size_t start = 0;
    while (start <= path.size()) {
#ifdef _WIN32
        size_t end = path.find(';', start);
#else
        size_t end = path.find(':', start);
#endif
        std::string dir = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (dir.empty()) {
            dir = ".";
        }
#ifdef _WIN32
        char separator = (!dir.empty() && (dir.back() == '/' || dir.back() == '\\')) ? '\0' : '\\';
        std::string candidate = separator == '\0' ? dir + program : dir + separator + program;
#else
        std::string candidate = dir + "/" + program;
#endif
        if (is_executable(candidate)) {
            return true;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }

    return false;
}

#ifndef _WIN32
static std::string errno_message(const std::string &prefix) {
    return prefix + ": " + std::strerror(errno);
}
#else
static std::string windows_error_message(const std::string &prefix) {
    DWORD error = GetLastError();
    char *message = nullptr;
    DWORD size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<char *>(&message),
        0,
        nullptr);

    std::string result = prefix + ": Windows error " + std::to_string(error);
    if (size > 0 && message != nullptr) {
        result += ": ";
        result += message;
        while (!result.empty() && (result.back() == '\r' || result.back() == '\n')) {
            result.pop_back();
        }
    }
    if (message != nullptr) {
        LocalFree(message);
    }
    return result;
}
#endif

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

#ifdef _WIN32
static std::string windows_quote_arg(const std::string &arg) {
    if (arg.empty()) {
        return "\"\"";
    }

    bool quote = false;
    for (char ch : arg) {
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\v' || ch == '"') {
            quote = true;
            break;
        }
    }
    if (!quote) {
        return arg;
    }

    std::string result = "\"";
    size_t backslashes = 0;
    for (char ch : arg) {
        if (ch == '\\') {
            ++backslashes;
            continue;
        }
        if (ch == '"') {
            result.append(backslashes * 2 + 1, '\\');
            result.push_back(ch);
            backslashes = 0;
            continue;
        }
        result.append(backslashes, '\\');
        backslashes = 0;
        result.push_back(ch);
    }
    result.append(backslashes * 2, '\\');
    result.push_back('"');
    return result;
}

static std::string windows_command_line(const std::vector<std::string> &command) {
    std::ostringstream oss;
    for (size_t i = 0; i < command.size(); ++i) {
        if (i > 0) {
            oss << ' ';
        }
        oss << windows_quote_arg(command[i]);
    }
    return oss.str();
}
#endif

#ifndef _WIN32
static int decode_wait_status(int status) {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return status;
}
#endif

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
    auto *state = static_cast<LibavState *>(opaque);
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

static void finish_libav_state(const std::shared_ptr<LibavState> &state,
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

static void run_libav_remux(std::shared_ptr<LibavState> state,
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
    finish_libav_state(state, exit_code, error);
}

}  // namespace

NativePusher::NativePusher(PusherConfig config) : config_(std::move(config)) {
    if (config_.name.empty()) {
        throw std::invalid_argument("pusher name cannot be empty");
    }
    if (config_.timeout_ms <= 0) {
        throw std::invalid_argument("timeout_ms must be greater than 0");
    }
    if (config_.engine != "auto" && config_.engine != "libav" &&
        config_.engine != "ffmpeg" && config_.engine != "stream_push") {
        throw std::invalid_argument("engine must be one of: auto, libav, ffmpeg, stream_push");
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
        if (is_camera_path(input)) {
            throw std::invalid_argument("libav remux engine does not support camera input yet; use engine='ffmpeg'");
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
        std::vector<std::string> command = {config_.stream_push_path};
        if (is_camera_path(input)) {
            command.push_back("--width");
            command.push_back(std::to_string(config_.width));
            command.push_back("--height");
            command.push_back(std::to_string(config_.height));
            command.push_back("--fps");
            command.push_back(std::to_string(config_.fps));
        }
        command.push_back(config_.loop ? "--loop" : "--no-loop");
        command.push_back("--bitrate");
        command.push_back(std::to_string(config_.bitrate));
        command.push_back(input);
        command.push_back(output_url);
        return command;
    }

    if (engine != "ffmpeg") {
        throw std::invalid_argument("unsupported engine");
    }
    if (protocol == "whip") {
        throw std::invalid_argument("ffmpeg engine does not support WHIP; use engine='stream_push'");
    }

    std::vector<std::string> command = {config_.ffmpeg_path, "-hide_banner", "-nostdin"};
    if (!config_.log_level.empty()) {
        command.push_back("-loglevel");
        command.push_back(config_.log_level);
    }
    if (config_.realtime && is_local_file_input(input)) {
        command.push_back("-re");
    }
    if (config_.loop && is_local_file_input(input)) {
        command.push_back("-stream_loop");
        command.push_back("-1");
    }
    if (is_camera_path(input)) {
        command.push_back("-f");
        command.push_back("v4l2");
        command.push_back("-framerate");
        command.push_back(std::to_string(config_.fps));
        command.push_back("-video_size");
        command.push_back(std::to_string(config_.width) + "x" + std::to_string(config_.height));
    }

    command.push_back("-i");
    command.push_back(input);

    if (config_.copy_codecs && !is_camera_path(input)) {
        command.push_back("-c");
        command.push_back("copy");
    } else {
        command.push_back("-c:v");
        command.push_back("libx264");
        command.push_back("-preset");
        command.push_back("veryfast");
        command.push_back("-tune");
        command.push_back("zerolatency");
        command.push_back("-b:v");
        command.push_back(std::to_string(config_.bitrate));
        command.push_back("-pix_fmt");
        command.push_back("yuv420p");
        command.push_back("-an");
    }

    if (protocol == "rtmp") {
        command.push_back("-f");
        command.push_back("flv");
    } else if (protocol == "rtsp") {
        command.push_back("-rtsp_transport");
        command.push_back("tcp");
        command.push_back("-f");
        command.push_back("rtsp");
    } else if (protocol == "srt") {
        command.push_back("-f");
        command.push_back("mpegts");
    } else if (protocol == "rtp") {
        command.push_back("-f");
        command.push_back("rtp");
    } else {
        throw std::invalid_argument("unsupported output protocol for ffmpeg engine");
    }

    command.push_back(output_url);
    return command;
}

void NativePusher::launch_process_locked(const std::vector<std::string> &command) {
    if (command.empty()) {
        throw std::invalid_argument("command cannot be empty");
    }
    if (!executable_exists(command[0])) {
        throw std::runtime_error("executable not found or not executable: " + command[0]);
    }

#ifdef _WIN32
    SECURITY_ATTRIBUTES security_attrs {};
    security_attrs.nLength = sizeof(security_attrs);
    security_attrs.bInheritHandle = TRUE;

    HANDLE stdin_handle = CreateFileA(
        "NUL",
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security_attrs,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    HANDLE log_handle = INVALID_HANDLE_VALUE;
    if (!config_.log_path.empty()) {
        log_handle = CreateFileA(
            config_.log_path.c_str(),
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            &security_attrs,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (log_handle == INVALID_HANDLE_VALUE) {
            if (stdin_handle != INVALID_HANDLE_VALUE) {
                CloseHandle(stdin_handle);
            }
            throw std::runtime_error(windows_error_message("open log file failed"));
        }
    }

    STARTUPINFOA startup {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = stdin_handle != INVALID_HANDLE_VALUE ? stdin_handle : GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = log_handle != INVALID_HANDLE_VALUE ? log_handle : GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = log_handle != INVALID_HANDLE_VALUE ? log_handle : GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION process {};
    std::string command_line = windows_command_line(command);
    std::vector<char> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back('\0');

    BOOL created = CreateProcessA(
        nullptr,
        mutable_command.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &startup,
        &process);

    if (stdin_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(stdin_handle);
    }
    if (log_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(log_handle);
    }

    if (!created) {
        throw std::runtime_error(windows_error_message("CreateProcess failed"));
    }

    CloseHandle(process.hThread);
    process_handle_ = reinterpret_cast<std::uintptr_t>(process.hProcess);
    pid_ = static_cast<long>(process.dwProcessId);
    exit_code_ = -1;
#else
    pid_t child = fork();
    if (child < 0) {
        throw std::runtime_error(errno_message("fork failed"));
    }

    if (child == 0) {
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }

        if (!config_.log_path.empty()) {
            int log_fd = open(config_.log_path.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
            if (log_fd >= 0) {
                dup2(log_fd, STDOUT_FILENO);
                dup2(log_fd, STDERR_FILENO);
                close(log_fd);
            }
        }

        std::vector<char *> argv;
        argv.reserve(command.size() + 1);
        for (const auto &arg : command) {
            argv.push_back(const_cast<char *>(arg.c_str()));
        }
        argv.push_back(nullptr);

        execvp(argv[0], argv.data());
        _exit(errno == ENOENT ? 127 : 126);
    }

    pid_ = static_cast<long>(child);
    exit_code_ = -1;
#endif
}

bool NativePusher::reap_locked() {
#ifdef _WIN32
    if (process_handle_ == 0 || pid_ <= 0) {
        return false;
    }

    HANDLE handle = reinterpret_cast<HANDLE>(process_handle_);
    DWORD result = WaitForSingleObject(handle, 0);
    if (result == WAIT_TIMEOUT) {
        return true;
    }
    if (result != WAIT_OBJECT_0) {
        throw std::runtime_error(windows_error_message("WaitForSingleObject failed"));
    }

    DWORD exit_code = 0;
    if (!GetExitCodeProcess(handle, &exit_code)) {
        throw std::runtime_error(windows_error_message("GetExitCodeProcess failed"));
    }
    CloseHandle(handle);
    process_handle_ = 0;
    pid_ = -1;
    exit_code_ = static_cast<int>(exit_code);
    return false;
#else
    if (pid_ <= 0) {
        return false;
    }

    int status = 0;
    pid_t result = waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
    if (result == 0) {
        return true;
    }
    if (result < 0) {
        if (errno == ECHILD) {
            pid_ = -1;
            exit_code_ = -1;
            return false;
        }
        throw std::runtime_error(errno_message("waitpid failed"));
    }

    exit_code_ = decode_wait_status(status);
    pid_ = -1;
    return false;
#endif
}

bool NativePusher::libav_running_locked() const {
    return libav_state_ != nullptr && libav_state_->running.load() && !libav_state_->finished.load();
}

bool NativePusher::libav_finished_locked() const {
    return libav_state_ != nullptr && libav_state_->finished.load();
}

void NativePusher::join_libav_if_finished_locked() {
    if (libav_finished_locked()) {
        exit_code_ = libav_state_->exit_code;
        if (libav_thread_.joinable()) {
            libav_thread_.join();
        }
    }
}

void NativePusher::start_libav_locked(const std::string &input,
                                      const std::string &output_url,
                                      const std::string &protocol) {
    auto state = std::make_shared<LibavState>();
    state->running.store(true);
    state->finished.store(false);
    libav_state_ = state;
    exit_code_ = -1;
    pid_ = -1;
#ifdef _WIN32
    process_handle_ = 0;
#endif
    PusherConfig config = config_;
    libav_thread_ = std::thread(run_libav_remux, state, config, input, output_url, protocol);
}

void NativePusher::start(const std::string &input, const std::string &output_url) {
    std::lock_guard<std::mutex> lock(mutex_);
    join_libav_if_finished_locked();
    if (libav_running_locked()) {
        throw std::runtime_error("pusher is already running");
    }
    if (reap_locked()) {
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
    } else {
        launch_process_locked(command_);
    }
}

void NativePusher::stop(int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (libav_running_locked()) {
        auto state = libav_state_;
        state->stop_requested.store(true);
        int wait_ms = timeout_ms >= 0 ? timeout_ms : config_.timeout_ms;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
        while (!state->finished.load() && std::chrono::steady_clock::now() < deadline) {
            state->cv.wait_for(lock, std::chrono::milliseconds(50));
        }
        if (libav_thread_.joinable()) {
            lock.unlock();
            libav_thread_.join();
            lock.lock();
        }
        exit_code_ = state->exit_code >= 0 ? state->exit_code : 255;
        return;
    }

    if (!reap_locked()) {
        return;
    }

#ifdef _WIN32
    HANDLE target_handle = reinterpret_cast<HANDLE>(process_handle_);
    if (target_handle == nullptr) {
        pid_ = -1;
        return;
    }

    int wait_ms = timeout_ms >= 0 ? timeout_ms : config_.timeout_ms;
    if (!TerminateProcess(target_handle, 1)) {
        DWORD error = GetLastError();
        if (error != ERROR_ACCESS_DENIED) {
            throw std::runtime_error(windows_error_message("TerminateProcess failed"));
        }
    }

    DWORD wait_result = WaitForSingleObject(target_handle, static_cast<DWORD>(wait_ms));
    if (wait_result != WAIT_OBJECT_0 && wait_result != WAIT_TIMEOUT) {
        throw std::runtime_error(windows_error_message("WaitForSingleObject failed"));
    }

    DWORD process_exit_code = 1;
    GetExitCodeProcess(target_handle, &process_exit_code);
    if (process_exit_code == STILL_ACTIVE && wait_result == WAIT_TIMEOUT) {
        process_exit_code = 1;
    }
    CloseHandle(target_handle);
    process_handle_ = 0;
    pid_ = -1;
    exit_code_ = static_cast<int>(process_exit_code);
#else
    long target_pid = pid_;
    kill(static_cast<pid_t>(target_pid), SIGTERM);

    int wait_ms = timeout_ms >= 0 ? timeout_ms : config_.timeout_ms;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!reap_locked()) {
            return;
        }
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        lock.lock();
    }

    if (pid_ > 0) {
        kill(static_cast<pid_t>(pid_), SIGKILL);
        int status = 0;
        pid_t result = waitpid(static_cast<pid_t>(pid_), &status, 0);
        if (result > 0) {
            exit_code_ = decode_wait_status(status);
        }
        pid_ = -1;
    }
#endif
}

bool NativePusher::wait(int timeout_ms, int &exit_code) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (true) {
        if (libav_state_ != nullptr) {
            if (libav_state_->finished.load()) {
                exit_code_ = libav_state_->exit_code;
                if (libav_thread_.joinable()) {
                    lock.unlock();
                    libav_thread_.join();
                    lock.lock();
                }
                exit_code = exit_code_;
                return true;
            }
            if (timeout_ms >= 0 && std::chrono::steady_clock::now() >= deadline) {
                exit_code = -1;
                return false;
            }
            libav_state_->cv.wait_for(lock, std::chrono::milliseconds(50));
            continue;
        }

        bool running = reap_locked();
        if (!running) {
            exit_code = exit_code_;
            return true;
        }
        if (timeout_ms >= 0 && std::chrono::steady_clock::now() >= deadline) {
            exit_code = -1;
            return false;
        }
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        lock.lock();
    }
}

bool NativePusher::is_running() {
    std::lock_guard<std::mutex> lock(mutex_);
    join_libav_if_finished_locked();
    if (libav_running_locked()) {
        return true;
    }
    return reap_locked();
}

std::string NativePusher::status() {
    std::lock_guard<std::mutex> lock(mutex_);
    join_libav_if_finished_locked();
    bool running = libav_running_locked() || reap_locked();

    std::ostringstream oss;
    oss << "name=" << config_.name
        << ", state=" << (running ? "running" : "stopped");

    if (pid_ > 0) {
        oss << ", pid=" << pid_;
    }
    if (libav_running_locked()) {
        oss << ", worker=thread";
    }
    if (exit_code_ >= 0) {
        oss << ", exit_code=" << exit_code_;
    }
    if (libav_state_ != nullptr && !libav_state_->error.empty()) {
        oss << ", error=" << libav_state_->error;
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
    join_libav_if_finished_locked();
    reap_locked();
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
