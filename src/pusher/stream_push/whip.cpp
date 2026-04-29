#include "pusher/stream_push.hpp"
#include "pusher/camera_source.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdint>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <string.h>
#else
#include <strings.h>
#endif

extern "C" {
#include <libavcodec/bsf.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/time.h>
}

#ifdef PUSHER_ENABLE_WHIP
#include <curl/curl.h>
#include <rtc/rtc.hpp>
#endif

namespace pusher {
namespace {

static bool starts_with(const std::string &value, const std::string &prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

static std::string av_error(int ret) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(ret, buf, sizeof(buf));
    return std::string(buf);
}

static bool is_stream_input(const std::string &input) {
    return starts_with(input, "rtsp://") ||
           starts_with(input, "rtmp://") ||
           starts_with(input, "http://") ||
           starts_with(input, "https://") ||
           starts_with(input, "srt://");
}

static std::vector<uint8_t> extract_sps_pps_from_extradata(const uint8_t *extra, int extra_size) {
    std::vector<uint8_t> result;
    if (extra == nullptr || extra_size < 4) {
        return result;
    }

    if (extra[0] == 1 && extra_size >= 8) {
        int sps_count = extra[5] & 0x1f;
        int offset = 6;
        for (int i = 0; i < sps_count && offset + 2 <= extra_size; ++i) {
            int len = (extra[offset] << 8) | extra[offset + 1];
            offset += 2;
            if (len <= 0 || offset + len > extra_size) {
                break;
            }
            result.insert(result.end(), {0, 0, 0, 1});
            result.insert(result.end(), extra + offset, extra + offset + len);
            offset += len;
        }
        if (offset < extra_size) {
            int pps_count = extra[offset++];
            for (int i = 0; i < pps_count && offset + 2 <= extra_size; ++i) {
                int len = (extra[offset] << 8) | extra[offset + 1];
                offset += 2;
                if (len <= 0 || offset + len > extra_size) {
                    break;
                }
                result.insert(result.end(), {0, 0, 0, 1});
                result.insert(result.end(), extra + offset, extra + offset + len);
                offset += len;
            }
        }
    } else if (extra[0] == 0 && extra[1] == 0) {
        result.assign(extra, extra + extra_size);
    }

    return result;
}

#ifdef PUSHER_ENABLE_WHIP

struct HttpResponse {
    long http_code = 0;
    std::string body;
    std::string location;
};

struct UrlParts {
    std::string scheme;
    std::string host;
    std::string host_only;
    std::string app = "live";
    std::string stream = "test";
    std::string secret;
};

static size_t curl_write_cb(void *data, size_t size, size_t nmemb, void *userp) {
    auto *out = static_cast<std::string *>(userp);
    out->append(static_cast<char *>(data), size * nmemb);
    return size * nmemb;
}

static std::string trim_copy(std::string value) {
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n' ||
                              value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    size_t start = 0;
    while (start < value.size() && (value[start] == ' ' || value[start] == '\t')) {
        ++start;
    }
    return value.substr(start);
}

static bool http_post(const std::string &url,
                      const std::string &body,
                      const std::string &content_type,
                      HttpResponse &resp,
                      int timeout_sec) {
    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
        return false;
    }

    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, ("Content-Type: " + content_type).c_str());
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeout_sec));
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION,
        +[](char *buffer, size_t size, size_t nitems, void *userdata) -> size_t {
            const size_t total = size * nitems;
            std::string line(buffer, total);
            auto *response = static_cast<HttpResponse *>(userdata);
            const std::string prefix = "Location:";
            if (line.size() >= prefix.size() &&
                strncasecmp(line.c_str(), prefix.c_str(), prefix.size()) == 0) {
                response->location = trim_copy(line.substr(prefix.size()));
            }
            return total;
        });
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &resp);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return res == CURLE_OK && resp.http_code >= 200 && resp.http_code < 300;
}

static bool http_delete(const std::string &url, int timeout_sec) {
    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
        return false;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeout_sec));
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return res == CURLE_OK;
}

static std::string json_escape(const std::string &value) {
    std::string out;
    out.reserve(value.size() + 16);
    for (char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

static bool json_get_string(const std::string &json, const std::string &key, std::string &out) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) {
        return false;
    }
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) {
        return false;
    }
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) {
        return false;
    }
    ++pos;

    std::string value;
    bool escape = false;
    for (; pos < json.size(); ++pos) {
        char c = json[pos];
        if (escape) {
            switch (c) {
                case 'n': value += '\n'; break;
                case 'r': value += '\r'; break;
                case 't': value += '\t'; break;
                default: value += c; break;
            }
            escape = false;
        } else if (c == '\\') {
            escape = true;
        } else if (c == '"') {
            out = value;
            return true;
        } else {
            value += c;
        }
    }
    return false;
}

static UrlParts parse_url(const std::string &url) {
    UrlParts parts;
    size_t scheme_pos = url.find("://");
    parts.scheme = scheme_pos == std::string::npos ? "http" : url.substr(0, scheme_pos);
    size_t host_start = scheme_pos == std::string::npos ? 0 : scheme_pos + 3;
    size_t path_start = url.find('/', host_start);
    parts.host = url.substr(host_start, path_start == std::string::npos ? std::string::npos : path_start - host_start);
    parts.host_only = parts.host;
    size_t colon = parts.host_only.find(':');
    if (colon != std::string::npos) {
        parts.host_only = parts.host_only.substr(0, colon);
    }

    size_t query = url.find('?', path_start == std::string::npos ? url.size() : path_start);
    if (query != std::string::npos) {
        size_t pos = query + 1;
        while (pos < url.size()) {
            size_t end = url.find('&', pos);
            std::string kv = url.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            size_t eq = kv.find('=');
            std::string key = eq == std::string::npos ? kv : kv.substr(0, eq);
            std::string value = eq == std::string::npos ? "" : kv.substr(eq + 1);
            if (key == "app") {
                parts.app = value;
            } else if (key == "stream") {
                parts.stream = value;
            } else if (key == "secret") {
                parts.secret = value;
            }
            if (end == std::string::npos) {
                break;
            }
            pos = end + 1;
        }
    }
    return parts;
}

static std::string prepare_offer(std::string sdp) {
    if (sdp.find("a=rtcp-mux") == std::string::npos) {
        size_t pos = sdp.find("m=video");
        if (pos != std::string::npos) {
            size_t next = sdp.find("\r\nm=", pos + 1);
            size_t insert = next == std::string::npos ? sdp.size() : next;
            sdp.insert(insert, "a=rtcp-mux\r\n");
        }
    }
    return sdp;
}

static std::string prepare_answer(std::string sdp) {
    if (sdp.find("\r\n") == std::string::npos) {
        size_t pos = 0;
        while ((pos = sdp.find('\n', pos)) != std::string::npos) {
            sdp.replace(pos, 1, "\r\n");
            pos += 2;
        }
    }
    return sdp;
}

static bool signaling_whip(const std::string &url,
                           const std::string &sdp_offer,
                           std::string &sdp_answer,
                           std::string &resource_url,
                           int timeout_sec) {
    HttpResponse resp;
    if (!http_post(url, sdp_offer, "application/sdp", resp, timeout_sec)) {
        return false;
    }
    if (resp.body.find("m=") == std::string::npos) {
        return false;
    }
    if (!resp.location.empty()) {
        if (!starts_with(resp.location, "http://") && !starts_with(resp.location, "https://")) {
            size_t scheme = url.find("://");
            size_t path = url.find('/', scheme == std::string::npos ? 0 : scheme + 3);
            resource_url = path == std::string::npos ? url + resp.location : url.substr(0, path) + resp.location;
        } else {
            resource_url = resp.location;
        }
    }
    sdp_answer = resp.body;
    return true;
}

static bool signaling_srs(const UrlParts &parts,
                          const std::string &sdp_offer,
                          std::string &sdp_answer,
                          int timeout_sec) {
    std::string api_url = parts.scheme + "://" + parts.host + "/rtc/v1/publish/";
    std::string stream_url = "webrtc://" + parts.host_only + "/" + parts.app + "/" + parts.stream;
    std::ostringstream body;
    body << "{"
         << "\"api\":\"" << json_escape(api_url) << "\","
         << "\"streamurl\":\"" << json_escape(stream_url) << "\","
         << "\"sdp\":\"" << json_escape(sdp_offer) << "\"";
    if (!parts.secret.empty()) {
        body << ",\"secret\":\"" << json_escape(parts.secret) << "\"";
    }
    body << "}";

    HttpResponse resp;
    if (!http_post(api_url, body.str(), "application/json", resp, timeout_sec)) {
        return false;
    }
    return json_get_string(resp.body, "sdp", sdp_answer) &&
           sdp_answer.find("m=") != std::string::npos;
}

struct NalInfo {
    int offset = 0;
    int start_code_size = 0;
    int type = 0;
};

static std::vector<NalInfo> find_nals(const uint8_t *data, int size) {
    std::vector<NalInfo> nals;
    if (data == nullptr || size <= 4) {
        return nals;
    }

    for (int pos = 0; pos + 3 < size; ++pos) {
        if (data[pos] != 0 || data[pos + 1] != 0) {
            continue;
        }
        if (data[pos + 2] == 1) {
            nals.push_back({pos, 3, data[pos + 3] & 0x1f});
            pos += 2;
        } else if (pos + 4 < size && data[pos + 2] == 0 && data[pos + 3] == 1) {
            nals.push_back({pos, 4, data[pos + 4] & 0x1f});
            pos += 3;
        }
    }
    return nals;
}

static rtc::binary build_frame_data(const uint8_t *data,
                                    int size,
                                    bool key_frame,
                                    const std::vector<uint8_t> &sps_pps) {
    rtc::binary frame;
    if (key_frame && !sps_pps.empty()) {
        frame.insert(frame.end(),
                     reinterpret_cast<const std::byte *>(sps_pps.data()),
                     reinterpret_cast<const std::byte *>(sps_pps.data() + sps_pps.size()));
    }

    std::vector<NalInfo> nals = find_nals(data, size);
    if (nals.empty()) {
        frame.insert(frame.end(),
                     reinterpret_cast<const std::byte *>(data),
                     reinterpret_cast<const std::byte *>(data + size));
        return frame;
    }

    for (size_t i = 0; i < nals.size(); ++i) {
        if (nals[i].type < 1 || nals[i].type > 5) {
            continue;
        }
        int start = nals[i].offset;
        int end = i + 1 < nals.size() ? nals[i + 1].offset : size;
        if (end <= start) {
            continue;
        }
        frame.insert(frame.end(),
                     reinterpret_cast<const std::byte *>(data + start),
                     reinterpret_cast<const std::byte *>(data + end));
    }
    return frame;
}

#endif  // PUSHER_ENABLE_WHIP

}  // namespace

bool whip_push_available() {
#ifdef PUSHER_ENABLE_WHIP
    return true;
#else
    return false;
#endif
}

int run_whip_push(const StreamPushConfig &config,
                  const std::string &input,
                  const std::string &output_url,
                  std::atomic<bool> &stop_requested,
                  std::string &error) {
#ifndef PUSHER_ENABLE_WHIP
    (void)config;
    (void)input;
    (void)output_url;
    (void)stop_requested;
    error = "embedded WHIP support is not enabled in this build";
    return 1;
#else
    if (!starts_with(output_url, "http://") && !starts_with(output_url, "https://")) {
        error = "WHIP output URL must start with http:// or https://";
        return 1;
    }

    avformat_network_init();
    curl_global_init(CURL_GLOBAL_DEFAULT);
    rtc::InitLogger(rtc::LogLevel::Warning);

    AVFormatContext *input_ctx = nullptr;
    AVDictionary *input_opts = nullptr;
    AVBSFContext *bsf_ctx = nullptr;
    AVPacket *pkt = nullptr;
    std::string resource_url;
    int ret = 0;
    int exit_code = 0;

    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> video_track;
    std::shared_ptr<rtc::Track> audio_track;
    std::atomic<bool> connected{false};
    std::atomic<bool> pli_requested{false};

    const std::byte opus_silence[] = {std::byte{0xF8}, std::byte{0xFF}, std::byte{0xFE}};
    double last_audio = 0.0;
    double last_dts = 0.0;
    int64_t wall_start_us = 0;
    int64_t media_start_us = AV_NOPTS_VALUE;
    int video_index = -1;
    bool camera_input = is_camera_input(input);
    bool local_file = !camera_input && !is_stream_input(input);
    std::unique_ptr<CameraH264Source> camera_source;
    AVRational camera_time_base{1, config.fps};
    std::vector<uint8_t> sps_pps;

    if (camera_input) {
        CameraSourceConfig camera_config;
        camera_config.width = config.width;
        camera_config.height = config.height;
        camera_config.fps = config.fps;
        camera_config.bitrate = config.bitrate;
        camera_config.timeout_ms = config.timeout_ms;
        camera_config.analyzeduration_us = config.analyzeduration_us;
        camera_config.probesize = config.probesize;
        camera_source = std::make_unique<CameraH264Source>(camera_config);
        ret = camera_source->open(input, stop_requested, error);
        if (ret != 0) {
            exit_code = 1;
            goto cleanup;
        }
        camera_time_base = camera_source->time_base();
        const AVCodecParameters *camera_params = camera_source->codec_parameters();
        if (camera_params != nullptr && camera_params->extradata != nullptr && camera_params->extradata_size > 0) {
            sps_pps = extract_sps_pps_from_extradata(camera_params->extradata, camera_params->extradata_size);
        }
    } else if (is_stream_input(input)) {
        av_dict_set(&input_opts, "rtsp_transport", "tcp", 0);
        av_dict_set(&input_opts, "analyzeduration", std::to_string(config.analyzeduration_us).c_str(), 0);
        av_dict_set(&input_opts, "probesize", std::to_string(config.probesize).c_str(), 0);
        av_dict_set(&input_opts, "stimeout", std::to_string(config.timeout_ms * 1000).c_str(), 0);
        av_dict_set(&input_opts, "rw_timeout", std::to_string(config.timeout_ms * 1000).c_str(), 0);
    }

    if (!camera_input) {
        ret = avformat_open_input(&input_ctx, input.c_str(), nullptr, &input_opts);
        av_dict_free(&input_opts);
        if (ret < 0) {
            error = "open input failed: " + av_error(ret);
            exit_code = 1;
            goto cleanup;
        }
        input_ctx->flags |= AVFMT_FLAG_GENPTS;

        ret = avformat_find_stream_info(input_ctx, nullptr);
        if (ret < 0) {
            error = "find input stream info failed: " + av_error(ret);
            exit_code = 1;
            goto cleanup;
        }

        for (unsigned int i = 0; i < input_ctx->nb_streams; ++i) {
            AVStream *stream = input_ctx->streams[i];
            if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                video_index = static_cast<int>(i);
                break;
            }
        }
        if (video_index < 0) {
            error = "input has no video stream";
            exit_code = 1;
            goto cleanup;
        }

        AVStream *video_stream = input_ctx->streams[video_index];
        AVCodecParameters *codecpar = video_stream->codecpar;
        if (codecpar->codec_id != AV_CODEC_ID_H264) {
            error = "embedded WHIP currently supports H264 input only";
            exit_code = 1;
            goto cleanup;
        }
        if (codecpar->extradata != nullptr && codecpar->extradata_size > 0) {
            sps_pps = extract_sps_pps_from_extradata(codecpar->extradata, codecpar->extradata_size);
        }

        bool need_bsf = codecpar->extradata != nullptr &&
                        codecpar->extradata_size > 0 &&
                        codecpar->extradata[0] == 1;
        if (need_bsf) {
            const AVBitStreamFilter *bsf = av_bsf_get_by_name("h264_mp4toannexb");
            if (bsf == nullptr) {
                error = "h264_mp4toannexb bitstream filter is unavailable";
                exit_code = 1;
                goto cleanup;
            }
            ret = av_bsf_alloc(bsf, &bsf_ctx);
            if (ret < 0) {
                error = "allocate h264_mp4toannexb failed: " + av_error(ret);
                exit_code = 1;
                goto cleanup;
            }
            ret = avcodec_parameters_copy(bsf_ctx->par_in, codecpar);
            if (ret < 0) {
                error = "copy bsf codec parameters failed: " + av_error(ret);
                exit_code = 1;
                goto cleanup;
            }
            bsf_ctx->time_base_in = video_stream->time_base;
            ret = av_bsf_init(bsf_ctx);
            if (ret < 0) {
                error = "initialize h264_mp4toannexb failed: " + av_error(ret);
                exit_code = 1;
                goto cleanup;
            }
        }
    }

    try {
        UrlParts url_parts = parse_url(output_url);
        rtc::SetThreadPoolSize(2);

        rtc::Configuration rtc_config;
        rtc_config.disableAutoNegotiation = true;
        pc = std::make_shared<rtc::PeerConnection>(rtc_config);

        std::mutex mtx;
        std::condition_variable cv;
        bool gathering_done = false;

        pc->onStateChange([&](rtc::PeerConnection::State state) {
            if (state == rtc::PeerConnection::State::Connected) {
                connected = true;
            } else if (state >= rtc::PeerConnection::State::Disconnected) {
                connected = false;
                if (state >= rtc::PeerConnection::State::Failed) {
                    stop_requested.store(true);
                }
            }
        });
        pc->onGatheringStateChange([&](rtc::PeerConnection::GatheringState state) {
            if (state == rtc::PeerConnection::GatheringState::Complete) {
                std::lock_guard<std::mutex> lk(mtx);
                gathering_done = true;
                cv.notify_one();
            }
        });

        const std::string cname = "pusher-py";
        rtc::Description::Audio audio_media("audio", rtc::Description::Direction::SendOnly);
        audio_media.addOpusCodec(111);
        audio_media.addSSRC(2, cname, "stream", "audio");
        audio_track = pc->addTrack(audio_media);
        auto audio_rtp = std::make_shared<rtc::RtpPacketizationConfig>(2, cname, 111, 48000);
        auto audio_packetizer = std::make_shared<rtc::OpusRtpPacketizer>(audio_rtp);
        auto audio_sr = std::make_shared<rtc::RtcpSrReporter>(audio_rtp);
        audio_packetizer->addToChain(audio_sr);
        audio_track->setMediaHandler(audio_packetizer);

        rtc::Description::Video video_media("video", rtc::Description::Direction::SendOnly);
        video_media.addH264Codec(
            96,
            "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42c01f");
        video_media.addSSRC(1, cname, "stream", "video");
        video_track = pc->addTrack(video_media);
        auto video_rtp = std::make_shared<rtc::RtpPacketizationConfig>(1, cname, 96, 90000);
        auto video_packetizer = std::make_shared<rtc::H264RtpPacketizer>(
            rtc::NalUnit::Separator::StartSequence, video_rtp);
        auto video_sr = std::make_shared<rtc::RtcpSrReporter>(video_rtp);
        video_packetizer->addToChain(video_sr);
        auto nack = std::make_shared<rtc::RtcpNackResponder>();
        video_sr->addToChain(nack);
        auto pli = std::make_shared<rtc::PliHandler>([&pli_requested]() {
            pli_requested = true;
        });
        nack->addToChain(pli);
        video_track->setMediaHandler(video_packetizer);

        pc->setLocalDescription(rtc::Description::Type::Offer);
        {
            std::unique_lock<std::mutex> lk(mtx);
            if (!cv.wait_for(lk, std::chrono::seconds(10), [&] { return gathering_done; })) {
                error = "WHIP ICE gathering timed out";
                exit_code = 1;
                goto cleanup;
            }
        }

        auto local_desc = pc->localDescription();
        if (!local_desc) {
            error = "failed to get WHIP local SDP";
            exit_code = 1;
            goto cleanup;
        }

        std::string offer = prepare_offer(local_desc->generateSdp());
        std::string answer_raw;
        int timeout_sec = std::max(1, config.timeout_ms / 1000);
        bool signaled = signaling_whip(output_url, offer, answer_raw, resource_url, timeout_sec);
        if (!signaled) {
            signaled = signaling_srs(url_parts, offer, answer_raw, timeout_sec);
        }
        if (!signaled) {
            error = "WHIP signaling failed";
            exit_code = 1;
            goto cleanup;
        }

        pc->setRemoteDescription(rtc::Description(prepare_answer(answer_raw), rtc::Description::Type::Answer));
        for (int i = 0; i < 100 && !stop_requested.load() && !connected.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!connected.load()) {
            error = "WHIP peer connection timed out";
            exit_code = 1;
            goto cleanup;
        }
    } catch (const std::exception &exc) {
        error = std::string("WHIP setup failed: ") + exc.what();
        exit_code = 1;
        goto cleanup;
    }

    pkt = av_packet_alloc();
    if (pkt == nullptr) {
        error = "allocate packet failed";
        exit_code = 1;
        goto cleanup;
    }

    while (!stop_requested.load() && connected.load()) {
        AVStream *video_stream = camera_input ? nullptr : input_ctx->streams[video_index];
        if (camera_input) {
            ret = camera_source->read_packet(pkt, error);
        } else {
            ret = av_read_frame(input_ctx, pkt);
        }
        if (ret == AVERROR_EOF && config.loop && local_file) {
            media_start_us = AV_NOPTS_VALUE;
            wall_start_us = 0;
            last_dts = 0.0;
            avformat_seek_file(input_ctx, -1, INT64_MIN, 0, INT64_MAX, 0);
            avformat_flush(input_ctx);
            if (bsf_ctx != nullptr) {
                av_bsf_flush(bsf_ctx);
            }
            continue;
        }
        if (ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            if (error.empty()) {
                error = "read input packet failed: " + av_error(ret);
            }
            exit_code = 1;
            break;
        }
        if (!camera_input && pkt->stream_index != video_index) {
            av_packet_unref(pkt);
            continue;
        }

        auto send_packet = [&](AVPacket *packet, AVRational time_base) {
            int64_t ts = packet->dts != AV_NOPTS_VALUE ? packet->dts : packet->pts;
            double dts_sec = ts == AV_NOPTS_VALUE ? last_dts + (1.0 / std::max(1, config.fps))
                                                  : ts * av_q2d(time_base);
            last_dts = dts_sec;

            if (config.realtime && local_file && ts != AV_NOPTS_VALUE) {
                int64_t media_ts_us = av_rescale_q(ts, time_base, AV_TIME_BASE_Q);
                if (media_start_us == AV_NOPTS_VALUE) {
                    media_start_us = media_ts_us;
                    wall_start_us = av_gettime_relative();
                } else {
                    int64_t target_us = wall_start_us + (media_ts_us - media_start_us);
                    while (!stop_requested.load()) {
                        int64_t delay_us = target_us - av_gettime_relative();
                        if (delay_us <= 0) {
                            break;
                        }
                        av_usleep(delay_us > 100000 ? 100000 : delay_us);
                    }
                }
            }

            bool key = (packet->flags & AV_PKT_FLAG_KEY) != 0;
            if (pli_requested.exchange(false) && !key) {
                return;
            }

            try {
                rtc::FrameInfo info{std::chrono::duration<double>{dts_sec}};
                info.isKeyFrame = key;
                video_track->sendFrame(
                    build_frame_data(packet->data, packet->size, key, sps_pps),
                    info);
                while (last_audio < dts_sec && connected.load() && !stop_requested.load()) {
                    rtc::binary silence(opus_silence, opus_silence + sizeof(opus_silence));
                    audio_track->sendFrame(
                        std::move(silence),
                        rtc::FrameInfo{std::chrono::duration<double>{last_audio}});
                    last_audio += 0.020;
                }
            } catch (const std::exception &exc) {
                error = std::string("send WHIP frame failed: ") + exc.what();
                exit_code = 1;
                stop_requested.store(true);
            }
        };

        if (bsf_ctx != nullptr) {
            ret = av_bsf_send_packet(bsf_ctx, pkt);
            av_packet_unref(pkt);
            if (ret < 0) {
                continue;
            }
            while (!stop_requested.load() && av_bsf_receive_packet(bsf_ctx, pkt) == 0) {
                send_packet(pkt, bsf_ctx->time_base_out);
                av_packet_unref(pkt);
            }
        } else {
            send_packet(pkt, camera_input ? camera_time_base : video_stream->time_base);
            av_packet_unref(pkt);
        }
    }

cleanup:
    if (!resource_url.empty()) {
        http_delete(resource_url, std::max(1, config.timeout_ms / 1000));
    }
    if (video_track) {
        video_track->close();
    }
    if (audio_track) {
        audio_track->close();
    }
    if (pc) {
        pc->close();
    }
    if (pkt != nullptr) {
        av_packet_free(&pkt);
    }
    if (bsf_ctx != nullptr) {
        av_bsf_free(&bsf_ctx);
    }
    if (input_ctx != nullptr) {
        avformat_close_input(&input_ctx);
    }
    av_dict_free(&input_opts);
    curl_global_cleanup();
    return stop_requested.load() && exit_code == 0 ? 0 : exit_code;
#endif
}

}  // namespace pusher
