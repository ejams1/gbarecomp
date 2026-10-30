#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace emerald_media {

using AssetResolver = const char* (*)(void* context, const char* asset_id);

class GalleryMediaPackage {
public:
    bool load(const char* manifest_path, AssetResolver resolver,
              void* resolver_context, std::string* error);
    void clear();

    std::size_t frame_count() const { return frames_.size(); }
    const std::uint16_t* frame(std::size_t index) const;
    const std::vector<std::int16_t>& audio() const { return audio_; }
    std::uint32_t total_vblanks() const { return total_vblanks_; }
    std::uint32_t total_samples() const { return total_samples_; }
    std::uint16_t scene_id() const { return scene_id_; }
    std::uint16_t media_id(std::size_t frame_index) const;
    bool loops() const { return loops_; }
    std::size_t frame_for_elapsed(std::uint64_t elapsed_vblanks) const;
    std::size_t frame_for_elapsed(std::uint64_t elapsed_vblanks,
                                  bool loop) const;
    std::size_t sample_for_elapsed(std::uint64_t elapsed_vblanks) const;
    std::size_t sample_for_elapsed(std::uint64_t elapsed_vblanks,
                                   bool loop) const;
    std::size_t read_audio(std::size_t offset, std::int16_t* dst,
                           std::size_t count, bool loop) const;

private:
    static constexpr std::size_t kPixels = 240u * 160u;
    struct TimelineFrame {
        std::uint32_t start_vblank = 0;
        std::uint32_t end_vblank = 0;
        std::uint32_t start_sample = 0;
        std::uint32_t end_sample = 0;
        std::string media_id;
    };
    std::vector<std::vector<std::uint16_t>> frames_;
    std::vector<TimelineFrame> timeline_;
    std::vector<std::int16_t> audio_;
    std::uint32_t total_vblanks_ = 0;
    std::uint32_t total_samples_ = 0;
    std::uint16_t scene_id_ = 0;
    std::uint16_t media_id_base_ = 0;
    bool loops_ = false;
};

}  // namespace emerald_media
