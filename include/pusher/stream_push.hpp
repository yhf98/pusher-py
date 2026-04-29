#ifndef PUSHER_STREAM_PUSH_HPP
#define PUSHER_STREAM_PUSH_HPP

#include <atomic>
#include <string>

namespace pusher {

struct StreamPushConfig {
    bool loop = true;
    bool realtime = true;
    int width = 1280;
    int height = 720;
    int fps = 30;
    int bitrate = 2000000;
    int timeout_ms = 5000;
    long long analyzeduration_us = 10000000;
    long long probesize = 50000000;
    std::string log_path;
};

int run_whip_push(const StreamPushConfig &config,
                  const std::string &input,
                  const std::string &output_url,
                  std::atomic<bool> &stop_requested,
                  std::string &error);

bool whip_push_available();

}  // namespace pusher

#endif
