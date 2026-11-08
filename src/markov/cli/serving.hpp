#pragma once

#include <cstddef>
#include <map>

#include <nlohmann/json.hpp>

#include "markov/templates/library.hpp"

namespace markov {

class ExecutionObserver;
class ResourceObserver;
class ControlObserver;

nlohmann::json serve(const std::map<std::size_t, TemplateLibrary> & libraries, const nlohmann::json & task, ExecutionObserver * execution = nullptr,
                     ResourceObserver * resources = nullptr, ControlObserver * control = nullptr);

} // namespace markov
