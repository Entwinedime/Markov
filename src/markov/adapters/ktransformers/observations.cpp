#include "markov/adapters/ktransformers/observations.hpp"

#include "markov/adapters/probes.hpp"
#include "markov/trace_graph/source/clients.hpp"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <numeric>
#include <set>
#include <stdexcept>
#include <utility>

namespace markov::adapters::ktransformers {
namespace {

using namespace trace_graph;
constexpr const char * implementation = "ktransformers.ascend.deepseek_v4.eager";

class KtReader final : public ProbeReader {
public:
    KtReader(source::Observations & output, const std::map<std::string, std::size_t> & prompts, const nlohmann::json & server)
        : ProbeReader(output),
          prompts_(prompts),
          server_(server) {}
    std::vector<Batch> batches;

private:
    const std::map<std::string, std::size_t> & prompts_;
    const nlohmann::json & server_;

    void observe(const ProbeEvent & event) override {
        if (event.name != "runtime.kt.batch") return;
        const auto & fields = event.args;
        if (fields.at("status") != "returned" || fields.at("implementation") != implementation || fields.at("request_ids").size() != 1)
            throw std::runtime_error("KT batch observation is incomplete or uses an unsupported execution mode");
        if (fields.value("timing_scope", "") != "closed_forward_response")
            throw std::runtime_error("KT source needs complete forward observations; recapture with the current semantic probe");
        const auto phase = fields.at("phase").get<std::string>();
        if (phase != "prefill" && phase != "decode") throw std::runtime_error("Unsupported KT forward phase");
        const auto request = fields.at("request_ids").front().get<std::string>();
        const auto iteration = fields.at("iteration").get<std::size_t>();
        const auto query = fields.at("query_tokens").get<std::size_t>();
        const auto prompt = prompts_.at(request);
        const auto context = fields.at("context_tokens").get<std::size_t>();
        if ((iteration == 0 && (phase != "prefill" || query != prompt)) || (iteration != 0 && (phase != "decode" || query != 1)))
            throw std::runtime_error("KT source requires one full Prefill followed by ordinary single-request Decode");
        if (context != prompt + iteration) throw std::runtime_error("KT forward context does not match the serial request");
        const auto resource = output_.resource(source::ResourceKind::CpuThread, "cpu:" + event.process + ":" + event.thread);
        const auto request_name = output_.name(request);
        Batch batch{ request, iteration, event.begin_ns, event.end_ns, {}, 0 };
        std::set<std::size_t> layers;
        for (const auto & layer : fields.at("layers")) {
            const auto layer_id = layer.at("layer").get<std::size_t>();
            if (!layers.insert(layer_id).second) throw std::runtime_error("KT layer appears more than once in the batch");
            const auto & moe = layer.at("moe");
            auto counts = moe.at("expert_counts").get<std::vector<std::size_t>>();
            if (counts.size() != moe.at("experts").get<std::size_t>()
                || std::accumulate(counts.begin(), counts.end(), std::size_t{}) != query * moe.at("top_k").get<std::size_t>())
                throw std::runtime_error("KT expert histogram does not describe the observed query");
            if (moe.at("resident_experts") != server_.at("kt_num_gpu_experts") || moe.at("cpu_threads") != server_.at("kt_cpuinfer")
                || moe.at("numa_pools") != server_.at("kt_threadpool_count"))
                throw std::runtime_error("KT observed resources disagree with the captured native invocation");
            batch.layers.push_back({ layer_id, std::move(counts) });
        }
        if (batch.layers.empty()) throw std::runtime_error("KT forward has no observed routing");
        batch.source_cost = output_.cost_observations.size();
        output_.cost_observations.push_back({
            request_name,
            iteration,
            0,
            "forward",
            forward_work(phase, batch.layers, query, context, server_.at("kt_num_gpu_experts"), server_.at("kt_cpuinfer"), server_.at("kt_threadpool_count")),
            event.end_ns - event.begin_ns,
            { resource, event.begin_ns, event.end_ns, event.record }
        });
        batches.push_back(std::move(batch));
    }
};

} // namespace

planning::Work forward_work(const std::string & phase, const std::vector<LayerRouting> & layers, std::size_t query_tokens, std::size_t context_tokens,
                            std::size_t resident, std::size_t threads, std::size_t pools) {
    if (!query_tokens || context_tokens < query_tokens || !threads || !pools || !resident || layers.empty())
        throw std::runtime_error("KT forward work requires query, context, routing and positive CPU resources");
    std::size_t npu_pairs = 0, cpu_pairs = 0, npu_active = 0, cpu_active = 0;
    for (const auto & layer : layers) {
        const auto & counts = layer.expert_counts;
        if (resident >= counts.size()) throw std::runtime_error("KT forward needs a nonempty CPU expert partition");
        npu_pairs += std::accumulate(counts.begin(), counts.begin() + resident, std::size_t{});
        cpu_pairs += std::accumulate(counts.begin() + resident, counts.end(), std::size_t{});
        npu_active += std::count_if(counts.begin(), counts.begin() + resident, [](auto count) { return count != 0; });
        cpu_active += std::count_if(counts.begin() + resident, counts.end(), [](auto count) { return count != 0; });
    }
    return {
        { "forward." + phase, implementation, "closed_forward_response" },
        { { "query_tokens", static_cast<double>(query_tokens) },
         { "context_tokens", static_cast<double>(context_tokens) },
         { "attention_pairs", static_cast<double>(query_tokens) * (2.0 * context_tokens - query_tokens + 1) / 2 },
         { "layers", static_cast<double>(layers.size()) },
         { "cpu_token_expert_pairs", static_cast<double>(cpu_pairs) },
         { "npu_token_expert_pairs", static_cast<double>(npu_pairs) },
         { "active_cpu_experts", static_cast<double>(cpu_active) },
         { "active_npu_experts", static_cast<double>(npu_active) },
         { "cpu_threads", static_cast<double>(threads) },
         { "numa_pools", static_cast<double>(pools) },
         { "cpu_pairs_per_thread", static_cast<double>(cpu_pairs) / threads },
         { "active_cpu_experts_per_pool", static_cast<double>(cpu_active) / pools },
         { "resident_experts", static_cast<double>(resident) } }
    };
}

KtSource read_source(source::ProfileManifest manifest, const nlohmann::json & captured) {
    const auto begin = std::chrono::steady_clock::now();
    source::Observations observed;
    std::map<std::string, std::size_t> prompts;
    for (std::size_t id = 0; id < captured.at("workload").at("requests").size(); ++id)
        prompts.emplace("request_" + std::to_string(id), captured.at("workload").at("requests")[id].at("input_ids").size());
    KtReader reader(observed, prompts, captured.at("server").at("arguments"));
    std::vector<std::size_t> counts;
    std::vector<std::size_t> skipped;
    for (std::size_t file_id = 0; file_id < manifest.files.size(); ++file_id) {
        const auto & file = manifest.files[file_id];
        if (file.channel == source::Channel::PythonProbe) {
            reader.set_file(file_id);
            counts.push_back(source::read_json_lines(file.path, reader));
        }
        else if (file.channel == source::Channel::Client) {
            auto client = source::read_client_program(file.path, file_id);
            if (!client.barriers.empty() || !observed.requests.empty())
                throw std::runtime_error("KT requires one serial client report without HiCache barriers");
            observed.requests = std::move(client.requests);
            counts.push_back(client.record_count);
        }
        else {
            skipped.push_back(file_id);
            counts.push_back(0);
        }
    }
    for (auto & request : observed.requests) {
        const auto & ports = reader.requests.at(request.identity);
        if (ports.received.size() != 1 || ports.sent.size() != 1) throw std::runtime_error("KT request transport is ambiguous or incomplete");
        request.received = ports.received.front();
        request.sent = ports.sent.front();
        if (!(request.begin_ns <= observed.points[*request.received].time_ns
              && observed.points[*request.received].time_ns <= observed.points[*request.sent].time_ns
              && observed.points[*request.sent].time_ns <= request.end_ns))
            throw std::runtime_error("KT request transport ports lie outside the HTTP window");
    }
    std::ranges::sort(reader.batches, {}, &Batch::begin_ns);
    std::vector<std::size_t> tails;
    std::size_t batch_index = 0;
    for (std::size_t request_index = 0; request_index < observed.requests.size(); ++request_index) {
        const auto & request = observed.requests[request_index];
        auto cursor = observed.points[*request.received].time_ns;
        std::size_t iteration = 0;
        const auto add_frontend = [&](source::Time end, const Batch & batch) {
            if (end < cursor) throw std::runtime_error("KT batches overlap or escape their request transport");
            const auto & original = observed.cost_observations[batch.source_cost];
            const auto id = observed.cost_observations.size();
            observed.cost_observations.push_back({
                observed.name(request.identity),
                iteration,
                0,
                "frontend",
                { { "frontend", implementation, "source_residual" }, {} },
                end - cursor,
                { original.region.resource, cursor, end, original.region.record },
                std::nullopt,
                true,
                true
            });
            return id;
        };
        const Batch * last = nullptr;
        while (batch_index < reader.batches.size() && reader.batches[batch_index].request == request.identity) {
            auto & batch = reader.batches[batch_index++];
            if (batch.iteration != iteration++) throw std::runtime_error("KT forward iterations are missing or duplicated");
            batch.frontend_cost = add_frontend(batch.begin_ns, batch);
            cursor = batch.end_ns;
            last = &batch;
        }
        if (!last) throw std::runtime_error("KT request has no observed forward");
        if (iteration != captured.at("workload").at("requests")[request_index].at("max_new_tokens").get<std::size_t>())
            throw std::runtime_error("KT forward counts do not match the captured fixed output count");
        tails.push_back(add_frontend(observed.points[*request.sent].time_ns, *last));
    }
    if (batch_index != reader.batches.size()) throw std::runtime_error("KT source has forwards outside its serial client program");
    observed.normalize_resources();
    observed.resolve_flows();
    source::SourceContext context(std::move(observed));
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    return {
        { std::move(manifest), std::move(context), std::move(counts), std::move(reader.roles), elapsed, 0, std::nullopt, std::move(skipped) },
        std::move(reader.batches),
        std::move(tails)
    };
}

} // namespace markov::adapters::ktransformers
