#pragma once

#include <cstddef>

namespace gbarecomp { struct RunOptions; }

namespace emerald_media {
void configure_scene_media(gbarecomp::RunOptions& options);
int scene_media_tcp_command(
    const char* request,
    void (*write)(void* ctx, const char* data, std::size_t len),
    void* write_ctx);
}
