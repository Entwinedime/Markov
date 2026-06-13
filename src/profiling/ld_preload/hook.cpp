#include "framework/framework.hpp"

/**
 * @file
 * @brief LD_PRELOAD 框架聚合入口。
 *
 * 新增 wrapper 时应使用明确函数签名、明确目标符号名和明确目标 so。示例：
 *
 * @code
 * using foo_fn_t = int (*)(void * self, int a);
 * HOOKFW_DEFINE_TARGET(foo, foo_fn_t, "Namespace::Class::Foo", "_ZN9Namespace5Class3FooEPvi", "/path/to/libtarget.so")
 *
 * int __attribute__((noinline, visibility("default"))) foo_hook(void * self, int a) asm("_ZN9Namespace5Class3FooEPvi");
 * int foo_hook(void * self, int a) {
 *     return HOOKFW_INVOKE(foo, {{"a", std::to_string(a)}}, self, a);
 * }
 * @endcode
 */
