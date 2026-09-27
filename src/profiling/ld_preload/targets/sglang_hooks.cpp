/**
 * @file
 * @brief SGLang runtime hook profile。
 *
 * 当前 active LD_PRELOAD 框架按 C++ wrapper 硬编码拦截目标符号。SGLang profile
 * 先复用 AscendCL runtime 同步点；后续若要采集 SGLang / HiCache 的 native 事件，
 * 必须在这里新增明确签名、明确符号名、明确目标 so 的 wrapper，不能从 Python 配置动态生成。
 */
#include "ascendcl_hooks.cpp"

// Optional host-side transfer observation; no tensor data or snapshots.
using memcpy2d_fn = int (*)(void*, size_t, const void*, size_t, size_t, size_t, int, void*);
HOOKFW_DEFINE_TARGET(memcpy2d, memcpy2d_fn, "AscendCL@aclrtMemcpy2dAsync", "aclrtMemcpy2dAsync", TargetlibascendclSoPath)

extern "C" int aclrtMemcpy2dAsync(void* dst, size_t dpitch, const void* src, size_t spitch,
                                size_t width, size_t height, int kind, void* stream) {
    static const bool enabled = HookFrameWork::ParseEnvFlag("HOOK_MEMCPY2D_TIMING", false);
    if (!enabled) return memcpy2d_target().Original()(dst, dpitch, src, spitch, width, height, kind, stream);
    return HOOKFW_INVOKE(memcpy2d,
        {{"dpitch", std::to_string(dpitch)}, {"spitch", std::to_string(spitch)},
         {"width", std::to_string(width)}, {"height", std::to_string(height)},
         {"kind", std::to_string(kind)}, {"stream", std::to_string(reinterpret_cast<uintptr_t>(stream))}},
        dst, dpitch, src, spitch, width, height, kind, stream);
}

// Torch NPU resolves some ACL APIs through explicit library handles, bypassing
// their LD_PRELOAD wrappers. The runtime entries cover those calls as well.
// Keep the existing stream-sync event contract for DAG dependency construction.
using runtime_stream_sync_fn = int (*)(void*);
HOOKFW_DEFINE_TARGET(runtime_stream_sync, runtime_stream_sync_fn,
                     "AscendCL@aclrtSynchronizeStream", "rtStreamSynchronize", "libruntime.so")

extern "C" int rtStreamSynchronize(void* stream) {
    HookFrameWork::StreamSyncScope scope(stream);
    if (scope.Nested()) return runtime_stream_sync_target().Original()(stream);
    return HOOKFW_INVOKE(runtime_stream_sync,
        {{"stream", std::to_string(reinterpret_cast<uintptr_t>(stream))}}, stream);
}

using runtime_stream_sync_timeout_fn = int (*)(void*, int32_t);
HOOKFW_DEFINE_TARGET(runtime_stream_sync_timeout, runtime_stream_sync_timeout_fn,
                     "AscendCL@aclrtSynchronizeStreamWithTimeout", "rtStreamSynchronizeWithTimeout", "libruntime.so")

extern "C" int rtStreamSynchronizeWithTimeout(void* stream, int32_t timeout) {
    HookFrameWork::StreamSyncScope scope(stream);
    if (scope.Nested()) return runtime_stream_sync_timeout_target().Original()(stream, timeout);
    return HOOKFW_INVOKE(runtime_stream_sync_timeout,
        {{"stream", std::to_string(reinterpret_cast<uintptr_t>(stream))},
         {"timeout", std::to_string(timeout)}}, stream, timeout);
}
