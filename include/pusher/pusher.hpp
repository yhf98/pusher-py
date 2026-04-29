#ifndef PUSHER_PUSHER_HPP
#define PUSHER_PUSHER_HPP

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pusher {

struct WorkerState;

struct PusherConfig {
    std::string name = "default";
    int timeout_ms = 5000;
    bool auto_reconnect = true;
    std::string engine = "auto";
    std::string log_path;
    bool loop = true;
    bool realtime = true;
    int width = 1280;
    int height = 720;
    int fps = 30;
    int bitrate = 2000000;
    long long analyzeduration_us = 10000000;
    long long probesize = 50000000;
};

class NativePusher {
public:
    explicit NativePusher(PusherConfig config);
    ~NativePusher();

    void start(const std::string &input, const std::string &output_url);
    void stop(int timeout_ms = -1);
    bool wait(int timeout_ms, int &exit_code);

    bool is_running();
    std::string status();
    std::string protocol() const;
    std::string name() const;
    std::string engine() const;
    long pid();
    int exit_code() const;
    std::vector<std::string> command() const;
    std::vector<std::string> preview_command(const std::string &input,
                                             const std::string &output_url) const;

private:
    std::vector<std::string> build_command(const std::string &input,
                                           const std::string &output_url,
                                           const std::string &protocol) const;
    void start_libav_locked(const std::string &input,
                            const std::string &output_url,
                            const std::string &protocol);
    void start_whip_locked(const std::string &input,
                           const std::string &output_url);
    bool worker_running_locked() const;
    bool worker_finished_locked() const;
    void join_worker_if_finished_locked();

    PusherConfig config_;
    mutable std::mutex mutex_;
    std::string input_;
    std::string output_url_;
    std::string protocol_;
    std::string active_engine_;
    std::vector<std::string> command_;
    long pid_ = -1;
    int exit_code_ = -1;
    std::shared_ptr<WorkerState> worker_state_;
    std::thread worker_thread_;
};

std::string build_output_url(const std::string &protocol,
                             const std::string &host,
                             const std::string &app,
                             const std::string &stream,
                             const std::string &secret,
                             int port,
                             bool use_tls);

}  // namespace pusher

#endif
