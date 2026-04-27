#include "pusher/url_utils.h"

#include <stddef.h>
#include <string.h>

int pusher_has_prefix(const char *text, const char *prefix) {
    size_t prefix_len;

    if (text == NULL || prefix == NULL) {
        return 0;
    }

    prefix_len = strlen(prefix);
    return strncmp(text, prefix, prefix_len) == 0;
}

const char *pusher_detect_protocol(const char *url) {
    if (url == NULL || url[0] == '\0') {
        return "unknown";
    }

    if (pusher_has_prefix(url, "rtmp://") || pusher_has_prefix(url, "rtmps://")) {
        return "rtmp";
    }
    if (pusher_has_prefix(url, "rtsp://") || pusher_has_prefix(url, "rtsps://")) {
        return "rtsp";
    }
    if (pusher_has_prefix(url, "srt://")) {
        return "srt";
    }
    if (pusher_has_prefix(url, "rtp://")) {
        return "rtp";
    }
    if (pusher_has_prefix(url, "http://") || pusher_has_prefix(url, "https://")) {
        return "whip";
    }

    return "unknown";
}
