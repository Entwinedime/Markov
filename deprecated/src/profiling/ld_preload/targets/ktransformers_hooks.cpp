/**
 * @file
 * @brief KTransformers runtime hook profile。
 *
 * 当前复用通用 AscendCL wrappers，但保留独立 translation unit。后续新增
 * ktransformers / kt-kernel 专项符号时，应在这里声明明确签名、符号名和目标 so，
 * 避免影响 SGLang hook library。
 */
#include "ascendcl_hooks.cpp"
