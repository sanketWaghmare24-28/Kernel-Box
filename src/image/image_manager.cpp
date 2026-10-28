// KernelBox — Image Module Implementation
// Build, list, import, export, extract images with Boxfile.json (or Portafile.json) manifests

#include "../../include/image/image_manager.hpp"
#include "../../include/common.hpp"

#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <iostream>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <sys/wait.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace kernelbox::image {

namespace {

std::vector<char*> make_argv(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    return argv;
}

bool wait_success(pid_t pid) {
    int status = 0;
    pid_t result;
    do {
        result = waitpid(pid, &status, 0);
    } while (result == -1 && errno == EINTR);
    return result == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool run_process(const std::vector<std::string>& args) {
    if (args.empty()) return false;
    pid_t pid = fork();
    if (pid == -1) {
        log_error("fork " + args.front() + ": " + strerror(errno));
        return false;
    }
    if (pid == 0) {
        auto argv = make_argv(args);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    return wait_success(pid);
}

bool capture_process(const std::vector<std::string>& args, std::string& output) {
    if (args.empty()) return false;
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        log_error("pipe " + args.front() + ": " + strerror(errno));
        return false;
    }

    pid_t pid = fork();
    if (pid == -1) {
        log_error("fork " + args.front() + ": " + strerror(errno));
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return false;
    }
    if (pid == 0) {
        close(pipe_fds[0]);
        if (dup2(pipe_fds[1], STDOUT_FILENO) == -1) _exit(127);
        close(pipe_fds[1]);
        auto argv = make_argv(args);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    close(pipe_fds[1]);
    char buffer[4096];
    bool read_ok = true;
    for (;;) {
        ssize_t count = read(pipe_fds[0], buffer, sizeof(buffer));
        if (count > 0) {
            output.append(buffer, static_cast<size_t>(count));
        } else if (count == 0) {
            break;
        } else if (errno != EINTR) {
            log_error("read " + args.front() + ": " + strerror(errno));
            read_ok = false;
            break;
        }
    }
    close(pipe_fds[0]);
    return read_ok && wait_success(pid);
}

bool path_is_within(const fs::path& root, const fs::path& candidate) {
    fs::path relative = candidate.lexically_relative(root);
    if (relative.empty()) return candidate == root;
    return *relative.begin() != "..";
}

bool valid_tag(const std::string& tag) {
    if (tag.empty() || tag.size() > 128 || tag == "." || tag == "..") return false;
    for (unsigned char c : tag) {
        if (!std::isalnum(c) && c != '.' && c != '_' && c != '-' && c != ':' && c != '/') {
            return false;
        }
    }
    return true;
}

std::string compute_cache_stamp(const fs::path& tarball, const fs::path& manifest_path) {
    struct stat st;
    if (stat(tarball.c_str(), &st) != 0) {
        return "";
    }
    std::string stamp = "size=" + std::to_string(st.st_size) +
                        ";mtime=" + std::to_string(st.st_mtim.tv_sec) + "." + std::to_string(st.st_mtim.tv_nsec);
    if (fs::exists(manifest_path)) {
        struct stat mst;
        if (stat(manifest_path.c_str(), &mst) == 0) {
            stamp += ";manifest_size=" + std::to_string(mst.st_size) +
                     ";manifest_mtime=" + std::to_string(mst.st_mtim.tv_sec) + "." + std::to_string(mst.st_mtim.tv_nsec);
        }
    }
    return stamp;
}

bool is_cache_valid(const fs::path& cache_dir, const fs::path& stamp_file, const std::string& expected_stamp) {
    if (expected_stamp.empty()) return false;
    std::error_code ec;
    if (!fs::is_directory(cache_dir, ec) || ec) return false;
    if (fs::is_empty(cache_dir, ec) || ec) return false;
    if (!fs::exists(stamp_file, ec) || ec) return false;
    std::ifstream ifs(stamp_file);
    if (!ifs.is_open()) return false;
    std::string current_stamp;
    std::getline(ifs, current_stamp);
    return (current_stamp == expected_stamp);
}

bool ensure_cache(const fs::path& image_dir, const fs::path& tarball, const fs::path& manifest_path) {
    fs::path lock_file = image_dir / ".cache.lock";
    int lock_fd = open(lock_file.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (lock_fd < 0) {
        log_warn("Failed to open cache lock file: " + lock_file.string());
        return false;
    }

    struct LockGuard {
        int fd;
        ~LockGuard() {
            flock(fd, LOCK_UN);
            close(fd);
        }
    } guard{lock_fd};

    if (flock(lock_fd, LOCK_EX) != 0) {
        log_warn("flock failed on " + lock_file.string());
        return false;
    }

    std::string expected_stamp = compute_cache_stamp(tarball, manifest_path);
    if (expected_stamp.empty()) return false;

    fs::path cache_dir = image_dir / ".rootfs_cache";
    fs::path stamp_file = image_dir / ".cache_stamp";

    if (is_cache_valid(cache_dir, stamp_file, expected_stamp)) {
        return true;
    }

    fs::path tmp_dir = image_dir / (".cache_tmp_" + std::to_string(getpid()));
    std::error_code ec;
    fs::remove_all(tmp_dir, ec);
    fs::create_directories(tmp_dir, ec);
    if (ec) {
        log_warn("Failed to create temporary cache dir: " + tmp_dir.string());
        return false;
    }

    if (!run_process({"tar", "-xzf", fs::absolute(tarball).string(),
                      "-C", fs::absolute(tmp_dir).string()})) {
        log_warn("Failed to extract tarball to temporary cache");
        fs::remove_all(tmp_dir, ec);
        return false;
    }

    fs::remove_all(cache_dir, ec);
    fs::rename(tmp_dir, cache_dir, ec);
    if (ec) {
        log_warn("Failed to rename temporary cache to rootfs_cache: " + ec.message());
        fs::remove_all(tmp_dir, ec);
        return false;
    }

    std::ofstream ofs(stamp_file);
    if (ofs.is_open()) {
        ofs << expected_stamp << "\n";
        ofs.close();
    } else {
        log_warn("Failed to write cache stamp file: " + stamp_file.string());
    }

    return true;
}

} // namespace

// ── Tag sanitisation ─────────────────────────────────────────────────────────

std::string ImageManager::sanitize_tag(const std::string& tag) {
    std::string r = tag;
    std::replace(r.begin(), r.end(), ':', '_');
    std::replace(r.begin(), r.end(), '/', '_');
    return r;
}

// ── Archive safety ───────────────────────────────────────────────────────────

bool ImageManager::validate_archive_paths(const std::string& archive) {
    // List entries and reject any with ".." or absolute paths
    std::string entries;
    if (!capture_process({"tar", "-tf", fs::absolute(archive).string()}, entries)) return false;
    std::istringstream input(entries);
    bool safe = true;
    std::string entry;
    while (std::getline(input, entry)) {
        if (!entry.empty() && entry.back() == '\n') entry.pop_back();
        if (entry.find("..") != std::string::npos || (!entry.empty() && entry[0] == '/')) {
            log_error("Unsafe archive entry: " + entry);
            safe = false;
            break;
        }
    }
    return safe;
}

// ── Manifest I/O ─────────────────────────────────────────────────────────────

ImageManifest ImageManager::parse_manifest(const std::string& path) {
    std::ifstream ifs(path);
    std::stringstream buf;
    buf << ifs.rdbuf();
    std::string j = buf.str();

    ImageManifest m;
    m.name         = json::get_string(j, "name");
    m.version      = json::get_string(j, "version");
    if (m.version.empty()) m.version = "latest";
    m.tag          = json::get_string(j, "tag");
    if (m.tag.empty()) m.tag = m.name + ":" + m.version;
    m.working_dir  = json::get_string(j, "working_dir");
    m.description  = json::get_string(j, "description");
    m.architecture = json::get_string(j, "architecture");
    if (m.architecture.empty()) m.architecture = "x86_64";
    m.entrypoint   = json::get_array(j, "entrypoint");
    m.args         = json::get_array(j, "args");
    m.env          = json::get_array(j, "env");
    m.exposed_ports = json::get_array(j, "exposed_ports");

    // Compute size
    fs::path rootfs_tar = fs::path(path).parent_path() / "rootfs.tar.gz";
    if (fs::exists(rootfs_tar)) {
        m.size_bytes = fs::file_size(rootfs_tar);
    }
    return m;
}

bool ImageManager::write_manifest(const std::string& path, const ImageManifest& m) {
    std::ofstream ofs(path);
    if (!ofs) return false;

    auto arr = [](const std::vector<std::string>& v) -> std::string {
        std::string s = "[";
        for (size_t i = 0; i < v.size(); ++i) {
            if (i) s += ", ";
            s += "\"" + v[i] + "\"";
        }
        s += "]";
        return s;
    };

    ofs << "{\n"
        << "  \"name\": \""         << m.name         << "\",\n"
        << "  \"version\": \""      << m.version      << "\",\n"
        << "  \"tag\": \""          << m.tag           << "\",\n"
        << "  \"entrypoint\": "     << arr(m.entrypoint) << ",\n"
        << "  \"args\": "           << arr(m.args)       << ",\n"
        << "  \"working_dir\": \""  << m.working_dir   << "\",\n"
        << "  \"env\": "            << arr(m.env)        << ",\n"
        << "  \"exposed_ports\": "  << arr(m.exposed_ports) << ",\n"
        << "  \"description\": \""  << m.description   << "\",\n"
        << "  \"architecture\": \"" << m.architecture  << "\"\n"
        << "}\n";
    return true;
}

// ── build ────────────────────────────────────────────────────────────────────

bool ImageManager::build(const std::string& context_dir, const std::string& tag) {
    if (!valid_tag(tag)) {
        log_error("Invalid image tag: " + tag);
        return false;
    }

    fs::path ctx(context_dir);
    fs::path boxfile = ctx / "Boxfile.json";
    fs::path portafile = ctx / "Portafile.json";
    fs::path manifest_file;

    if (fs::exists(boxfile)) {
        manifest_file = boxfile;
    } else if (fs::exists(portafile)) {
        manifest_file = portafile;
    } else {
        log_error("Boxfile.json (or Portafile.json) not found in " + context_dir);
        return false;
    }

    // Parse the manifest
    std::ifstream ifs(manifest_file);
    std::stringstream buf;
    buf << ifs.rdbuf();
    std::string j = buf.str();

    ImageManifest m;
    m.name         = json::get_string(j, "name");
    m.version      = json::get_string(j, "version");
    if (m.version.empty()) m.version = "latest";
    m.tag          = tag;
    m.entrypoint   = json::get_array(j, "entrypoint");
    m.args         = json::get_array(j, "args");
    m.working_dir  = json::get_string(j, "working_dir");
    if (m.working_dir.empty()) m.working_dir = "/";
    m.env          = json::get_array(j, "env");
    m.exposed_ports = json::get_array(j, "exposed_ports");
    m.description  = json::get_string(j, "description");
    m.created_at   = current_timestamp();

    std::string base = json::get_string(j, "base");
    std::string stag = sanitize_tag(tag);
    fs::path image_dir = fs::path(images_dir_) / stag;
    fs::create_directories(image_dir);

    // Assemble rootfs in a temporary directory
    fs::path rootfs_tmp = image_dir / "rootfs_build";
    fs::create_directories(rootfs_tmp);
    auto fail_build = [&]() {
        fs::remove_all(rootfs_tmp);
        return false;
    };

    // Copy base rootfs if specified
    if (base == "alpine") {
        fs::path alpine = fs::path(BASE_DIR) / "alpine-rootfs";
        if (!fs::exists(alpine)) {
            fs::path legacy_alpine = fs::path("/var/lib/portarun/base/alpine-rootfs");
            if (fs::exists(legacy_alpine)) {
                alpine = legacy_alpine;
            }
        }
        
        if (!fs::exists(alpine)) {
            log_info("Alpine base rootfs not found. Downloading and configuring (this may take a moment)...");
            const std::string archive = "/tmp/alpine.tar.gz";
            std::error_code ec;
            fs::create_directories(alpine, ec);
            bool setup_ok = !ec &&
                run_process({"wget", "-qO", archive,
                             "https://dl-cdn.alpinelinux.org/alpine/v3.19/releases/x86_64/alpine-minirootfs-3.19.1-x86_64.tar.gz"}) &&
                run_process({"tar", "-xzf", archive, "-C", alpine.string()});
            fs::remove(archive, ec);
            if (setup_ok) {
                fs::copy_file("/etc/resolv.conf", alpine / "etc" / "resolv.conf",
                              fs::copy_options::overwrite_existing, ec);
                setup_ok = !ec && run_process({"chroot", alpine.string(), "/sbin/apk", "add", "--no-cache", "nodejs"});
            }
            if (!setup_ok) {
                log_error("Failed to set up Alpine base rootfs");
            }
        }
        
        if (fs::exists(alpine)) {
            log_info("Copying Alpine base rootfs...");
            if (!run_process({"cp", "-a", (alpine / ".").string(), (rootfs_tmp / ".").string()})) {
                log_warn("Base rootfs copy had issues");
            }
        } else {
            log_warn("Alpine base rootfs not found at " + alpine.string());
            log_info("Building without base — container will only have application files");
        }
    }

    // Copy application files into working_dir inside rootfs
    auto copy_list = json::get_array(j, "copy");
    const fs::path canonical_context = fs::canonical(ctx);
    fs::path working_dir = fs::path(m.working_dir).lexically_normal();
    if (working_dir.is_absolute()) working_dir = working_dir.relative_path();
    if (working_dir == ".." || (!working_dir.empty() && *working_dir.begin() == "..")) {
        log_error("Invalid working_dir: " + m.working_dir);
        return fail_build();
    }
    fs::path dest = rootfs_tmp / working_dir;
    fs::create_directories(dest);

    for (const auto& item : copy_list) {
        fs::path relative_item(item);
        if (relative_item.is_absolute() ||
            std::find(relative_item.begin(), relative_item.end(), fs::path("..")) != relative_item.end()) {
            log_error("Invalid copy source: " + item);
            return fail_build();
        }

        fs::path src = (ctx / relative_item).lexically_normal();
        std::error_code ec;
        if (!fs::exists(src, ec) || ec) {
            log_error("Copy source not found: " + item);
            return fail_build();
        }
        const fs::path canonical_source = fs::canonical(src);
        if (!path_is_within(canonical_context, canonical_source)) {
            log_error("Copy source escapes build context: " + item);
            return fail_build();
        }

        if (fs::is_directory(fs::symlink_status(src))) {
            if (!run_process({"cp", "-a", (src / ".").string(), (dest / ".").string()})) {
                log_error("Failed to copy directory: " + item);
                return fail_build();
            }
        } else if (!run_process({"cp", "-a", src.string(), (dest / src.filename()).string()})) {
            log_error("Failed to copy file: " + item);
            return fail_build();
        }
    }

    // Create rootfs.tar.gz
    fs::path tarball = image_dir / "rootfs.tar.gz";
    log_info("Creating rootfs tarball...");
    if (!run_process({"tar", "-czf", fs::absolute(tarball).string(),
                      "-C", fs::absolute(rootfs_tmp).string(), "."})) {
        log_error("Failed to create rootfs tarball");
        fs::remove_all(rootfs_tmp);
        return false;
    }

    // Write manifest
    write_manifest((image_dir / "manifest.json").string(), m);

    // Clean up build directory
    fs::remove_all(rootfs_tmp);

    // Invalidate any existing template cache for this image tag
    std::error_code ec_cache;
    fs::remove_all(image_dir / ".rootfs_cache", ec_cache);
    fs::remove(image_dir / ".cache_stamp", ec_cache);

    m.size_bytes = fs::file_size(tarball);
    log_info("Image " + tag + " built (" + std::to_string(m.size_bytes / 1024) + " KiB)");
    return true;
}

// ── list ─────────────────────────────────────────────────────────────────────

std::vector<ImageManifest> ImageManager::list() {
    std::vector<ImageManifest> result;
    if (!fs::exists(images_dir_)) return result;
    for (const auto& entry : fs::directory_iterator(images_dir_)) {
        if (!entry.is_directory()) continue;
        fs::path mf = entry.path() / "manifest.json";
        if (fs::exists(mf)) {
            result.push_back(parse_manifest(mf.string()));
        }
    }
    return result;
}

// ── remove ───────────────────────────────────────────────────────────────────

bool ImageManager::remove(const std::string& tag) {
    if (!valid_tag(tag)) return false;
    fs::path dir = fs::path(images_dir_) / sanitize_tag(tag);
    if (fs::exists(dir)) return fs::remove_all(dir) > 0;
    log_error("Image not found: " + tag);
    return false;
}

// ── export ───────────────────────────────────────────────────────────────────

bool ImageManager::export_image(const std::string& tag, const std::string& output_path) {
    if (!valid_tag(tag)) return false;
    fs::path dir = fs::path(images_dir_) / sanitize_tag(tag);
    if (!fs::exists(dir)) {
        fs::path legacy_dir = fs::path("/var/lib/portarun/images") / sanitize_tag(tag);
        if (fs::exists(legacy_dir)) {
            dir = legacy_dir;
        } else {
            log_error("Image not found: " + tag);
            return false;
        }
    }
    return run_process({"tar", "-czf", fs::absolute(output_path).string(),
                        "-C", fs::absolute(dir).string(), "manifest.json", "rootfs.tar.gz"});
}

// ── import ───────────────────────────────────────────────────────────────────

bool ImageManager::import_image(const std::string& archive_path) {
    if (!validate_archive_paths(archive_path)) {
        log_error("Archive failed safety validation");
        return false;
    }
    fs::path tmp = fs::path(images_dir_) / "_import_tmp";
    fs::create_directories(tmp);
    if (!run_process({"tar", "-xzf", fs::absolute(archive_path).string(),
                      "-C", fs::absolute(tmp).string()})) { fs::remove_all(tmp); return false; }
    if (!fs::exists(tmp / "manifest.json")) { fs::remove_all(tmp); return false; }

    ImageManifest m = parse_manifest((tmp / "manifest.json").string());
    if (!valid_tag(m.tag)) {
        log_error("Invalid image tag in archive: " + m.tag);
        fs::remove_all(tmp);
        return false;
    }
    fs::path dest = fs::path(images_dir_) / sanitize_tag(m.tag);
    fs::remove_all(dest);
    fs::rename(tmp, dest);
    log_info("Imported image " + m.tag);
    return true;
}

// ── get_manifest ─────────────────────────────────────────────────────────────

std::optional<ImageManifest> ImageManager::get_manifest(const std::string& tag) {
    if (!valid_tag(tag)) return std::nullopt;
    fs::path mf = fs::path(images_dir_) / sanitize_tag(tag) / "manifest.json";
    if (!fs::exists(mf)) {
        fs::path legacy_mf = fs::path("/var/lib/portarun/images") / sanitize_tag(tag) / "manifest.json";
        if (fs::exists(legacy_mf)) {
            return parse_manifest(legacy_mf.string());
        }
        return std::nullopt;
    }
    return parse_manifest(mf.string());
}

// ── extract ──────────────────────────────────────────────────────────────────

bool ImageManager::extract(const std::string& tag, const std::string& dest_path) {
    if (!valid_tag(tag)) return false;
    fs::path tarball = fs::path(images_dir_) / sanitize_tag(tag) / "rootfs.tar.gz";
    if (!fs::exists(tarball)) {
        fs::path legacy_tarball = fs::path("/var/lib/portarun/images") / sanitize_tag(tag) / "rootfs.tar.gz";
        if (fs::exists(legacy_tarball)) {
            tarball = legacy_tarball;
        } else {
            log_error("rootfs.tar.gz not found for " + tag);
            return false;
        }
    }

    fs::path image_dir = tarball.parent_path();
    fs::path manifest_path = image_dir / "manifest.json";
    fs::path cache_dir = image_dir / ".rootfs_cache";
    fs::path stamp_file = image_dir / ".cache_stamp";
    fs::path lock_file = image_dir / ".cache.lock";

    std::string expected_stamp = compute_cache_stamp(tarball, manifest_path);

    bool cache_ready = false;
    if (is_cache_valid(cache_dir, stamp_file, expected_stamp)) {
        cache_ready = true;
    } else {
        cache_ready = ensure_cache(image_dir, tarball, manifest_path);
    }

    fs::create_directories(dest_path);

    if (cache_ready) {
        int lock_fd = open(lock_file.c_str(), O_RDONLY | O_CLOEXEC, 0644);
        if (lock_fd >= 0) {
            flock(lock_fd, LOCK_SH);
        }

        bool copy_ok = run_process({"cp", "-a", "--reflink=auto",
                                    (cache_dir / ".").string(),
                                    (fs::path(dest_path) / ".").string()});

        if (lock_fd >= 0) {
            flock(lock_fd, LOCK_UN);
            close(lock_fd);
        }

        if (copy_ok) {
            return true;
        }

        log_warn("cp -a --reflink=auto from cache failed, falling back to direct tar extraction");
        std::error_code ec;
        fs::remove_all(dest_path, ec);
        fs::create_directories(dest_path, ec);
    }

    return run_process({"tar", "-xzf", fs::absolute(tarball).string(),
                        "-C", fs::absolute(dest_path).string()});
}


} // namespace kernelbox::image
