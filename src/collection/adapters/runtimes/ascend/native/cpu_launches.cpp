#include "cpu_sampling.hpp"
#include "native_symbols.hpp"

#include "markov/adapters/runtimes/ascend/cpu_identity.hpp"

#include <runtime/kernel.h>
#include <runtime/rts/rts_kernel.h>

using markov::collection::CpuActivity;
using markov::collection::CpuActivityKind;
using markov::collection::native_symbol;

// Preserve the SDK ABI and native execution. These entry probes do not collect
// task descriptors, synchronize, or maintain request/worker state.
extern "C" rtError_t rtKernelLaunch(const void * stub, std::uint32_t blocks, void * args, std::uint32_t bytes, rtSmDesc_t * description, rtStream_t stream) {
    static const auto original = native_symbol<decltype(&rtKernelLaunch)>("rtKernelLaunch");
    const CpuActivity cpu(CpuActivityKind::Submission, "rtKernelLaunch");
    return original(stub, blocks, args, bytes, description, stream);
}

extern "C" rtError_t rtKernelLaunchWithHandle(void * handle, const std::uint64_t key, std::uint32_t blocks, rtArgsEx_t * args, rtSmDesc_t * description,
                                              rtStream_t stream, const void * kernel) {
    static const auto original = native_symbol<decltype(&rtKernelLaunchWithHandle)>("rtKernelLaunchWithHandle");
    const CpuActivity cpu(CpuActivityKind::Submission, markov::ascend_cpu::handle_launch);
    return original(handle, key, blocks, args, description, stream, kernel);
}

extern "C" rtError_t rtKernelLaunchWithHandleV2(void * handle, const std::uint64_t key, std::uint32_t blocks, rtArgsEx_t * args, rtSmDesc_t * description,
                                                rtStream_t stream, const rtTaskCfgInfo_t * configuration) {
    static const auto original = native_symbol<decltype(&rtKernelLaunchWithHandleV2)>("rtKernelLaunchWithHandleV2");
    const CpuActivity cpu(CpuActivityKind::Submission, markov::ascend_cpu::handle_launch);
    return original(handle, key, blocks, args, description, stream, configuration);
}

extern "C" rtError_t rtsLaunchKernelWithConfig(rtFuncHandle function, std::uint32_t blocks, rtStream_t stream, rtKernelLaunchCfg_t * configuration,
                                               rtArgsHandle args, void * reserve) {
    static const auto original = native_symbol<decltype(&rtsLaunchKernelWithConfig)>("rtsLaunchKernelWithConfig");
    const CpuActivity cpu(CpuActivityKind::Submission, markov::ascend_cpu::kernel_launch);
    return original(function, blocks, stream, configuration, args, reserve);
}

extern "C" rtError_t rtsLaunchKernelWithDevArgs(rtFuncHandle function, std::uint32_t blocks, rtStream_t stream, rtKernelLaunchCfg_t * configuration,
                                                const void * args, std::uint32_t bytes, void * reserve) {
    static const auto original = native_symbol<decltype(&rtsLaunchKernelWithDevArgs)>("rtsLaunchKernelWithDevArgs");
    const CpuActivity cpu(CpuActivityKind::Submission, markov::ascend_cpu::kernel_launch);
    return original(function, blocks, stream, configuration, args, bytes, reserve);
}

extern "C" rtError_t rtsLaunchKernelWithHostArgs(rtFuncHandle function, std::uint32_t blocks, rtStream_t stream, rtKernelLaunchCfg_t * configuration,
                                                 void * args, std::uint32_t bytes, rtPlaceHolderInfo_t * placeholders, std::uint32_t count) {
    static const auto original = native_symbol<decltype(&rtsLaunchKernelWithHostArgs)>("rtsLaunchKernelWithHostArgs");
    const CpuActivity cpu(CpuActivityKind::Submission, markov::ascend_cpu::kernel_launch);
    return original(function, blocks, stream, configuration, args, bytes, placeholders, count);
}
