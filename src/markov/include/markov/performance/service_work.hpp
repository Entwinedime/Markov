#pragma once

#include "markov/graph/source_graph.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace markov {

struct PublicationWork;
struct InvokeStep;

// Work for one complete accepted-candidate budget call. Stage totals are not
// interchangeable with these member quantities.
struct AdmissionServiceWork {
    std::size_t accepted_requests;
    std::size_t running_requests;
    std::uint32_t input_tokens;
    std::size_t horizon_checks;

    bool request_states_initialized() const;
    nlohmann::json describe() const;
};

struct CpuSubmittedWork {
    SourceSpanKind kind;
    const SourceKernel * kernel;
    const SourceCommunicationTask * communication;
    const SourceTransfer * transfer;
};

struct CpuSubmissionWork {
    const SourceCpuWork * observation;
    // Actual target roots of this submission, not the donor's device spans.
    std::vector<CpuSubmittedWork> roots;

    nlohmann::json describe() const;
};

// Borrowed for one cost query. Descriptors belong to the target instance;
// the source identity is provenance, not a measured-cost matching key.
struct ServiceWork {
    const std::filesystem::path & source_manifest;
    SourceSpanId source_span;
    std::uint32_t rank;
    SourceSpanKind node_kind;
    const InvokeStep & invocation; // Runtime-owned target context, not per-node work to charge.
    const SourceKernel * kernel;
    const SourceCommunicationTask * communication;
    const SourceTransfer * transfer;
    const PublicationWork * publication;
    std::optional<std::size_t> publication_member_slot;
    std::optional<SourceCpuPart> control_part = std::nullopt;
    std::optional<std::size_t> admission_member_slot = std::nullopt;
    const CpuSubmissionWork * cpu_submission = nullptr;

    std::optional<AdmissionServiceWork> admission_member() const;
    nlohmann::json describe() const;
};

} // namespace markov
