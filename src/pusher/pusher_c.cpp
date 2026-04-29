#include "pusher/pusher_c.h"

#include "pusher/pusher.hpp"
#include "pusher/url_utils.h"
#include "pusher/version.h"

#include <cstring>
#include <exception>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

struct pusher_handle {
    pusher::NativePusher *impl = nullptr;
    std::string last_error;
};

namespace {

static pusher::PusherConfig to_cpp_config(const pusher_config_t *config) {
    pusher::PusherConfig out;
    if (config == nullptr) {
        return out;
    }

    out.name = config->name == nullptr ? "default" : config->name;
    out.timeout_ms = config->timeout_ms > 0 ? config->timeout_ms : 5000;
    out.auto_reconnect = config->auto_reconnect != 0;
    out.engine = config->engine == nullptr ? "auto" : config->engine;
    out.log_path = config->log_path == nullptr ? "" : config->log_path;
    out.loop = config->loop != 0;
    out.realtime = config->realtime != 0;
    out.width = config->width > 0 ? config->width : 1280;
    out.height = config->height > 0 ? config->height : 720;
    out.fps = config->fps > 0 ? config->fps : 30;
    out.bitrate = config->bitrate > 0 ? config->bitrate : 2000000;
    out.analyzeduration_us = config->analyzeduration_us > 0 ? config->analyzeduration_us : 10000000;
    out.probesize = config->probesize > 0 ? config->probesize : 50000000;
    return out;
}

static int copy_string(const std::string &value, char *buffer, size_t buffer_size) {
    const size_t required = value.size() + 1;
    if (buffer == nullptr || buffer_size == 0) {
        return static_cast<int>(required);
    }

    const size_t copy_size = required <= buffer_size ? value.size() : buffer_size - 1;
    if (copy_size > 0) {
        std::memcpy(buffer, value.data(), copy_size);
    }
    buffer[copy_size] = '\0';
    return required <= buffer_size ? PUSHER_OK : static_cast<int>(required);
}

static std::string join_command(const std::vector<std::string> &command) {
    std::ostringstream oss;
    for (size_t i = 0; i < command.size(); ++i) {
        if (i > 0) {
            oss << ' ';
        }
        oss << command[i];
    }
    return oss.str();
}

static int set_error(pusher_handle_t *handle, const std::string &message, int code) {
    if (handle != nullptr) {
        handle->last_error = message;
    }
    return code;
}

static int map_exception(pusher_handle_t *handle, const std::exception &error) {
    if (dynamic_cast<const std::invalid_argument *>(&error) != nullptr) {
        return set_error(handle, error.what(), PUSHER_ERROR_INVALID_ARGUMENT);
    }
    if (dynamic_cast<const std::bad_alloc *>(&error) != nullptr) {
        return set_error(handle, error.what(), PUSHER_ERROR_NO_MEMORY);
    }
    return set_error(handle, error.what(), PUSHER_ERROR_RUNTIME);
}

}  // namespace

const char *pusher_version(void) {
    return PUSHER_VERSION;
}

void pusher_config_init(pusher_config_t *config) {
    if (config == nullptr) {
        return;
    }
    config->name = "default";
    config->timeout_ms = 5000;
    config->auto_reconnect = 1;
    config->engine = "auto";
    config->log_path = "";
    config->loop = 1;
    config->realtime = 1;
    config->width = 1280;
    config->height = 720;
    config->fps = 30;
    config->bitrate = 2000000;
    config->analyzeduration_us = 10000000;
    config->probesize = 50000000;
}

pusher_handle_t *pusher_create(const pusher_config_t *config) {
    pusher_handle_t *handle = nullptr;
    try {
        handle = new pusher_handle;
        handle->impl = new pusher::NativePusher(to_cpp_config(config));
        return handle;
    } catch (const std::exception &error) {
        if (handle != nullptr) {
            delete handle->impl;
            delete handle;
        }
        return nullptr;
    }
}

void pusher_destroy(pusher_handle_t *handle) {
    if (handle == nullptr) {
        return;
    }
    delete handle->impl;
    delete handle;
}

int pusher_start(pusher_handle_t *handle, const char *input, const char *output_url) {
    if (handle == nullptr || handle->impl == nullptr) {
        return PUSHER_ERROR_INVALID_ARGUMENT;
    }
    try {
        handle->impl->start(input == nullptr ? "" : input, output_url == nullptr ? "" : output_url);
        handle->last_error.clear();
        return PUSHER_OK;
    } catch (const std::exception &error) {
        return map_exception(handle, error);
    } catch (...) {
        return set_error(handle, "unknown error", PUSHER_ERROR_UNKNOWN);
    }
}

int pusher_stop(pusher_handle_t *handle, int timeout_ms) {
    if (handle == nullptr || handle->impl == nullptr) {
        return PUSHER_ERROR_INVALID_ARGUMENT;
    }
    try {
        handle->impl->stop(timeout_ms);
        handle->last_error.clear();
        return PUSHER_OK;
    } catch (const std::exception &error) {
        return map_exception(handle, error);
    } catch (...) {
        return set_error(handle, "unknown error", PUSHER_ERROR_UNKNOWN);
    }
}

int pusher_wait(pusher_handle_t *handle, int timeout_ms, int *exit_code) {
    if (handle == nullptr || handle->impl == nullptr) {
        return PUSHER_ERROR_INVALID_ARGUMENT;
    }
    try {
        int code = -1;
        bool completed = handle->impl->wait(timeout_ms, code);
        if (exit_code != nullptr) {
            *exit_code = code;
        }
        return completed ? PUSHER_OK : PUSHER_WAIT_TIMEOUT;
    } catch (const std::exception &error) {
        return map_exception(handle, error);
    } catch (...) {
        return set_error(handle, "unknown error", PUSHER_ERROR_UNKNOWN);
    }
}

int pusher_is_running(pusher_handle_t *handle) {
    if (handle == nullptr || handle->impl == nullptr) {
        return 0;
    }
    try {
        return handle->impl->is_running() ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

int pusher_exit_code(pusher_handle_t *handle, int *exit_code) {
    if (handle == nullptr || handle->impl == nullptr || exit_code == nullptr) {
        return PUSHER_ERROR_INVALID_ARGUMENT;
    }
    *exit_code = handle->impl->exit_code();
    return PUSHER_OK;
}

int pusher_status(pusher_handle_t *handle, char *buffer, size_t buffer_size) {
    if (handle == nullptr || handle->impl == nullptr) {
        return PUSHER_ERROR_INVALID_ARGUMENT;
    }
    try {
        return copy_string(handle->impl->status(), buffer, buffer_size);
    } catch (const std::exception &error) {
        return map_exception(handle, error);
    } catch (...) {
        return set_error(handle, "unknown error", PUSHER_ERROR_UNKNOWN);
    }
}

int pusher_preview_command(pusher_handle_t *handle,
                           const char *input,
                           const char *output_url,
                           char *buffer,
                           size_t buffer_size) {
    if (handle == nullptr || handle->impl == nullptr) {
        return PUSHER_ERROR_INVALID_ARGUMENT;
    }
    try {
        return copy_string(
            join_command(handle->impl->preview_command(input == nullptr ? "" : input,
                                                       output_url == nullptr ? "" : output_url)),
            buffer,
            buffer_size);
    } catch (const std::exception &error) {
        return map_exception(handle, error);
    } catch (...) {
        return set_error(handle, "unknown error", PUSHER_ERROR_UNKNOWN);
    }
}

int pusher_last_error(pusher_handle_t *handle, char *buffer, size_t buffer_size) {
    if (handle == nullptr) {
        return PUSHER_ERROR_INVALID_ARGUMENT;
    }
    return copy_string(handle->last_error, buffer, buffer_size);
}

const char *pusher_detect_output_protocol(const char *url) {
    return pusher_detect_protocol(url);
}

int pusher_build_output_url(const pusher_url_config_t *config, char *buffer, size_t buffer_size) {
    if (config == nullptr) {
        return PUSHER_ERROR_INVALID_ARGUMENT;
    }
    try {
        std::string url = pusher::build_output_url(
            config->protocol == nullptr ? "" : config->protocol,
            config->host == nullptr ? "" : config->host,
            config->app == nullptr ? "" : config->app,
            config->stream == nullptr ? "" : config->stream,
            config->secret == nullptr ? "" : config->secret,
            config->port,
            config->use_tls != 0);
        return copy_string(url, buffer, buffer_size);
    } catch (const std::invalid_argument &) {
        return PUSHER_ERROR_INVALID_ARGUMENT;
    } catch (const std::bad_alloc &) {
        return PUSHER_ERROR_NO_MEMORY;
    } catch (...) {
        return PUSHER_ERROR_UNKNOWN;
    }
}
