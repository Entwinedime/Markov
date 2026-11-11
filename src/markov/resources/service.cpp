#include "markov/resources/service.hpp"

#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace markov {

ResourceService::ResourceService(KvPool & pool) : pool_(pool) {}

std::size_t ResourceService::request(const nlohmann::json & identity, const std::set<std::size_t> & delivered) const {
    if (!identity.is_number_integer() || identity.get<std::int64_t>() < 0 || !delivered.contains(identity.get<std::size_t>()))
        throw std::invalid_argument("resource request was not delivered to this instance");
    return identity.get<std::size_t>();
}

nlohmann::json ResourceService::call(const nlohmann::json & message, const std::set<std::size_t> & delivered) {
    const auto kind = message.at("kind").get<std::string>();
    if (kind == "declare_resources") {
        const auto fields = message.at("fields").get<std::set<std::string>>();
        for (const auto & field : fields) {
            if (field.empty() || fields_.contains(field)) throw std::invalid_argument("request slot field conflicts with another pool");
        }
        const auto reuse = message.at("page_reuse").get<std::string>();
        if (reuse != "prepend" && reuse != "deferred_sorted") throw std::invalid_argument("unknown page reuse policy: " + reuse);
        pool_.declare_resources(message.at("slots").get<std::size_t>(),
                                reuse == "prepend" ? PageReuse::Prepend : PageReuse::DeferredSorted);
        request_fields_ = fields;
        return nullptr;
    }
    if (kind == "state_slots") {
        const auto field = message.at("field").get<std::string>();
        const auto operation = message.at("operation").get<std::string>();
        if (operation == "declare") {
            const auto capacity = read_parameters({{"capacity", message.at("capacity")}}).at("capacity");
            if (field.empty() || request_fields_.contains(field) || fields_.contains(field) || capacity <= 0)
                throw std::invalid_argument("state slot declaration requires a new field and positive capacity");
            fields_.try_emplace(field, static_cast<std::size_t>(capacity));
            return nullptr;
        }
        auto & slots = fields_.at(field);
        if (operation == "available") return slots.available();
        if (operation == "allocate") {
            const auto slot = slots.allocate();
            return slot ? nlohmann::json(*slot) : nlohmann::json();
        }
        const auto index = read_parameters({{"slot", message.at("slot")}}).at("slot");
        if (index < 0) throw std::invalid_argument("state slot index must be nonnegative");
        const auto slot = static_cast<std::size_t>(index);
        if (operation == "bind") {
            slots.bind(request(message.at("request"), delivered), slot);
            return nullptr;
        }
        if (operation == "release") {
            slots.release(slot);
            changes_.returned_fields.emplace_back(field, slot);
            changes_.capacity_increased = true;
            return nullptr;
        }
        throw std::invalid_argument("unknown state slot operation: " + operation);
    }
    if (kind == "free_slots") return pool_.free_slots();
    if (kind == "capacity") {
        const auto resource = message.at("resource").get<std::string>();
        if (resource == "slots") return pool_.free_slots().size();
        if (resource == "pages") return pool_.free_pages().size();
        if (resource == "evictable_tokens") return pool_.prefixes().evictable_tokens();
        if (resource == "protected_tokens") return pool_.prefixes().protected_tokens();
        throw std::invalid_argument("unknown capacity resource: " + resource);
    }

    if (kind == "prefix") {
        auto & index = pool_.prefixes();
        const auto operation = message.at("operation").get<std::string>();
        if (operation == "match") {
            const auto match = index.match(message.at("tokens").get<std::vector<std::int64_t>>());
            return {
                {  "node",                match.node },
                { "pages", index.binding(match.node) }
            };
        }
        if (operation == "insert") {
            const auto pages = message.at("pages").get<std::vector<std::size_t>>();
            for (const auto page : pages) {
                if (page >= pool_.capacity().pages) throw std::invalid_argument("cached page lies outside its pool");
            }
            const auto before = index.evictable_tokens();
            const auto inserted = index.insert(message.at("tokens").get<std::vector<std::int64_t>>(), pages);
            changes_.capacity_increased |= index.evictable_tokens() > before;
            return {
                {            "node",            inserted.node },
                { "existing_tokens", inserted.existing_tokens }
            };
        }
        if (operation == "lock") {

            // Unlocking cached pages can unblock admission without returning them to the free list.
            const auto delta = index.lock(message.at("node").get<std::size_t>(), message.at("acquire").get<bool>());
            changes_.capacity_increased |= delta > 0;
            return delta;
        }
        if (operation == "evict") return index.evict(message.at("tokens").get<std::int64_t>());
        throw std::invalid_argument("unknown prefix operation: " + operation);
    }

    if (kind == "slots" || kind == "allocate") {
        std::vector<KvDemand> demands;
        std::set<std::size_t> members;
        for (const auto & member : message.at("members")) {
            const auto id = request(member.at("request"), delivered);
            if (!members.insert(id).second) throw std::invalid_argument("allocation repeats a request");
            std::int64_t tokens = 0;
            if (kind == "allocate") {
                const auto demand = read_parameters({
                    { "tokens", member.at("tokens") }
                });
                tokens = demand.at("tokens");
                if (tokens < 0) throw std::invalid_argument("allocation has a negative token demand");
            }

            if (kind == "slots" && member.contains("prefix")) {
                const auto & prefix = member.at("prefix");
                pool_.bind_prefix(id, prefix.at("node").get<std::size_t>(), prefix.at("tokens").get<std::int64_t>());
            }
            demands.push_back({ id, tokens });
        }
        const auto allocation = pool_.allocate(demands);
        auto bindings = nlohmann::json::array();
        for (const auto & member : allocation.members)
            bindings.push_back({
                { "slot", member.slot },
                { "new_pages", member.new_pages },
                { "indices", pool_.indices(member.request, member.context_tokens, member.context_tokens + member.new_tokens) }
            });
        if (kind == "slots") return bindings;

        // Ordering changes the index computation's execution, not the ownership of a second page pool.
        nlohmann::json ordering;
        if (allocation.ordering)
            ordering = { { "ready_pages", allocation.ordering->ready_pages }, { "released_pages", allocation.ordering->released_pages } };
        return { { "members", std::move(bindings) }, { "ordering", std::move(ordering) } };
    }
    if (kind == "bind_prefix") {
        const auto id = request(message.at("request"), delivered);
        pool_.bind_prefix(id, message.at("node").get<std::size_t>(), message.at("tokens").get<std::int64_t>());
        return nullptr;
    }
    if (kind == "release_request") {
        const auto id = request(message.at("request"), delivered);
        const auto slot = pool_.residence(id).slot.value();
        pool_.release(id);
        changes_.returned_slots.push_back(slot);
        changes_.capacity_increased = true;
        return nullptr;
    }
    if (kind == "release_pages") {
        auto pages = message.at("pages").get<std::vector<std::size_t>>();
        for (const auto page : pages) {
            if (page >= pool_.capacity().pages) throw std::invalid_argument("returned page lies outside its pool");
        }
        pool_.return_pages(pages);
        changes_.returned_pages.insert(changes_.returned_pages.end(), pages.begin(), pages.end());
        changes_.capacity_increased |= !pages.empty();
        return nullptr;
    }
    throw std::invalid_argument("unknown resource operation: " + kind);
}

ResourceChanges ResourceService::take_changes() { return std::exchange(changes_, {}); }
std::size_t ResourceService::free_pages() const { return pool_.free_pages().size(); }

std::size_t ResourceService::slot(const std::string & field, std::size_t request) const {
    return request_fields_.contains(field) ? pool_.residence(request).slot.value() : fields_.at(field).slot(request);
}

const std::set<std::string> & ResourceService::request_fields() const { return request_fields_; }

} // namespace markov
