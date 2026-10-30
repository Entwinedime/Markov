#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <nlohmann/json.hpp>

#include "markov/contracts/serving.hpp"
#include "markov/controller/batch_replay.hpp"
#include "markov/controller/control_peer.hpp"
#include "markov/execution/durations.hpp"
#include "markov/templates/asset.hpp"

namespace markov {

class ResourceTrace;

// Assemble one serving run; CLI owns input assets and the optional native transport.
class ServingSimulation {
public:
    ServingSimulation(std::span<const TemplateAsset> assets, const nlohmann::json & input, const nlohmann::json & batches, ControlPeer * peer);
    nlohmann::json run(bool details);

private:
    std::int64_t prefill_chunk_tokens(const nlohmann::json & pool, const ServingSettings & settings) const;
    nlohmann::json joint(ResourceTrace * trace);
    nlohmann::json disaggregated(ResourceTrace * trace);

    std::span<const TemplateAsset> assets_;
    const nlohmann::json & input_;
    const nlohmann::json & configuration_;
    std::vector<Request> requests_;
    std::optional<BatchReplay> replay_;
    GivenDurations durations_;
    std::vector<std::int64_t> devices_;
    ControlPeer * peer_;
};

} // namespace markov
