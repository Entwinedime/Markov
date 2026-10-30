#pragma once

#include "markov/adapters/probes.hpp"

namespace markov::adapters::ktransformers {

class ProbeReader final : public adapters::ProbeReader {
public:
    using adapters::ProbeReader::ProbeReader;

private:
    void observe(const ProbeEvent & event) override;
};

} // namespace markov::adapters::ktransformers
