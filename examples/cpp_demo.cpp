#include "pusher/pusher.hpp"
#include "pusher/url_utils.h"
#include "pusher/version.h"

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::string join(const std::vector<std::string> &items) {
    std::string out;
    for (const auto &item : items) {
        if (!out.empty()) {
            out += ' ';
        }
        out += item;
    }
    return out;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        std::cerr << "usage: " << argv[0] << " INPUT OUTPUT_URL [--start]\n";
        std::cerr << "example: " << argv[0]
                  << " /dev/video0 rtmp://127.0.0.1/live/camera --start\n";
        return 2;
    }

    const bool should_start = argc >= 4 && std::strcmp(argv[3], "--start") == 0;

    try {
        pusher::PusherConfig config;
        config.name = "cpp-demo";
        config.loop = false;
        config.realtime = false;
        config.width = 1280;
        config.height = 720;
        config.fps = 30;
        config.bitrate = 2000000;

        pusher::NativePusher pusher(config);

        std::cout << "pusher version: " << PUSHER_VERSION << '\n';
        std::cout << "output protocol: " << pusher_detect_protocol(argv[2]) << '\n';
        std::cout << "preview: " << join(pusher.preview_command(argv[1], argv[2])) << '\n';

        if (!should_start) {
            std::cout << "dry run only; pass --start to start pushing\n";
            return 0;
        }

        pusher.start(argv[1], argv[2]);
        int exit_code = -1;
        pusher.wait(-1, exit_code);
        std::cout << "exit code: " << exit_code << '\n';
        std::cout << "status: " << pusher.status() << '\n';
    } catch (const std::exception &error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
