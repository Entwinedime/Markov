#pragma once

#include <filesystem>

namespace markov {

struct TemplateSource;

// Compiled, source-owned templates. Raw profiler files are production inputs only.
class TemplateAsset {
public:
    static void write(const std::filesystem::path & path, const TemplateSource & source);
    static TemplateSource read(const std::filesystem::path & path);
};

} // namespace markov
