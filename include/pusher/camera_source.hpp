#ifndef PUSHER_CAMERA_SOURCE_HPP
#define PUSHER_CAMERA_SOURCE_HPP

#include <atomic>
#include <cstdint>
#include <string>

extern "C" {
#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>
#include <libavutil/rational.h>
}

namespace pusher {

struct CameraSourceConfig {
    int width = 1280;
    int height = 720;
    int fps = 30;
    int bitrate = 2000000;
    int timeout_ms = 5000;
    long long analyzeduration_us = 10000000;
    long long probesize = 50000000;
};

bool is_camera_input(const std::string &input);

class CameraH264Source {
public:
    explicit CameraH264Source(CameraSourceConfig config);
    ~CameraH264Source();

    CameraH264Source(const CameraH264Source &) = delete;
    CameraH264Source &operator=(const CameraH264Source &) = delete;

    int open(const std::string &input,
             std::atomic<bool> &stop_requested,
             std::string &error);
    int read_packet(AVPacket *out, std::string &error);

    const AVCodecParameters *codec_parameters() const;
    AVRational time_base() const;
    bool copy_mode() const;

private:
    struct Impl;
    Impl *impl_;
};

}  // namespace pusher

#endif
