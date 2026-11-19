#pragma once

#include <deque>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "markov/templates/library.hpp"

namespace markov {

class TemplateGap final : public std::invalid_argument {
public:
    explicit TemplateGap(nlohmann::json details);
    const nlohmann::json & details() const { return details_; }

private:
    nlohmann::json details_;
};

// The immutable library outlives this index; dynamic target choices never modify its source candidates.
class TemplateSelection {
public:
    explicit TemplateSelection(const TemplateLibrary & library);
    TemplateSelection(const TemplateSelection &) = delete;
    TemplateSelection & operator=(const TemplateSelection &) = delete;
    TemplateEntry select(std::string_view action, const nlohmann::json & condition, const Parameters & parameters,
                         const std::map<std::string, StateBindings> & state_inputs = {}) const;

private:
    using Family = std::vector<const TemplateFrame *>;
    const TemplateLibrary & library_;
    std::deque<TemplateFrame> alternatives_;
    std::vector<Family> families_;
    std::map<const TemplateFrame *, std::size_t> membership_;
    std::map<std::pair<std::string, nlohmann::json>, std::set<std::size_t>> roots_;
};

} // namespace markov
