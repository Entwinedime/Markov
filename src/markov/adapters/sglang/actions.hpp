#pragma once

#include <map>
#include <set>
#include <string>
#include <string_view>

#include "markov/adapters/sglang/workers.hpp"
#include "markov/controller/action_graphs.hpp"
#include "markov/templates/library.hpp"
#include "markov/templates/selection.hpp"

namespace markov::sglang {

class NativeActionGraphs final : public ActionGraphs {
public:
    NativeActionGraphs(const TemplateLibrary & library, nlohmann::json environment, std::vector<Resource> resources, std::string group, std::string instance);
    CoarseGraph build(const ServingAction & action) override;
    std::map<std::string, TensorLayout> outputs(std::string_view action, const nlohmann::json & condition, const Parameters & parameters) const override;
    std::set<std::string> retained_bindings() const override;

private:
    TemplateEntry select(std::string_view action, const nlohmann::json & condition, const Parameters & parameters) const;
    std::set<std::string> bind_values(const TemplateEntry & entry, const nlohmann::json & values, Direction direction, GraphInvocation & call) const;
    void bind_inputs(const TemplateFrame & frame, const std::map<std::string, StorageBinding> & fields, const std::set<std::string> & explicit_inputs,
                     GraphInvocation & call) const;
    void bind_buffers(const TemplateFrame & frame, GraphInvocation & call);

    const TemplateLibrary & library_;
    TemplateSelection selection_;
    nlohmann::json environment_;
    std::vector<Resource> resources_;
    WorkerBindings worker_;
    std::string group_;
    std::string instance_;
    std::map<std::size_t, std::map<std::string, StorageBinding>> batches_;
    std::map<std::string, StorageBinding> buffers_;
    std::set<std::string> signals_;
    std::set<std::string> values_;
};

} // namespace markov::sglang
