#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "markov/contracts/index_work.hpp"

namespace markov {

enum class Phase { Prefill, Decode, Mixed };

const char * phase_name(Phase phase);

struct Request {
    std::string id;
    std::int64_t arrival_ns;
    std::int64_t prompt_tokens;
    std::int64_t output_tokens;
    std::optional<std::string> prefix_id{};

    static std::vector<Request> read(const nlohmann::json & source);
};

struct ServingSettings {
    ServingSettings(std::int64_t request_capacity, std::int64_t token_capacity, std::int64_t page_tokens, std::int64_t prefill_tokens);

    std::int64_t max_running_requests;
    std::int64_t max_total_tokens;
    std::int64_t page_size;
    std::int64_t max_prefill_tokens;
};

enum class PageReuse { Immediate, DeferredSorted };

// Framework facts constrain admission; control selects requests and the resource pool owns their pages.
struct AdmissionConstraints {
    std::vector<std::size_t> occupied_members;
    std::int64_t prefill_token_granularity;
};

using GroupAdmission = std::map<std::string, AdmissionConstraints>;

// Group-local sequences remain unique when independent controllers share one executor.
struct TaskIdentity {
    std::string group;
    std::size_t sequence;

    auto operator<=>(const TaskIdentity &) const = default;
};

// Each member carries only the page changes needed for this batch.
struct BatchMember {
    std::size_t request;
    std::size_t slot;
    std::int64_t context_tokens;
    std::int64_t new_tokens;

    std::optional<std::size_t> previous_page;
    std::vector<std::size_t> new_pages;
    Phase phase;
};

// A batch is a decision snapshot; committing it remains the controller's responsibility.
struct Batch {
    Phase phase;
    std::int64_t request_capacity;
    std::int64_t free_pages_before;
    std::int64_t allocated_pages;
    std::vector<BatchMember> members;
    std::int64_t deferred_pages_before = 0;
    std::int64_t slot_capacity = 0;

    std::int64_t new_tokens() const;
};

// Control progress can complete without producing a user-visible token.
struct RequestCompletion {
    std::size_t request;
    std::size_t slot;
    std::int64_t generated_tokens;
    std::int64_t resident_tokens;
    bool finished;
    bool emits_output;
    std::vector<std::size_t> released_pages;
    bool continues_prefill = false;

    // Snapshot read by a cache operation; releasing a subset must not change this operand's contents.
    std::vector<std::size_t> mapped_pages{};
};

// Effects can shape the result graph before execution; the completion event commits them later.
struct BatchEffects {
    std::vector<RequestCompletion> members;
};

struct KvPageCopy {
    std::size_t source;
    std::size_t destination;
};

// One immutable model iteration can be shared by its stage decisions and unfinished feedback.
struct BatchWork {
    Batch batch;
    BatchEffects effects;
    std::vector<KvPageCopy> transferred_pages{};
    std::map<std::size_t, std::size_t> metadata_slots{};
    bool final_transfer = true;

    // Separate native preparations feed one model call; empty means the batch was prepared as a whole.
    std::vector<Batch> preparations{};

    // Decode's grouped index release follows native output publication inside result processing.
    std::optional<IndexWork> index_tail{};
};

enum class ServingStep {
    Batch,
    Select,
    Retain,
    Stage,
    Prepare,
    Compute,
    Send,
    Receive,
    Wait,
    ReadResult,
    Result,
    Reserve,
    Model,
    Transfer,
    Accept,
    Release,
    Indices
};

struct TransferBinding {
    std::string source;
    std::string destination;

    auto operator<=>(const TransferBinding &) const = default;
};

// Task identifies one decision; flow links continued model work or a transfer to its source send.
struct ServingAction {
    TaskIdentity task;
    std::variant<std::shared_ptr<const BatchWork>, std::shared_ptr<const IndexWork>> work;
    std::int64_t scheduled_ns;
    std::size_t flow;
    std::size_t lane;
    std::optional<std::size_t> stage;
    ServingStep step;
    std::string channel{};
    std::optional<TransferBinding> transfer{};

    const BatchWork & batch_work() const { return *std::get<std::shared_ptr<const BatchWork>>(work); }
    const IndexWork & index_work() const { return *std::get<std::shared_ptr<const IndexWork>>(work); }

    bool commits_requests() const { return step == ServingStep::Batch || step == ServingStep::Result || step == ServingStep::Accept; }
    std::string_view step_name() const;
};

// Intermediate work can complete without publishing a user-visible token.
struct BatchProgram {
    std::optional<std::size_t> publication;
    std::vector<std::size_t> completions;
    std::vector<std::size_t> returned_pages;

    // Native control consumes returned metadata; tensor values and device work remain in the graph.
    std::map<std::string, nlohmann::json> values{};
};

} // namespace markov
