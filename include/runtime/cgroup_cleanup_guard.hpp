#pragma once

#include "../cgroups/cgroup_manager.hpp"
#include "../common.hpp"

namespace kernelbox::runtime {

class CgroupCleanupGuard {
public:
    CgroupCleanupGuard(cgroups::CgroupManager& manager, std::string container_id)
        : manager_(manager), container_id_(std::move(container_id)) {}

    CgroupCleanupGuard(const CgroupCleanupGuard&) = delete;
    CgroupCleanupGuard& operator=(const CgroupCleanupGuard&) = delete;

    ~CgroupCleanupGuard() {
        if (active_ && !manager_.remove(container_id_)) {
            log_error("Failed to clean up cgroup for " + container_id_);
        }
    }

    void release() { active_ = false; }

private:
    cgroups::CgroupManager& manager_;
    std::string container_id_;
    bool active_ = true;
};

} // namespace kernelbox::runtime
