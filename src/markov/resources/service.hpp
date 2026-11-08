#pragma once

#include <cstddef>
#include <map>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

#include "markov/resources/changes.hpp"
#include "markov/resources/kv_pool.hpp"
#include "markov/resources/slot_pool.hpp"

namespace markov {

// Native control requests ownership changes; graph execution receives the returned physical pages.
class ResourceService {
public:
    explicit ResourceService(KvPool & pool);
    nlohmann::json call(const nlohmann::json & message, const std::set<std::size_t> & delivered);
    ResourceChanges take_changes();
    std::size_t free_pages() const;
    std::size_t slot(const std::string & field, std::size_t request) const;
    const std::set<std::string> & request_fields() const;

private:
    std::size_t request(const nlohmann::json & identity, const std::set<std::size_t> & delivered) const;
    KvPool & pool_;
    ResourceChanges changes_;
    std::set<std::string> request_fields_;
    std::map<std::string, SlotPool> fields_;
};

} // namespace markov
