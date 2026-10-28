#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace markov::trace_graph::source {

enum class Channel { Torch, NativeHook, PythonProbe, Client };

struct ProfileFile {
    Channel channel;
    std::filesystem::path path;
    std::uintmax_t bytes;
    std::int64_t clock_offset_ns = 0;
    bool clock_recorded = true;
};

struct ProfileManifest {
    std::filesystem::path path;
    std::string framework;
    std::vector<ProfileFile> files;
    std::optional<std::filesystem::path> capture_origin_manifest = std::nullopt;
    std::optional<std::filesystem::path> configuration_path = std::nullopt;
};

// Resolve files relative to their capture after moving a complete profile.
// Validate the manifest and file accessibility without parsing trace contents.
ProfileManifest inspect_manifest(const std::filesystem::path & path);
std::string channel_name(Channel channel);

} // namespace markov::trace_graph::source
