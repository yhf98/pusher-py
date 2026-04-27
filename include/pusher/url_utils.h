#ifndef PUSHER_URL_UTILS_H
#define PUSHER_URL_UTILS_H

#ifdef __cplusplus
extern "C" {
#endif

int pusher_has_prefix(const char *text, const char *prefix);
const char *pusher_detect_protocol(const char *url);

#ifdef __cplusplus
}
#endif

#endif
