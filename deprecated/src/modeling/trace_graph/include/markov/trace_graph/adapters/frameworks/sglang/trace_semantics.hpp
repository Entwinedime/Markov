#pragma once

#include "markov/trace_graph/core/trace_event.hpp"

namespace markov::trace_graph::adapters::sglang {

/** Interpret recorded SGLang probe/control roles for source construction.
 * This is observation routing, not target policy or executable work. */
void classify_event(core::TraceEvent & event);

} // namespace markov::trace_graph::adapters::sglang
