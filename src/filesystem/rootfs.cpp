// KernelBox — Filesystem Module Implementation
// Container root filesystem preparation, mounts, device nodes, pivot_root

#include "../../include/filesystem/rootfs.hpp"
#include "../../include/common.hpp"

#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace kernelbox::filesystem {

// ── prepare_rootfs ───────────────────────────────────────────────────────────

bool prepare_rootfs(const std::string& rootfs_path) {
    // Essential directories that every Linux root filesystem needs
    std::vector<std::string> dirs = {
        "bin", "sbin", "lib", "lib64", "usr", "usr/bin", "usr/lib",
        "etc", "proc", "sys", "dev", "dev/pts", "tmp", "run", "var"
    };

    for (const auto& dir : dirs) {
        fs::path p = fs::path(rootfs_path) / dir;
        std::error_code ec;
        if (!fs::exists(p, ec)) {
            if (ec || !fs::create_directories(p, ec) || ec) {
                log_error("Failed to create " + p.string() + ": " + ec.message());
                return false;
            }
        }
        if (ec || !fs::is_directory(p, ec) || ec) {
            log_error("Required rootfs path is not a directory: " + p.string());
            return false;
        }
    }

    // /tmp needs the sticky bit
    std::error_code ec;
    fs::permissions(fs::path(rootfs_path) / "tmp",
                    fs::perms::all | fs::perms::sticky_bit,
                    fs::perm_options::replace, ec);
    if (ec) {
        log_error("Failed to set /tmp permissions: " + ec.message());
        return false;
    }

    log_info("Rootfs prepared at " + rootfs_path);
    return true;
}

// ── setup_mounts ─────────────────────────────────────────────────────────────

bool setup_mounts(const std::string& rootfs_path) {
    // Prevent mounts from leaking into the host mount namespace
    if (mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) {
        log_error("mount / private: " + std::string(strerror(errno)));
        return false;
    }

    // Helper: mount a single filesystem
    auto do_mount = [&](const char* src, const std::string& target,
                        const char* type, unsigned long flags,
                        const char* data) -> bool {
        fs::path p = fs::path(rootfs_path) / target;
        std::error_code ec;
        fs::create_directories(p, ec);
        if (ec || !fs::is_directory(p, ec) || ec) {
            log_error("Prepare mount target " + target + ": " + ec.message());
            return false;
        }
        if (mount(src, p.c_str(), type, flags, data) != 0) {
            log_error("mount " + target + ": " + strerror(errno));
            return false;
        }
        return true;
    };

    // The rootfs itself must be a mount point for pivot_root to work.
    // Bind-mount it onto itself.
    if (mount(rootfs_path.c_str(), rootfs_path.c_str(), nullptr, MS_BIND | MS_REC, nullptr) != 0) {
        log_error("bind-mount rootfs: " + std::string(strerror(errno)));
        return false;
    }

    if (!do_mount("proc",   "proc", "proc",   MS_NOSUID | MS_NODEV | MS_NOEXEC, nullptr)) return false;
    if (!do_mount("sysfs",  "sys",  "sysfs",  MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_RDONLY, nullptr)) return false;
    if (!do_mount("tmpfs",  "tmp",  "tmpfs",  MS_NOSUID | MS_NODEV, "size=64m"))           return false;
    if (!do_mount("tmpfs",  "dev",  "tmpfs",  MS_NOSUID | MS_STRICTATIME, "mode=755"))     return false;
    if (!do_mount("devpts", "dev/pts", "devpts", MS_NOSUID | MS_NOEXEC, "newinstance,ptmxmode=0666")) return false;

    // Mask sensitive kernel paths inside container procfs
    auto mask_file = [&](const std::string& rel) {
        fs::path p = fs::path(rootfs_path) / rel;
        std::error_code ec;
        if (fs::exists(p, ec)) {
            mount("/dev/null", p.c_str(), nullptr, MS_BIND, nullptr);
            mount(nullptr, p.c_str(), nullptr, MS_BIND | MS_REMOUNT | MS_RDONLY, nullptr);
        }
    };
    mask_file("proc/sysrq-trigger");
    mask_file("proc/kcore");

    log_info("Mounts set up in rootfs");
    return true;
}

// ── setup_dev ────────────────────────────────────────────────────────────────

bool setup_dev(const std::string& rootfs_path) {
    // Try mknod; fall back to bind-mount from host if permission is denied.
    auto mknod_or_bind = [&](const std::string& rel, mode_t mode, dev_t dev,
                             const std::string& host) -> bool {
        fs::path p = fs::path(rootfs_path) / rel;
        if (mknod(p.c_str(), mode, dev) == 0) return true;
        if (errno == EPERM) {
            // Create an empty file, then bind-mount the real device over it
            int fd = open(p.c_str(), O_CREAT | O_RDWR, 0666);
            if (fd >= 0) close(fd);
            if (mount(host.c_str(), p.c_str(), "", MS_BIND, nullptr) != 0) {
                log_error("bind " + host + " → " + p.string() + ": " + strerror(errno));
                return false;
            }
            return true;
        }
        log_error("mknod " + p.string() + ": " + strerror(errno));
        return false;
    };

    if (!mknod_or_bind("dev/null",    S_IFCHR | 0666, makedev(1, 3), "/dev/null"))    return false;
    if (!mknod_or_bind("dev/zero",    S_IFCHR | 0666, makedev(1, 5), "/dev/zero"))    return false;
    if (!mknod_or_bind("dev/random",  S_IFCHR | 0666, makedev(1, 8), "/dev/random"))  return false;
    if (!mknod_or_bind("dev/urandom", S_IFCHR | 0666, makedev(1, 9), "/dev/urandom")) return false;
    if (!mknod_or_bind("dev/tty",     S_IFCHR | 0666, makedev(5, 0), "/dev/tty"))     return false;

    // Standard I/O symlinks
    auto make_link = [&](const char* target, const char* link_rel) -> bool {
        fs::path lp = fs::path(rootfs_path) / link_rel;
        if (symlink(target, lp.c_str()) != 0 && errno != EEXIST) {
            log_error("symlink " + lp.string() + ": " + strerror(errno));
            return false;
        }
        return true;
    };

    if (!make_link("/proc/self/fd/0", "dev/stdin"))  return false;
    if (!make_link("/proc/self/fd/1", "dev/stdout")) return false;
    if (!make_link("/proc/self/fd/2", "dev/stderr")) return false;
    if (!make_link("/proc/self/fd",   "dev/fd"))     return false;

    log_info("Device nodes created");
    return true;
}

// ── do_pivot_root ────────────────────────────────────────────────────────────

bool do_pivot_root(const std::string& new_root) {
    // pivot_root requires a put_old directory inside new_root
    fs::path put_old = fs::path(new_root) / "put_old";
    std::error_code ec;
    if (!fs::exists(put_old, ec)) {
        if (ec || !fs::create_directories(put_old, ec) || ec) {
            log_error("create put_old: " + ec.message());
            return false;
        }
    }
    if (ec || fs::is_symlink(fs::symlink_status(put_old, ec)) || ec ||
        !fs::is_directory(put_old, ec) || ec) {
        log_error("put_old is not a directory inside the new root");
        return false;
    }

    if (syscall(SYS_pivot_root, new_root.c_str(), put_old.c_str()) != 0) {
        log_warn("pivot_root returned error (" + std::string(strerror(errno)) + "), attempting chroot fallback");
        if (chroot(new_root.c_str()) != 0) {
            log_error("chroot fallback failed: " + std::string(strerror(errno)));
            return false;
        }
        if (chdir("/") != 0) {
            log_error("chdir / after chroot failed: " + std::string(strerror(errno)));
            return false;
        }
        log_warn("Container running under chroot fallback (security boundary is pathname-only)");
        return true;
    }

    if (chdir("/") != 0) {
        log_error("chdir /: " + std::string(strerror(errno)));
        return false;
    }

    // Unmount the old root with lazy detach
    if (umount2("/put_old", MNT_DETACH) != 0) {
        log_error("umount /put_old: " + std::string(strerror(errno)));
        return false;
    }

    rmdir("/put_old");
    log_info("pivot_root successful");
    return true;
}

// ── cleanup_rootfs ───────────────────────────────────────────────────────────

bool cleanup_rootfs(const std::string& rootfs_path) {
    auto try_umount = [&](const std::string& rel) {
        fs::path p = fs::path(rootfs_path) / rel;
        std::error_code ec;
        if (fs::exists(p, ec)) {
            if (umount2(p.c_str(), 0) != 0) {
                umount2(p.c_str(), MNT_DETACH); // lazy fallback
            }
        }
    };

    try_umount("dev/pts");
    try_umount("dev");
    try_umount("tmp");
    try_umount("sys");
    try_umount("proc");
    try_umount(""); // rootfs bind mount itself

    log_info("Rootfs cleanup done");
    return true;
}

} // namespace kernelbox::filesystem
