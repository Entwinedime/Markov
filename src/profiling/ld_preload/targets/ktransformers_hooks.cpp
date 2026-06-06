// ktransformers runtime hook profile.
//
// Keep this as a separate translation unit even while it reuses the common
// AscendCL wrappers. ktransformers/kt-kernel specific symbols can be added here
// without changing the SGLang hook library.
#include "ascendcl_hooks.cpp"
