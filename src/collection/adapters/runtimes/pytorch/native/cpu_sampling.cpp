#include "cpu_sampling.hpp"

#include <ATen/record_function.h>

#include <memory>
#include <utility>

namespace {

thread_local bool operator_active = false;

class OperatorObservation : public at::ObserverContext {
public:
    explicit OperatorObservation(const at::RecordFunction & function)
        : previous_(std::exchange(operator_active, true)),
          activity_(markov::collection::CpuActivityKind::Operator, function.name()) {}
    ~OperatorObservation() override { operator_active = previous_; }

private:
    bool previous_;
    markov::collection::CpuActivity activity_;
};

std::unique_ptr<at::ObserverContext> begin(const at::RecordFunction & function) {
    if (!markov::collection::cpu_sampling_active()) return nullptr;
    // Dispatcher helpers remain part of their outer operation. Explicit native
    // ranges delimit queue/API submissions and must survive that aggregation.
    if (operator_active && function.operator_name()) return nullptr;
    return std::make_unique<OperatorObservation>(function);
}

void end(const at::RecordFunction &, at::ObserverContext *) {
    // RecordFunction owns and releases the context after the end callback.
}

} // namespace

extern "C" void markov_cpu_observe_pytorch() {
    // Install at worker initialization, before warmup and concurrent operators.
    // The callback remains installed; inactive scopes allocate no context.
    at::addGlobalCallback(at::RecordFunctionCallback(begin, end).scopes({ at::RecordScope::FUNCTION }));
}
