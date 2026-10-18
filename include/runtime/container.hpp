// KernelBox — Container Runtime
// Orchestrates container lifecycle: create, start, stop, remove, list, logs

#pragma once
#include <string>
#include <vector>
#include <optional>
#include <utility>
#include "../common.hpp"

namespace kernelbox::runtime {

class ContainerRuntime {
public:
    /// Create a container from an image. Returns the container ID.
    std::string create(const std::string& image_tag,
                       const std::string& name,
                       const std::vector<std::pair<std::string,std::string>>& port_mappings,
                       const std::string& cpu_limit    = "",
                       const std::string& memory_limit = "",
                       bool detach = false);

    /// Start a created or stopped container.
    bool start(const std::string& id_or_name);

    /// Stop a running container (SIGTERM then SIGKILL after timeout).
    bool stop(const std::string& id_or_name, int timeout_seconds = 10);

    /// Remove a container (stop first if running, then delete on disk).
    bool remove(const std::string& id_or_name);

    /// List all containers, auto-correcting stale RUNNING states.
    std::vector<ContainerState> list();

    /// Lookup a single container.
    std::optional<ContainerState> get(const std::string& id_or_name);

    /// Read container logs.
    std::string logs(const std::string& id_or_name);

private:
    std::string containers_dir_ = CONTAINERS_DIR;

    std::string    resolve_id(const std::string& id_or_name);
    ContainerState load_state(const std::string& id);
    bool           save_state(const ContainerState& state);
};

} // namespace kernelbox::runtime
