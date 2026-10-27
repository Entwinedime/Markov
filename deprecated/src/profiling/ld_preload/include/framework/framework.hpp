#pragma once

/**
 * @file
 * @brief LD_PRELOAD hook framework 聚合头文件。
 *
 * target hook 源文件只需要包含该头，即可获得符号解析、调用包装、trace 输出、
 * PMU 采集和 relation rule 支持。
 */

#include "framework/common.hpp"
#include "framework/hook_target.hpp"
#include "framework/invoke.hpp"
#include "framework/macros.hpp"
#include "framework/pmu_recorder.hpp"
#include "framework/relation_rules.hpp"
#include "framework/scope_registry.hpp"
#include "framework/scoped_timer.hpp"
#include "framework/symbol_resolver.hpp"
#include "framework/trace_logger.hpp"
