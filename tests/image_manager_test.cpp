#include "../include/image/image_manager.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

namespace fs = std::filesystem;

int main() {
    const fs::path temp = fs::temp_directory_path() /
        ("kernelbox-image-test-" + std::to_string(getpid()));
    const fs::path context = temp / "context";
    const std::string malicious_item = "payload; touch injected-marker; #";
    fs::create_directories(context / malicious_item);
    std::ofstream(context / "Boxfile.json")
        << "{\"name\":\"test\",\"version\":\"v1\",\"working_dir\":\"/app\","
           "\"copy\":[\"payload; touch injected-marker; #\"]}\n";
    std::ofstream(context / malicious_item / "app.txt") << "test\n";
    const fs::path unsafe_context = temp / "unsafe-context";
    fs::create_directories(unsafe_context);
    std::ofstream(unsafe_context / "Boxfile.json")
        << "{\"name\":\"unsafe\",\"working_dir\":\"/app\",\"copy\":[\"../secret\"]}\n";
    std::ofstream(temp / "secret") << "outside context\n";

    const fs::path original_dir = fs::current_path();
    fs::current_path(temp);
    kernelbox::image::ImageManager manager((temp / "images").string());
    const bool build_succeeded = manager.build(context.string(), "test:v1");
    const bool traversal_succeeded = manager.build(unsafe_context.string(), "unsafe:v1");
    const bool invalid_tag_succeeded = manager.build(context.string(), "..");
    fs::current_path(original_dir);

    const bool marker_created = fs::exists(temp / "injected-marker");
    fs::remove_all(temp);
    if (!build_succeeded) {
        std::cerr << "Image build failed\n";
        return 1;
    }
    if (marker_created) {
        std::cerr << "Shell metacharacters in a copy path were executed\n";
        return 1;
    }
    if (traversal_succeeded || invalid_tag_succeeded) {
        std::cerr << "Image path validation accepted an unsafe input\n";
        return 1;
    }
    return 0;
}
