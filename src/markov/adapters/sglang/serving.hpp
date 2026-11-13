#pragma once

#include <cstddef>
#include <map>

#include <nlohmann/json.hpp>

#include "markov/templates/library.hpp"

namespace markov {

class ControlPeer;
class ExecutionObserver;
class ResourceObserver;
class ControlObserver;

} // namespace markov

namespace markov::sglang {

nlohmann::json serve(const std::map<std::size_t, TemplateLibrary> & libraries, const nlohmann::json & task, ControlPeer & peer,
                     ExecutionObserver * execution = nullptr,
                     ResourceObserver * resources = nullptr, ControlObserver * control = nullptr);

} // namespace markov::sglang
