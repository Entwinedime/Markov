#include "framework/framework.hpp"
#include <cstdlib>
#include <memory>
#include <string>

/**
 * @file
 * @brief AscendCL runtime 同步与 event wrapper。
 *
 * 这些 wrapper 只记录 runtime 参数和调用耗时，用于补齐 torch trace 中可能缺失的 sync 边界。
 * 采集层不推导 target policy，也不生成 HiCache target 行为。
 */

/** @brief 允许通过 HOOK_ASCENDCL_SO_PATH 指向非默认 AscendCL 安装路径。 */
static std::string ResolveAscendClSoPath() {
    const char * env_path = std::getenv("HOOK_ASCENDCL_SO_PATH");
    if (env_path != nullptr && env_path[0] != '\0') { return env_path; }
    return "/usr/local/Ascend/ascend-toolkit/latest/acllib/lib64/libascendcl.so";
}

static const std::string TargetlibascendclSoPath{ResolveAscendClSoPath()};

/**
 * @name AscendCL 同步与 event wrapper
 *
 * wrapper 参数统一转换成字符串写入 Function-Args；真实函数返回值和副作用必须保持不变。
 * @{
 */

static const std::string MangledAclrtSynchronizeStream{"aclrtSynchronizeStream"};
using aclrt_synchronize_stream_fn_t = int (*)(void * stream);
HOOKFW_DEFINE_TARGET(aclrt_synchronize_stream, aclrt_synchronize_stream_fn_t, "AscendCL@aclrtSynchronizeStream", MangledAclrtSynchronizeStream,
                     TargetlibascendclSoPath)

int __attribute__((noinline, visibility("default"))) aclrt_synchronize_stream_hook(void * stream) asm("aclrtSynchronizeStream");
int aclrt_synchronize_stream_hook(void * stream) {
    return HOOKFW_INVOKE(aclrt_synchronize_stream, {{"stream", std::to_string(reinterpret_cast<uintptr_t>(stream))}}, stream);
}

static const std::string MangledAclrtSynchronizeStreamWithTimeout{"aclrtSynchronizeStreamWithTimeout"};
using aclrt_synchronize_stream_with_timeout_fn_t = int (*)(void * stream, int32_t timeout);
HOOKFW_DEFINE_TARGET(aclrt_synchronize_stream_with_timeout, aclrt_synchronize_stream_with_timeout_fn_t, "AscendCL@aclrtSynchronizeStreamWithTimeout",
                     MangledAclrtSynchronizeStreamWithTimeout, TargetlibascendclSoPath)

int __attribute__((noinline, visibility("default")))
aclrt_synchronize_stream_with_timeout_hook(void * stream, int32_t timeout) asm("aclrtSynchronizeStreamWithTimeout");
int aclrt_synchronize_stream_with_timeout_hook(void * stream, int32_t timeout) {
    return HOOKFW_INVOKE(aclrt_synchronize_stream_with_timeout,
                         {{"stream", std::to_string(reinterpret_cast<uintptr_t>(stream))}, {"timeout", std::to_string(timeout)}},
                         stream,
                         timeout);
}

static const std::string MangledAclrtSynchronizeEvent{"aclrtSynchronizeEvent"};
using aclrt_synchronize_event_fn_t = int (*)(void * event);
HOOKFW_DEFINE_TARGET(aclrt_synchronize_event, aclrt_synchronize_event_fn_t, "AscendCL@aclrtSynchronizeEvent", MangledAclrtSynchronizeEvent,
                     TargetlibascendclSoPath)

int __attribute__((noinline, visibility("default"))) aclrt_synchronize_event_hook(void * event) asm("aclrtSynchronizeEvent");
int aclrt_synchronize_event_hook(void * event) {
    return HOOKFW_INVOKE(aclrt_synchronize_event, {{"event", std::to_string(reinterpret_cast<uintptr_t>(event))}}, event);
}

static const std::string MangledAclrtSynchronizeEventWithTimeout{"aclrtSynchronizeEventWithTimeout"};
using aclrt_synchronize_event_with_timeout_fn_t = int (*)(void * event, int32_t timeout);
HOOKFW_DEFINE_TARGET(aclrt_synchronize_event_with_timeout, aclrt_synchronize_event_with_timeout_fn_t, "AscendCL@aclrtSynchronizeEventWithTimeout",
                     MangledAclrtSynchronizeEventWithTimeout, TargetlibascendclSoPath)

int __attribute__((noinline, visibility("default")))
aclrt_synchronize_event_with_timeout_hook(void * event, int32_t timeout) asm("aclrtSynchronizeEventWithTimeout");
int aclrt_synchronize_event_with_timeout_hook(void * event, int32_t timeout) {
    return HOOKFW_INVOKE(aclrt_synchronize_event_with_timeout,
                         {{"event", std::to_string(reinterpret_cast<uintptr_t>(event))}, {"timeout", std::to_string(timeout)}},
                         event,
                         timeout);
}

static const std::string MangledAclrtSynchronizeDevice{"aclrtSynchronizeDevice"};
using aclrt_synchronize_device_fn_t = int (*)();
HOOKFW_DEFINE_TARGET(aclrt_synchronize_device, aclrt_synchronize_device_fn_t, "AscendCL@aclrtSynchronizeDevice", MangledAclrtSynchronizeDevice,
                     TargetlibascendclSoPath)

int __attribute__((noinline, visibility("default"))) aclrt_synchronize_device_hook() asm("aclrtSynchronizeDevice");
int aclrt_synchronize_device_hook() { return HOOKFW_INVOKE(aclrt_synchronize_device, {}); }

static const std::string MangledAclrtSynchronizeDeviceWithTimeout{"aclrtSynchronizeDeviceWithTimeout"};
using aclrt_synchronize_device_with_timeout_fn_t = int (*)(int32_t timeout);
HOOKFW_DEFINE_TARGET(aclrt_synchronize_device_with_timeout, aclrt_synchronize_device_with_timeout_fn_t, "AscendCL@aclrtSynchronizeDeviceWithTimeout",
                     MangledAclrtSynchronizeDeviceWithTimeout, TargetlibascendclSoPath)

int __attribute__((noinline, visibility("default"))) aclrt_synchronize_device_with_timeout_hook(int32_t timeout) asm("aclrtSynchronizeDeviceWithTimeout");
int aclrt_synchronize_device_with_timeout_hook(int32_t timeout) {
    return HOOKFW_INVOKE(aclrt_synchronize_device_with_timeout, {{"timeout", std::to_string(timeout)}}, timeout);
}

static const std::string MangledAclrtRecordEvent{"aclrtRecordEvent"};
using aclrt_record_event_fn_t = int (*)(void * event, void * stream);
HOOKFW_DEFINE_TARGET(aclrt_record_event, aclrt_record_event_fn_t, "AscendCL@aclrtRecordEvent", MangledAclrtRecordEvent, TargetlibascendclSoPath)

int __attribute__((noinline, visibility("default"))) aclrt_record_event_hook(void * event, void * stream) asm("aclrtRecordEvent");
int aclrt_record_event_hook(void * event, void * stream) {
    return HOOKFW_INVOKE(aclrt_record_event,
                         {{"stream", std::to_string(reinterpret_cast<uintptr_t>(stream))}, {"event", std::to_string(reinterpret_cast<uintptr_t>(event))}},
                         event,
                         stream);
}

static const std::string MangledAclrtStreamWaitEvent{"aclrtStreamWaitEvent"};
using aclrt_stream_wait_event_fn_t = int (*)(void * stream, void * event);
HOOKFW_DEFINE_TARGET(aclrt_stream_wait_event, aclrt_stream_wait_event_fn_t, "AscendCL@aclrtStreamWaitEvent", MangledAclrtStreamWaitEvent,
                     TargetlibascendclSoPath)

int __attribute__((noinline, visibility("default"))) aclrt_stream_wait_event_hook(void * stream, void * event) asm("aclrtStreamWaitEvent");
int aclrt_stream_wait_event_hook(void * stream, void * event) {
    return HOOKFW_INVOKE(aclrt_stream_wait_event,
                         {{"stream", std::to_string(reinterpret_cast<uintptr_t>(stream))}, {"event", std::to_string(reinterpret_cast<uintptr_t>(event))}},
                         stream,
                         event);
}

/** @} */
