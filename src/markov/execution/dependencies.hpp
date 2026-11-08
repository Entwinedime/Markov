#pragma once

#include <cstddef>
#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "markov/execution/timeline.hpp"
#include "markov/resources/kv_pool.hpp"
#include "markov/resources/observer.hpp"
#include "markov/resources/service.hpp"

namespace markov {

// One pool's physical contents; logical allocation and prefix ownership stay in KvPool.
class GraphDependencies {
public:
    GraphDependencies(KvPool & pool, ResourceService & resources, std::span<const Request> requests, ResourceObserver * observer = nullptr);
    std::vector<std::size_t> insert(const CoarseGraph & graph, Timeline & timeline, std::span<const std::int64_t> durations,
                                    const std::map<std::string, GraphDependencies *> & pools, std::span<const BoundaryTime> boundaries = {});
    void invalidate(const ResourceChanges & changes);
    std::optional<std::size_t> signal(const std::string & name) const;
    std::set<std::size_t> retain(const std::set<std::string> & bindings, const Timeline & timeline);

private:
    struct Access {
        std::int64_t begin;
        std::int64_t end;
        bool write;
        std::size_t start;
        std::size_t finish;

        auto operator<=>(const Access &) const = default;
    };

    struct Contents {
        std::vector<Access> active;
        std::vector<std::pair<std::int64_t, std::int64_t>> initialized;

        void prune(const Timeline & timeline);
        bool covers(std::int64_t begin, std::int64_t end) const;
        void commit(const std::set<Access> & accesses, std::span<const std::size_t> events);
    };

    KvPool & pool_;
    ResourceService & resources_;
    ResourceObserver * observer_;
    std::map<std::string, std::size_t> requests_;
    std::map<std::string, Contents> tensors_;
    std::map<std::pair<std::size_t, std::int64_t>, Contents> pages_;
    std::map<std::pair<std::size_t, std::string>, Contents> slots_;
    std::map<std::string, std::pair<std::int64_t, std::size_t>> values_;
    std::map<std::string, std::size_t> signals_;
};

} // namespace markov
