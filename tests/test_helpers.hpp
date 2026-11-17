#pragma once

#include <unistd.h>

#include <filesystem>
#include <string>
#include <system_error>

namespace redislite::testing {

// A unique path in the system temp directory, deleted when the object goes out of scope.
class TempFile {
public:
    explicit TempFile(const std::string& name)
        : path_(std::filesystem::temp_directory_path() /
                ("redislite_" + std::to_string(::getpid()) + "_" + name)) {
        removeFiles();
    }
    ~TempFile() { removeFiles(); }

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    std::string path() const { return path_.string(); }

private:
    void removeFiles() const {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        std::filesystem::remove(path_.string() + ".tmp", ignored);
    }

    std::filesystem::path path_;
};

}  // namespace redislite::testing
