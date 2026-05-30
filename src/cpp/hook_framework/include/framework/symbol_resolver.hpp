#pragma once

#include <string>

namespace HookFrameWork {

void * ResolveSymbol(const std::string & mangled_name, const std::string & so_path);

} // namespace HookFrameWork
