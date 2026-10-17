#pragma once

#include "markov/adapters/frameworks/ktransformers/configuration.hpp"
#include "markov/adapters/frameworks/sglang/configuration.hpp"
#include "markov/contracts/environment.hpp"

#include <filesystem>
#include <string>
#include <variant>

namespace markov {

enum class Framework { SGLang, KTransformers };

using FrameworkConfiguration = std::variant<SGLangConfiguration, KTransformersConfiguration>;

struct Candidate {
    std::string id;
    Framework framework;
    Environment environment;
    std::filesystem::path config_path;
    FrameworkConfiguration configuration;
};

} // namespace markov
