// KernelBox — Filesystem Module
// Manages container root filesystem: mounts, devices, pivot_root

#pragma once
#include <string>

namespace kernelbox::filesystem {

/// Ensure essential directories exist in the rootfs (bin, lib, etc, proc, sys, dev, tmp).
bool prepare_rootfs(const std::string& rootfs_path);

/// Mount /proc, /sys, /tmp, /dev, /dev/pts inside the rootfs.
bool setup_mounts(const std::string& rootfs_path);

/// Create essential device nodes (/dev/null, zero, random, urandom, tty)
/// and standard-IO symlinks. Falls back to bind-mount when mknod is denied.
bool setup_dev(const std::string& rootfs_path);

/// Switch the process root to new_root using pivot_root(2).
bool do_pivot_root(const std::string& new_root);

/// Unmount proc, sys, dev, tmp in reverse order.
bool cleanup_rootfs(const std::string& rootfs_path);

} // namespace kernelbox::filesystem
