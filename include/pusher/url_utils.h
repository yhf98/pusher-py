#ifndef PUSHER_URL_UTILS_H
#define PUSHER_URL_UTILS_H

#include "pusher/export.h"

#ifdef __cplusplus
extern "C" {
#endif

PUSHER_API int pusher_has_prefix(const char *text, const char *prefix);
PUSHER_API const char *pusher_detect_protocol(const char *url);

#ifdef __cplusplus
}
#endif

#endif
