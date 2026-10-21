#pragma once

#include "markov/contracts/collective_work.hpp"
#include "markov/contracts/cpu_service.hpp"
#include "markov/graph/source_graph.hpp"

#include <nlohmann/json_fwd.hpp>

#include <compare>
#include <cstdint>
#include <string>

namespace markov {

// Runtime-defined physical command identity, separate from host submission and
// synchronization waits. The core treats task_type as an opaque measured key.
struct DeviceCommandWork {
    std::string task_type;

    static DeviceCommandWork read(const nlohmann::json & value);
    nlohmann::json describe() const;
    auto operator<=>(const DeviceCommandWork &) const = default;
};

// Borrowed for one cost query. Dependencies and controller state stay in the
// execution structure; models receive only resolved work descriptions.
struct ServiceWork {
    SourceSpanKind node_kind;
    const SourceKernel * kernel;
    const SourceTransfer * transfer;
    const DeviceCommandWork * device_command;
    const CpuServiceWork * cpu = nullptr;
    const CollectiveWork * collective = nullptr;
    std::uint32_t collective_rank = 0;
};

} // namespace markov
