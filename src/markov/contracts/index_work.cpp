#include "markov/contracts/index_work.hpp"

#include <map>
#include <stdexcept>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace markov {

IndexWork IndexWork::read(const nlohmann::json & source) {
    static const std::map<std::string, IndexService> services{
        {           "match",          IndexService::Match },
        {     "empty_match",     IndexService::EmptyMatch },
        {          "insert",         IndexService::Insert },
        {     "insert_path",     IndexService::InsertPath },
        {           "split",          IndexService::Split },
        {         "acquire",        IndexService::Acquire },
        {         "release",        IndexService::Release },
        {  "retain_request",  IndexService::RetainRequest },
        { "release_request", IndexService::ReleaseRequest },
        {           "evict",          IndexService::Evict }
    };
    static const std::map<std::string, IndexReleaseMode> modes{
        { "immediate", IndexReleaseMode::Immediate },
        {  "deferred",  IndexReleaseMode::Deferred },
        {     "group",     IndexReleaseMode::Group }
    };
    IndexWork result;
    std::vector<IndexService> calls;
    result.live_indices = source.at("live_indices").get<std::vector<std::size_t>>();
    for (const auto & item : source.at("operations")) {
        const auto kind = item.at("kind").get<std::string>();
        if (kind == "call") {
            const IndexCall call{ services.at(item.at("service").get<std::string>()), item.at("enter").get<bool>() };
            if (call.enter) calls.push_back(call.service);
            else {
                if (calls.empty() || calls.back() != call.service) throw std::invalid_argument("index service return has no matching call");
                calls.pop_back();
            }
            result.operations.emplace_back(call);
        }
        else if (kind == "read")
            result.operations.emplace_back(IndexRead{ item.at("request").get<std::size_t>(),
                                                      item.at("output").get<std::size_t>(),
                                                      item.at("begin").get<std::int64_t>(),
                                                      item.at("tokens").get<std::int64_t>() });
        else if (kind == "slice")
            result.operations.emplace_back(IndexSlice{ item.at("input").get<std::size_t>(),
                                                       item.at("output").get<std::size_t>(),
                                                       item.at("begin").get<std::int64_t>(),
                                                       item.at("tokens").get<std::int64_t>() });
        else if (kind == "copy") result.operations.emplace_back(IndexCopy{ item.at("input").get<std::size_t>(), item.at("output").get<std::size_t>() });
        else if (kind == "concat") {
            auto inputs = item.at("inputs").get<std::vector<std::size_t>>();
            if (inputs.empty()) throw std::invalid_argument("index concatenation requires input views");
            result.operations.emplace_back(IndexConcat{ std::move(inputs), item.at("output").get<std::size_t>() });
        }
        else if (kind == "write")
            result.operations.emplace_back(IndexWrite{ item.at("request").get<std::size_t>(),
                                                       item.at("input").get<std::size_t>(),
                                                       item.at("begin").get<std::int64_t>(),
                                                       item.at("tokens").get<std::int64_t>() });
        else if (kind == "release")
            result.operations.emplace_back(IndexRelease{ modes.at(item.at("mode").get<std::string>()),
                                                         item.at("inputs").get<std::vector<std::size_t>>(),
                                                         item.at("pages").get<std::vector<std::size_t>>(),
                                                         item.at("pending_tokens_before").get<std::int64_t>() });
        else throw std::invalid_argument("unknown index operation: " + kind);
    }
    if (!calls.empty() || result.operations.empty()) throw std::invalid_argument("index work requires operations and closed services");
    if (source.contains("binding")) {
        const auto & binding = source.at("binding");
        result.binding =
            PrefixBinding{ binding.at("request").get<std::size_t>(), binding.at("node").get<std::size_t>(), binding.at("tokens").get<std::int64_t>() };
    }
    if (source.contains("prefix")) {
        const auto & prefix = source.at("prefix");
        result.prefix = RequestPrefix{ prefix.at("request").get<std::size_t>(), prefix.at("indices").get<std::size_t>() };
    }
    if (source.contains("release_request")) result.release_request = source.at("release_request").get<std::size_t>();
    return result;
}

} // namespace markov
