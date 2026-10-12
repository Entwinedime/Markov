#include "markov/production/source_call.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <limits>
#include <stdexcept>

namespace markov {

SourceAdmissionPolicy SourceAdmissionPolicy::read(const nlohmann::json & value) {
    const auto limit = value.at("output_estimate_limit_tokens").get<std::int64_t>();
    const auto reserve = value.at("reserve_tokens_per_request").get<std::int64_t>();
    if (limit <= 0 || limit > std::numeric_limits<std::uint32_t>::max() || reserve < 0 || reserve > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("native admission token limits are outside their domain");

    SourceAdmissionPolicy observed{
        { value.at("initial_ratio").get<double>(),
         value.at("minimum_ratio").get<double>(),
         value.at("decay_per_decode").get<double>(),
         static_cast<std::uint32_t>(limit),
         static_cast<std::uint32_t>(reserve) },
        value.at("entry_ratio").get<double>()
    };
    const auto & policy = observed.parameters;
    for (const auto ratio : { policy.initial_ratio, policy.minimum_ratio, policy.decay_per_decode, observed.entry_ratio })
        if (!std::isfinite(ratio) || ratio < 0 || ratio > 1) throw std::invalid_argument("native admission ratios must be finite values in [0, 1]");
    if (policy.minimum_ratio > policy.initial_ratio) throw std::invalid_argument("native minimum admission ratio exceeds its initial value");
    return observed;
}

nlohmann::json SourceAdmissionPolicy::describe() const {
    return {
        {                "initial_ratio",                parameters.initial_ratio },
        {                "minimum_ratio",                parameters.minimum_ratio },
        {             "decay_per_decode",             parameters.decay_per_decode },
        {                  "entry_ratio",                             entry_ratio },
        { "output_estimate_limit_tokens", parameters.output_estimate_limit_tokens },
        {   "reserve_tokens_per_request",   parameters.reserve_tokens_per_request }
    };
}

SourceWorkerControls SourceWorkerControls::read(const nlohmann::json & value) {
    SourceWorkerControls observed{ value.at("waiting_timeout_seconds").get<double>(),
                                   value.at("running_timeout_seconds").get<double>(),
                                   value.at("test_retract").get<bool>(),
                                   value.at("require_mlp_sync").get<bool>() };
    if (!std::isfinite(observed.waiting_timeout_seconds) || !std::isfinite(observed.running_timeout_seconds))
        throw std::invalid_argument("native request timeouts must be finite seconds");
    return observed;
}

nlohmann::json SourceWorkerControls::describe() const {
    return {
        { "waiting_timeout_seconds", waiting_timeout_seconds },
        { "running_timeout_seconds", running_timeout_seconds },
        {            "test_retract",            test_retract },
        {        "require_mlp_sync",        require_mlp_sync }
    };
}

} // namespace markov
