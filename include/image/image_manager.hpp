// KernelBox — Image Module
// Build, list, import, export, and extract container images

#pragma once
#include <string>
#include <vector>
#include <optional>
#include "../common.hpp"

namespace kernelbox::image {

class ImageManager {
public:
    /// Build an image from a context directory containing Boxfile.json (or Portafile.json).
    bool build(const std::string& context_dir, const std::string& tag);

    /// List all images stored under IMAGES_DIR.
    std::vector<ImageManifest> list();

    /// Remove an image by tag.
    bool remove(const std::string& tag);

    /// Export image to a portable .tar.gz archive.
    bool export_image(const std::string& tag, const std::string& output_path);

    /// Import image from a .tar.gz archive (validates against path traversal).
    bool import_image(const std::string& archive_path);

    /// Read the manifest for a tag.
    std::optional<ImageManifest> get_manifest(const std::string& tag);

    /// Extract the rootfs tarball into dest_path.
    bool extract(const std::string& tag, const std::string& dest_path);

private:
    std::string images_dir_ = IMAGES_DIR;

    ImageManifest parse_manifest(const std::string& path);
    bool          write_manifest(const std::string& path, const ImageManifest& m);
    std::string   sanitize_tag(const std::string& tag);
    bool          validate_archive_paths(const std::string& archive);
};

} // namespace kernelbox::image
