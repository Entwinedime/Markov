#pragma once

#include "framework/hook_target.hpp"
#include "framework/invoke.hpp"

#define HOOKFW_PP_CAT_IMPL(a, b) a##b
#define HOOKFW_PP_CAT(a, b) HOOKFW_PP_CAT_IMPL(a, b)

#define HOOKFW_DEFINE_TARGET(TAG, FN_TYPE, TRACE_NAME, MANGLED_NAME, SO_PATH)                                                                                  \
    static ::HookFrameWork::HookTarget<FN_TYPE> & TAG##_target() {                                                                                             \
        static ::HookFrameWork::HookTarget<FN_TYPE> target(TRACE_NAME, MANGLED_NAME, SO_PATH);                                                                 \
        return target;                                                                                                                                         \
    }

#define HOOKFW_INVOKE(TAG, ...) ::HookFrameWork::InvokeHook(TAG##_target(), __VA_ARGS__)

#define HOOKFW_SET_RULE(CALLEE, ...)                                                                                                                           \
    static const bool HOOKFW_PP_CAT(hookfw_rule_, __LINE__) = []() {                                                                                           \
        ::HookFrameWork::RelationRules::Get().UpsertRule((CALLEE), { __VA_ARGS__ });                                                                           \
        return true;                                                                                                                                           \
    }();
