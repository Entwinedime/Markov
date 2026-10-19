#pragma once

#include <cstddef>
#include <map>
#include <set>
#include <span>
#include <utility>
#include <vector>

namespace markov {

// Sparse cuts and cross-template boundaries retain their original identities.
// Large execution graphs use the compact, indexed DirectedGraph instead.
template <class Vertex> class DependencyPaths {
public:
    void connect(const Vertex & source, const Vertex & target) { successors_[source].push_back(target); }

    std::span<const Vertex> successors(const Vertex & vertex) const {
        const auto found = successors_.find(vertex);
        if (found == successors_.end()) return {};
        return found->second;
    }

    std::set<Vertex> reachable(std::vector<Vertex> pending) const {
        std::set<Vertex> reached(pending.begin(), pending.end());
        for (std::size_t cursor = 0; cursor < pending.size(); ++cursor)
            for (const auto & next : successors(pending[cursor]))
                if (reached.insert(next).second) pending.push_back(next);
        return reached;
    }

    // Remove only strict descendants of another candidate; incomparable
    // completions survive. Reverse edges to obtain the terminal frontier.
    std::set<Vertex> frontier(std::set<Vertex> candidates) const {
        std::vector<Vertex> pending;
        for (const auto & vertex : candidates) {
            const auto next = successors(vertex);
            pending.insert(pending.end(), next.begin(), next.end());
        }
        for (const auto & vertex : reachable(std::move(pending))) candidates.erase(vertex);
        return candidates;
    }

    DependencyPaths reversed() const {
        DependencyPaths result;
        for (const auto & [source, targets] : successors_)
            for (const auto & target : targets) result.connect(target, source);
        return result;
    }

private:
    std::map<Vertex, std::vector<Vertex>> successors_;
};

} // namespace markov
