#include "pusher/pusher_c.h"

#include <stdio.h>
#include <string.h>

static void print_error(pusher_handle_t *pusher, const char *action, int code) {
    char error[1024];
    int ret = pusher_last_error(pusher, error, sizeof(error));
    if (ret < 0 || error[0] == '\0') {
        snprintf(error, sizeof(error), "error code %d", code);
    }
    fprintf(stderr, "%s failed: %s\n", action, error);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s INPUT OUTPUT_URL [--start]\n", argv[0]);
        fprintf(stderr, "example: %s rtsp://127.0.0.1/live/test rtmp://127.0.0.1/live/test\n", argv[0]);
        return 2;
    }

    const int should_start = argc >= 4 && strcmp(argv[3], "--start") == 0;

    pusher_config_t config;
    pusher_config_init(&config);
    config.name = "c-demo";
    config.loop = 0;
    config.realtime = 0;

    pusher_handle_t *pusher = pusher_create(&config);
    if (pusher == NULL) {
        fprintf(stderr, "pusher_create failed\n");
        return 1;
    }

    printf("pusher version: %s\n", pusher_version());
    printf("output protocol: %s\n", pusher_detect_output_protocol(argv[2]));

    char text[2048];
    int ret = pusher_preview_command(pusher, argv[1], argv[2], text, sizeof(text));
    if (ret < 0) {
        print_error(pusher, "preview", ret);
        pusher_destroy(pusher);
        return 1;
    }
    printf("preview: %s\n", text);

    if (!should_start) {
        printf("dry run only; pass --start to start pushing\n");
        pusher_destroy(pusher);
        return 0;
    }

    ret = pusher_start(pusher, argv[1], argv[2]);
    if (ret != PUSHER_OK) {
        print_error(pusher, "start", ret);
        pusher_destroy(pusher);
        return 1;
    }

    ret = pusher_wait(pusher, -1, NULL);
    if (ret != PUSHER_OK) {
        print_error(pusher, "wait", ret);
    }

    ret = pusher_status(pusher, text, sizeof(text));
    if (ret >= 0) {
        printf("status: %s\n", text);
    }

    pusher_destroy(pusher);
    return 0;
}
