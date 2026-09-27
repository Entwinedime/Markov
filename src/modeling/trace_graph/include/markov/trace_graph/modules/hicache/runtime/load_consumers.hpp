#pragma once
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace markov::trace_graph::modules::hicache::runtime {

/** A forward keeps its layer events after DMA completion and scheduler ACK.
 * An empty submission still creates a new, inactive EXTEND consumer. */
class HiCacheLoadConsumers {
public:
    void submitted(const std::string & pid, std::vector<size_t> records) {
        if (!pending_.emplace(pid, std::move(records)).second)
            throw std::runtime_error("Load consumer submission has no intervening EXTEND");
    }

    const std::vector<size_t> & forward(const std::string & pid, const std::string & request, bool extend) {
        const auto key = std::pair{ pid, request };
        if (extend) {
            const auto pending = pending_.find(pid);
            if (pending == pending_.end()) throw std::runtime_error("EXTEND consumer has no load submission");
            active_.insert_or_assign(key, std::move(pending->second));
            pending_.erase(pending);
        }
        const auto active = active_.find(key);
        if (active == active_.end()) throw std::runtime_error("Decode consumer has no preceding EXTEND");
        return active->second;
    }

private:
    std::map<std::string, std::vector<size_t>> pending_;
    std::map<std::pair<std::string, std::string>, std::vector<size_t>> active_;
};

} // namespace markov::trace_graph::modules::hicache::runtime
