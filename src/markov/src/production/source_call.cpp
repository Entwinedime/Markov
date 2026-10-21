#include "markov/production/source_call.hpp"

#include "markov/contracts/json_input.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <limits>
#include <stdexcept>

namespace markov {

SourceAdmissionPolicy SourceAdmissionPolicy::read(const nlohmann::json & value) {
    SourceAdmissionPolicy observed{
        { value.at("initial_ratio").get<double>(),
         value.at("minimum_ratio").get<double>(),
         value.at("decay_per_decode").get<double>(),
         input::positive(value.at("output_estimate_limit_tokens")),
         static_cast<std::uint32_t>(input::integer(value.at("reserve_tokens_per_request"), 0, std::numeric_limits<std::uint32_t>::max())) },
        value.at("entry_ratio").get<double>()
    };
    const auto & policy = observed.parameters;
    for (const auto ratio : { policy.initial_ratio, policy.minimum_ratio, policy.decay_per_decode, observed.entry_ratio })
        if (!std::isfinite(ratio) || ratio < 0 || ratio > 1) throw std::invalid_argument("native admission ratios must be finite values in [0, 1]");
    if (policy.minimum_ratio > policy.initial_ratio) throw std::invalid_argument("native minimum admission ratio exceeds its initial value");
    return observed;
}

} // namespace markov
