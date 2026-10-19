#pragma once

#include "markov/contracts/control.hpp"
#include "markov/graph/source_graph.hpp"

#include <nlohmann/json_fwd.hpp>

#include <compare>
#include <cstdint>
#include <string>
#include <vector>

namespace markov {

// Runtime-defined physical command identity, separate from host submission and
// synchronization waits. The core treats task_type as an opaque measured key.
struct DeviceCommandWork {
    std::string task_type;

    static DeviceCommandWork read(const nlohmann::json & value);
    nlohmann::json describe() const;
    auto operator<=>(const DeviceCommandWork &) const = default;
};

struct PublicationMemberWork {
    RequestId request_id;
    std::uint64_t generated_tokens;
    bool emits;
    std::uint64_t output_tokens;
    std::uint64_t decode_context_tokens;
    std::uint64_t previous_output_tokens;
    std::uint64_t decode_read_offset_tokens;
    bool finishes;

    nlohmann::json describe() const;
};

struct PublicationWork {
    SourceSpanId source_span;
    std::vector<PublicationMemberWork> members;
    bool sender_enabled;

    nlohmann::json describe() const;
};

// Borrowed for one cost query. Dependencies and controller state stay in the
// execution structure; models receive only resolved work descriptions.
struct ServiceWork {
    SourceSpanKind node_kind;
    const SourceKernel * kernel;
    const SourceCommunicationTask * communication;
    const SourceTransfer * transfer;
    const DeviceCommandWork * device_command;
    const CpuServiceWork * cpu = nullptr;

    nlohmann::json describe() const;
};

} // namespace markov
