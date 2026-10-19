#pragma once

#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>

namespace markov::collection {

template <typename Function> Function native_symbol(const char * name) {
    const auto symbol = dlsym(RTLD_NEXT, name);
    if (symbol == nullptr) {
        std::fprintf(stderr, "Markov native observation cannot resolve %s: %s\n", name, dlerror());
        std::abort();
    }
    return reinterpret_cast<Function>(symbol);
}

} // namespace markov::collection
