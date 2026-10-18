// KernelBox — Image Module Implementation
// Build, list, import, export, extract images with Boxfile.json (or Portafile.json) manifests

#include "../../include/image/image_manager.hpp"
#include "../../include/common.hpp"

#include <fstream>
#include <sstream>
#include <filesystem>
#include <cstdlib>
#include <algorithm>
#include <iostream>

namespace fs = std::filesystem;

namespace kernelbox::image {

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
    FILE* fp = popen(("tar -tf " + archive + " 2>/dev/null").c_str(), "r");
    if (!fp) return false;
    char buf[4096];
    bool safe = true;
    while (fgets(buf, sizeof(buf), fp)) {
        std::string entry(buf);
        // trim newline
        if (!entry.empty() && entry.back() == '\n') entry.pop_back();
        if (entry.find("..") != std::string::npos || (!entry.empty() && entry[0] == '/')) {
            log_error("Unsafe archive entry: " + entry);
            safe = false;
            break;
        }
    }
    pclose(fp);
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
            std::string setup_cmd = 
                "mkdir -p " + alpine.string() + " && " +
                "wget -qO /tmp/alpine.tar.gz https://dl-cdn.alpinelinux.org/alpine/v3.19/releases/x86_64/alpine-minirootfs-3.19.1-x86_64.tar.gz && " +
                "tar -xzf /tmp/alpine.tar.gz -C " + alpine.string() + " && " +
                "rm /tmp/alpine.tar.gz && " +
                "cp /etc/resolv.conf " + alpine.string() + "/etc/ && " +
                "chroot " + alpine.string() + " /sbin/apk add --no-cache nodejs";
                
            if (std::system(setup_cmd.c_str()) != 0) {
                log_error("Failed to set up Alpine base rootfs");
            }
        }
        
        if (fs::exists(alpine)) {
            log_info("Copying Alpine base rootfs...");
            std::string cmd = "cp -a " + alpine.string() + "/. " + rootfs_tmp.string() + "/";
            if (std::system(cmd.c_str()) != 0) {
                log_warn("Base rootfs copy had issues");
            }
        } else {
            log_warn("Alpine base rootfs not found at " + alpine.string());
            log_info("Building without base — container will only have application files");
        }
    }

    // Copy application files into working_dir inside rootfs
    auto copy_list = json::get_array(j, "copy");
    for (const auto& item : copy_list) {
        fs::path src  = ctx / item;
        // Remove leading / from working_dir for relative path inside rootfs
        std::string wd = m.working_dir;
        if (!wd.empty() && wd[0] == '/') wd = wd.substr(1);
        fs::path dest = rootfs_tmp / wd;
        fs::create_directories(dest);

        if (fs::is_directory(src)) {
            std::string cmd = "cp -a " + src.string() + "/. " + dest.string() + "/";
            std::system(cmd.c_str());
        } else if (fs::exists(src)) {
            fs::copy(src, dest / src.filename(), fs::copy_options::overwrite_existing);
        }
    }

    // Create rootfs.tar.gz
    fs::path tarball = image_dir / "rootfs.tar.gz";
    log_info("Creating rootfs tarball...");
    std::string tar_cmd = "tar -czf " + tarball.string() + " -C " + rootfs_tmp.string() + " .";
    if (std::system(tar_cmd.c_str()) != 0) {
        log_error("Failed to create rootfs tarball");
        fs::remove_all(rootfs_tmp);
        return false;
    }

    // Write manifest
    write_manifest((image_dir / "manifest.json").string(), m);

    // Clean up build directory
    fs::remove_all(rootfs_tmp);

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
    fs::path dir = fs::path(images_dir_) / sanitize_tag(tag);
    if (fs::exists(dir)) return fs::remove_all(dir) > 0;
    log_error("Image not found: " + tag);
    return false;
}

// ── export ───────────────────────────────────────────────────────────────────

bool ImageManager::export_image(const std::string& tag, const std::string& output_path) {
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
    std::string cmd = "tar -czf " + output_path + " -C " + dir.string() + " manifest.json rootfs.tar.gz";
    return std::system(cmd.c_str()) == 0;
}

// ── import ───────────────────────────────────────────────────────────────────

bool ImageManager::import_image(const std::string& archive_path) {
    if (!validate_archive_paths(archive_path)) {
        log_error("Archive failed safety validation");
        return false;
    }
    fs::path tmp = fs::path(images_dir_) / "_import_tmp";
    fs::create_directories(tmp);
    std::string cmd = "tar -xzf " + archive_path + " -C " + tmp.string();
    if (std::system(cmd.c_str()) != 0) { fs::remove_all(tmp); return false; }
    if (!fs::exists(tmp / "manifest.json")) { fs::remove_all(tmp); return false; }

    ImageManifest m = parse_manifest((tmp / "manifest.json").string());
    fs::path dest = fs::path(images_dir_) / sanitize_tag(m.tag);
    fs::remove_all(dest);
    fs::rename(tmp, dest);
    log_info("Imported image " + m.tag);
    return true;
}

// ── get_manifest ─────────────────────────────────────────────────────────────

std::optional<ImageManifest> ImageManager::get_manifest(const std::string& tag) {
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
    fs::create_directories(dest_path);
    std::string cmd = "tar -xzf " + tarball.string() + " -C " + dest_path;
    return std::system(cmd.c_str()) == 0;
}

} // namespace kernelbox::image
