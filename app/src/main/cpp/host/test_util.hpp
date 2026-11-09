// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#define EXPECT(condition) \
    do { \
        if (!(condition)) { \
            std::fprintf(stderr, "%s:%d: expectation failed: %s\n", __FILE__, __LINE__, \
                         #condition); \
            std::exit(1); \
        } \
    } while (false)

namespace filepan::test {

// A temporary directory removed on destruction.
class TempDir {
public:
    TempDir() {
        std::string pattern = (std::filesystem::temp_directory_path() / "filepan-XXXXXX").string();
        EXPECT(mkdtemp(pattern.data()) != nullptr);
        path_ = pattern;
    }
    ~TempDir() { std::filesystem::remove_all(path_); }

    [[nodiscard]] const std::filesystem::path &path() const { return path_; }

private:
    std::filesystem::path path_;
};

}  // namespace filepan::test
