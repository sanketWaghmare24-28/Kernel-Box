// KernelBox — Lightweight Container Runtime
// common.hpp — Shared types, constants, and utilities

#pragma once

#include <string>
#include <vector>
#include <utility>
#include <optional>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <random>
#include <cstdint>
#include <sys/types.h>

namespace kernelbox {

// ── Status Enum ──────────────────────────────────────────────────────────────

enum class ContainerStatus {
    CREATED  = 0,
    RUNNING  = 1,
    STOPPED  = 2,
    REMOVING = 3
};

inline std::string status_to_string(ContainerStatus s) {
    switch (s) {
        case ContainerStatus::CREATED:  return "Created";
        case ContainerStatus::RUNNING:  return "Running";
        case ContainerStatus::STOPPED:  return "Stopped";
        case ContainerStatus::REMOVING: return "Removing";
    }
    return "Unknown";
}

// ── Image Manifest ───────────────────────────────────────────────────────────

struct ImageManifest {
    std::string name;
    std::string version      = "latest";
    std::string tag;                         // name:version combined
    std::vector<std::string> entrypoint;
    std::vector<std::string> args;
    std::string working_dir  = "/";
    std::vector<std::string> env;
    std::vector<std::string> exposed_ports;
    std::string description;
    std::string architecture = "x86_64";
    uint64_t    size_bytes   = 0;
    std::string created_at;
};

// ── Container State (persisted to disk) ──────────────────────────────────────

struct ContainerState {
    std::string     id;
    std::string     name;
    std::string     image;
    pid_t           pid        = 0;
    ContainerStatus status     = ContainerStatus::CREATED;
    std::string     created_at;
    int             exit_code  = -1;
    std::string     cpu_limit;     // e.g. "50000 100000"
    std::string     memory_limit;  // bytes as string
};

// ── Path Constants ───────────────────────────────────────────────────────────

inline const std::string KERNELBOX_ROOT = "/var/lib/kernelbox";
inline const std::string PORTARUN_ROOT  = KERNELBOX_ROOT; // Alias for backward compatibility
inline const std::string IMAGES_DIR     = KERNELBOX_ROOT + "/images";
inline const std::string CONTAINERS_DIR = KERNELBOX_ROOT + "/containers";
inline const std::string BASE_DIR       = KERNELBOX_ROOT + "/base";
inline const std::string LOGS_DIR       = KERNELBOX_ROOT + "/logs";

// ── Logging ──────────────────────────────────────────────────────────────────

inline void log_info(const std::string& msg) {
    std::cerr << "\033[32m[INFO]\033[0m  " << msg << std::endl;
}

inline void log_error(const std::string& msg) {
    std::cerr << "\033[31m[ERROR]\033[0m " << msg << std::endl;
}

inline void log_warn(const std::string& msg) {
    std::cerr << "\033[33m[WARN]\033[0m  " << msg << std::endl;
}

inline void log_debug([[maybe_unused]] const std::string& msg) {
#ifdef DEBUG
    std::cerr << "[DEBUG] " << msg << std::endl;
#endif
}

// ── Utility Functions ────────────────────────────────────────────────────────

/// Generate a 12-character lowercase hex container ID.
inline std::string generate_id() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);
    std::stringstream ss;
    for (int i = 0; i < 12; ++i) {
        ss << std::hex << dis(gen);
    }
    return ss.str();
}

/// Validate a container name (1-64 chars, alphanumeric + hyphen + underscore).
inline bool validate_name(const std::string& name) {
    if (name.empty() || name.length() > 64) return false;
    for (char c : name) {
        if (!std::isalnum(c) && c != '-' && c != '_') return false;
    }
    return true;
}

/// ISO-8601 timestamp.
inline std::string current_timestamp() {
    auto now  = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time), "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

// ── Minimal JSON Helpers (no external dependency) ────────────────────────────

namespace json {

/// Extract a string value for the given key from a flat JSON object.
inline std::string get_string(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos += search.length();
    // skip colon and whitespace
    while (pos < json.size() && (json[pos] == ':' || json[pos] == ' ' || json[pos] == '\t')) pos++;
    if (pos >= json.size() || json[pos] != '"') return "";
    pos++; // skip opening quote
    auto end = json.find('"', pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

/// Extract an integer value for the given key.
inline int get_int(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return 0;
    pos += search.length();
    while (pos < json.size() && (json[pos] == ':' || json[pos] == ' ' || json[pos] == '\t')) pos++;
    auto end = json.find_first_of(",}\n", pos);
    if (end == std::string::npos) return 0;
    try { return std::stoi(json.substr(pos, end - pos)); }
    catch (...) { return 0; }
}

/// Extract an array of strings for the given key.
inline std::vector<std::string> get_array(const std::string& json, const std::string& key) {
    std::vector<std::string> result;
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return result;
    pos += search.length();
    while (pos < json.size() && (json[pos] == ':' || json[pos] == ' ' || json[pos] == '\t')) pos++;
    if (pos >= json.size() || json[pos] != '[') return result;
    auto bracket_end = json.find(']', pos);
    if (bracket_end == std::string::npos) return result;
    std::string content = json.substr(pos + 1, bracket_end - pos - 1);
    // extract quoted strings
    size_t s = 0;
    while ((s = content.find('"', s)) != std::string::npos) {
        auto e = content.find('"', s + 1);
        if (e == std::string::npos) break;
        result.push_back(content.substr(s + 1, e - s - 1));
        s = e + 1;
    }
    return result;
}

} // namespace json
} // namespace kernelbox
