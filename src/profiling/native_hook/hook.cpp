#include "framework/framework.hpp"

// using foo_fn_t = int (*)(void * self, int a);
// HOOKFW_DEFINE_TARGET(foo, foo_fn_t, "Namespace::Class::Foo", "_ZN9Namespace5Class3FooEPvi", "/path/to/libtarget.so")

// int __attribute__((noinline, visibility("default"))) foo_hook(void * self, int a) asm("_ZN9Namespace5Class3FooEPvi");
// int foo_hook(void * self, int a) {
//     return HOOKFW_INVOKE(foo,
//                          {
//                              { 1, "a" }
//     },
//                          self,
//                          a);
// }
