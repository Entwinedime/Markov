#include "markov/adapters/sglang/kernel_preparation.hpp"

#include <utility>

namespace markov::adapters::sglang {

trace_graph::planning::KernelPreparation triton_preparation(const std::string & family, trace_graph::planning::Features specialization,
                                                            trace_graph::planning::Features launcher) {
    const auto implementation = "triton.ascend." + family;
    return {
        { { "runtime.triton.specialization", implementation, "kernel_specialization" },            specialization },
        {     { "runtime.triton.compile", implementation, "caller_blocking_interval" },            specialization },
        {  { "runtime.triton.first_load", implementation, "caller_blocking_interval" },       std::move(launcher) },
        {        { "runtime.triton.load", implementation, "caller_blocking_interval" }, std::move(specialization) }
    };
}

} // namespace markov::adapters::sglang
