#pragma once

#include <string>
#include <string_view>

namespace markov::ascend_cpu {

inline constexpr char kernel_launch[] = "runtime_kernel_launch";
inline constexpr char handle_launch[] = "rtKernelLaunchWithHandle";
inline constexpr char ffts_launch[] = "rtGeneralCtrl";
inline constexpr char driver_copy[] = "drvMemcpy";

// CANN reports implementation ranges; lightweight observation surrounds the
// installed public entry. Keep that association in this runtime adapter.
inline std::string runtime_entry(std::string_view trace_name) {
    auto name = trace_name.substr(std::string_view("Runtime@").size());
    if (name == "LaunchKernelV2") return kernel_launch;
    if (name == "FftsPlusTaskLaunch") return ffts_launch;
    if (name.ends_with("_Big")) name.remove_suffix(4);
    return "rt" + std::string(name);
}

} // namespace markov::ascend_cpu
