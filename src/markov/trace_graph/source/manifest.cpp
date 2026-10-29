#include "markov/trace_graph/source/manifest.hpp"

#include <fstream>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

namespace markov::trace_graph::source {
namespace {

std::filesystem::path captured_path(const std::filesystem::path & manifest, const std::filesystem::path & origin, const std::filesystem::path & declared) {
    if (declared.is_relative()) return manifest.parent_path() / declared;

    // The manifest's recorded capture root is a relocation rule, not a guess
    // based on whether another file happens to exist on this machine.
    const auto relative = declared.lexically_relative(origin);
    if (!relative.empty() && *relative.begin() != "..") return manifest.parent_path() / relative;

    return declared;
}

void append_files(ProfileManifest & result, const nlohmann::json & entries, const std::filesystem::path & origin, Channel channel) {
    for (const auto & entry : entries) {
        auto path = captured_path(result.path, origin, entry.at("path").get<std::string>()).lexically_normal();
        const auto bytes = std::filesystem::file_size(path);
        std::int64_t offset_ns = 0;
        const bool recorded = channel != Channel::Torch || entry.contains("host_clock");
        if (entry.contains("host_clock")) {
            const auto & clock = entry.at("host_clock");
            offset_ns = clock.at("unix_origin_ns").get<std::int64_t>() - clock.at("origin_ns").get<std::int64_t>();
        }
        auto format = RecordFormat::Chrome;
        if (entry.contains("format")) {
            const auto name = entry.at("format").get<std::string>();
            if (name == "jsonl" && channel == Channel::PythonProbe) format = RecordFormat::JsonLines;
            else if (name != "chrome") throw std::runtime_error("Unsupported record format for " + channel_name(channel) + ": " + name);
        }
        result.files.push_back({ channel, std::move(path), bytes, offset_ns, recorded, format });
    }
}

} // namespace

ProfileManifest inspect_manifest(const std::filesystem::path & path) {
    std::ifstream stream(path);
    if (!stream) throw std::runtime_error("Cannot read profile manifest: " + path.string());

    const auto raw = nlohmann::json::parse(stream);
    if (!raw.at("profiling_ready").get<bool>() || raw.at("dry_run").get<bool>())
        throw std::runtime_error("Profile is not a completed real capture: " + path.string());

    ProfileManifest result{ std::filesystem::absolute(path).lexically_normal(), raw.at("framework").get<std::string>(), {} };
    if (raw.contains("capture_origin_manifest")) result.capture_origin_manifest = raw.at("capture_origin_manifest").get<std::string>();
    const std::filesystem::path origin = raw.at("run_dir").get<std::string>();
    if (raw.contains("config_path")) {
        result.configuration_path = captured_path(result.path, origin, raw.at("config_path").get<std::string>()).lexically_normal();
        (void)std::filesystem::file_size(*result.configuration_path);
    }
    const auto & trace = raw.at("trace");
    append_files(result, trace.at("torch_trace_files"), origin, Channel::Torch);
    append_files(result, trace.at("ld_preload_trace_files"), origin, Channel::NativeHook);
    append_files(result, raw.at("sidecar").at("python_probe_files"), origin, Channel::PythonProbe);
    append_files(result, raw.at("bench").at("workload_report_files"), origin, Channel::Client);
    return result;
}

std::string channel_name(Channel channel) {
    switch (channel) {
    case Channel::Torch:
        return "torch";
    case Channel::NativeHook:
        return "native_hook";
    case Channel::PythonProbe:
        return "python_probe";
    case Channel::Client:
        return "client";
    }

    std::unreachable();
}

} // namespace markov::trace_graph::source
