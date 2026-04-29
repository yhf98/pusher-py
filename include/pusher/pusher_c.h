#ifndef PUSHER_PUSHER_C_H
#define PUSHER_PUSHER_C_H

#include <stddef.h>
#include <stdint.h>

#include "pusher/export.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pusher_handle pusher_handle_t;

enum {
    PUSHER_OK = 0,
    PUSHER_WAIT_TIMEOUT = 1,
    PUSHER_ERROR_INVALID_ARGUMENT = -1,
    PUSHER_ERROR_RUNTIME = -2,
    PUSHER_ERROR_NO_MEMORY = -3,
    PUSHER_ERROR_UNKNOWN = -255,
};

typedef struct pusher_config {
    const char *name;
    int timeout_ms;
    int auto_reconnect;
    const char *engine;
    const char *log_path;
    int loop;
    int realtime;
    int width;
    int height;
    int fps;
    int bitrate;
    int64_t analyzeduration_us;
    int64_t probesize;
} pusher_config_t;

typedef struct pusher_url_config {
    const char *protocol;
    const char *host;
    const char *app;
    const char *stream;
    const char *secret;
    int port;
    int use_tls;
} pusher_url_config_t;

PUSHER_API const char *pusher_version(void);
PUSHER_API void pusher_config_init(pusher_config_t *config);
PUSHER_API pusher_handle_t *pusher_create(const pusher_config_t *config);
PUSHER_API void pusher_destroy(pusher_handle_t *handle);

PUSHER_API int pusher_start(pusher_handle_t *handle,
                            const char *input,
                            const char *output_url);
PUSHER_API int pusher_stop(pusher_handle_t *handle, int timeout_ms);
PUSHER_API int pusher_wait(pusher_handle_t *handle,
                           int timeout_ms,
                           int *exit_code);
PUSHER_API int pusher_is_running(pusher_handle_t *handle);
PUSHER_API int pusher_exit_code(pusher_handle_t *handle, int *exit_code);

PUSHER_API int pusher_status(pusher_handle_t *handle,
                             char *buffer,
                             size_t buffer_size);
PUSHER_API int pusher_preview_command(pusher_handle_t *handle,
                                      const char *input,
                                      const char *output_url,
                                      char *buffer,
                                      size_t buffer_size);
PUSHER_API int pusher_last_error(pusher_handle_t *handle,
                                 char *buffer,
                                 size_t buffer_size);

PUSHER_API const char *pusher_detect_output_protocol(const char *url);
PUSHER_API int pusher_build_output_url(const pusher_url_config_t *config,
                                       char *buffer,
                                       size_t buffer_size);

#ifdef __cplusplus
}
#endif

#endif
