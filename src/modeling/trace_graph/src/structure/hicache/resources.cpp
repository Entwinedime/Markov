#include "markov/trace_graph/structure/hicache/resources.hpp"
#include "markov/trace_graph/frontend/model_config.hpp"

namespace markov::trace_graph::structure::hicache {

std::string io_resource_lane(const frontend::HiCacheIoResourceLanesConfig & lanes, const std::string & kind, const std::string & scope) {
    const auto local = kind == "prefetch"                ? "host_storage_read_lane"
                       : kind == "write_host_to_storage" ? "host_storage_write_lane"
                       : kind == "load"                  ? "host_to_device_lane"
                                                         : "device_to_host_lane";
    const bool shared = (kind == "prefetch" && lanes.shared_storage_read) || (kind == "write_host_to_storage" && lanes.shared_storage_write);
    return shared ? local : scope + "/" + local;
}

} // namespace markov::trace_graph::structure::hicache
