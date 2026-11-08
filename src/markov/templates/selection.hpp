#pragma once

#include <map>
#include <deque>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "markov/templates/library.hpp"

namespace markov {

// The immutable library outlives this index; dynamic target choices never modify its source candidates.
class TemplateSelection {
public:
    explicit TemplateSelection(const TemplateLibrary & library);
    TemplateSelection(const TemplateSelection &) = delete;
    TemplateSelection & operator=(const TemplateSelection &) = delete;
    TemplateEntry select(std::string_view action, const nlohmann::json & condition, const Parameters & parameters) const;

private:
    using Family = std::vector<const TemplateFrame *>;
    const TemplateLibrary & library_;
    std::deque<TemplateFrame> alternatives_;
    std::vector<Family> families_;
    std::map<const TemplateFrame *, std::size_t> membership_;
    std::map<std::pair<std::string, nlohmann::json>, std::set<std::size_t>> roots_;
};

} // namespace markov
