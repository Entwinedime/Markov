#pragma once

#include "framework/hook_target.hpp"
#include "framework/invoke.hpp"

/**
 * @file
 * @brief LD_PRELOAD wrapper 声明宏。
 *
 * 宏只负责减少样板代码；具体目标符号、函数签名和 trace 参数仍必须在 C++ wrapper 中显式写出。
 */

/** @brief 预处理 token 拼接的两段式展开实现。 */
#define HOOKFW_PP_CAT_IMPL(a, b) a##b
#define HOOKFW_PP_CAT(a, b) HOOKFW_PP_CAT_IMPL(a, b)

/** @brief 定义一个惰性初始化的 HookTarget 单例访问器。 */
#define HOOKFW_DEFINE_TARGET(TAG, FN_TYPE, TRACE_NAME, MANGLED_NAME, SO_PATH)                                                                                  \
    static ::HookFrameWork::HookTarget<FN_TYPE> & TAG##_target() {                                                                                             \
        static ::HookFrameWork::HookTarget<FN_TYPE> target(TRACE_NAME, MANGLED_NAME, SO_PATH);                                                                 \
        return target;                                                                                                                                         \
    }

/** @brief 通过统一 InvokeHook 路径调用真实函数并按规则写 trace。 */
#define HOOKFW_INVOKE(TAG, ...) ::HookFrameWork::InvokeHook(TAG##_target(), __VA_ARGS__)

/** @brief 在静态初始化阶段注册或覆盖 callee 的 caller 过滤规则。 */
#define HOOKFW_SET_RULE(CALLEE, ...)                                                                                                                           \
    static const bool HOOKFW_PP_CAT(hookfw_rule_, __LINE__) = []() {                                                                                           \
        ::HookFrameWork::RelationRules::Get().UpsertRule((CALLEE), { __VA_ARGS__ });                                                                           \
        return true;                                                                                                                                           \
    }();
