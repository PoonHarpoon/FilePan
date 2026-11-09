// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

// A DocumentProvider over host directories: a tree's URI is a directory's path, and document ids
// are "doc-" and the inode number, so that nothing can mistake them for paths. Like Android's
// providers, it picks another name on a clash instead of failing.

#include <atomic>
#include <cerrno>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "filepan/vfs/document_backend.hpp"

namespace filepan::test {

class FakeDocumentProvider final : public vfs::DocumentProvider {
public:
    bool supportsMove = true;
    std::atomic<int> calls = 0;

    Result<vfs::DocumentInfo> root(std::string_view tree) override {
        ++calls;
        std::error_code ec;
        if (!std::filesystem::is_directory(std::filesystem::path(tree), ec)) {
            return fail(ENOENT, std::string(tree));
        }
        return info(std::filesystem::path(tree));
    }

    Result<std::vector<vfs::DocumentInfo>> children(std::string_view tree, std::string_view documentId) override {
        ++calls;
        const auto path = find(tree, documentId);
        if (!path) {
            return fail(ENOENT);
        }
        std::vector<vfs::DocumentInfo> out;
        std::error_code ec;
        for (const auto &child : std::filesystem::directory_iterator(*path, ec)) {
            out.push_back(*info(child.path()));
        }
        return out;
    }

    Result<vfs::DocumentInfo> create(std::string_view tree, std::string_view parentId, std::string_view name,
                                     bool directory) override {
        ++calls;
        const auto parent = find(tree, parentId);
        if (!parent) {
            return fail(ENOENT);
        }
        std::filesystem::path path = *parent / std::string(name);
        for (int n = 1; std::filesystem::exists(path); ++n) {
            path = *parent / (std::string(name) + " (" + std::to_string(n) + ")");
        }
        if (directory) {
            std::filesystem::create_directory(path);
        } else {
            ::close(::open(path.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0644));
        }
        return info(path);
    }

    Result<std::string> rename(std::string_view tree, std::string_view documentId, std::string_view name) override {
        ++calls;
        const auto path = find(tree, documentId);
        if (!path) {
            return fail(ENOENT);
        }
        const auto target = path->parent_path() / std::string(name);
        if (std::filesystem::exists(target)) {
            return fail(EEXIST);
        }
        std::filesystem::rename(*path, target);
        return std::string(documentId);
    }

    Result<std::string> move(std::string_view tree, std::string_view documentId, std::string_view,
                             std::string_view toParentId) override {
        ++calls;
        if (!supportsMove) {
            return fail(ENOTSUP);
        }
        const auto path = find(tree, documentId);
        const auto parent = find(tree, toParentId);
        if (!path || !parent) {
            return fail(ENOENT);
        }
        std::filesystem::rename(*path, *parent / path->filename());
        return std::string(documentId);
    }

    Result<void> remove(std::string_view tree, std::string_view documentId) override {
        ++calls;
        const auto path = find(tree, documentId);
        if (!path) {
            return fail(ENOENT);
        }
        std::error_code ec;
        std::filesystem::remove_all(*path, ec);
        return ec ? fail(ec.value()) : Result<void>();
    }

    Result<int> open(std::string_view tree, std::string_view documentId, std::string_view mode) override {
        ++calls;
        const auto path = find(tree, documentId);
        if (!path) {
            return fail(ENOENT);
        }
        const int flags = mode == "r"    ? O_RDONLY
                          : mode == "wt" ? O_WRONLY | O_TRUNC
                          : mode == "rw" ? O_RDWR
                                         : O_WRONLY;
        const int fd = ::open(path->c_str(), flags | O_CLOEXEC);
        return fd == -1 ? Result<int>(failErrno()) : fd;
    }

    // The id of the document at path.
    static std::string idOf(const std::filesystem::path &path) {
        struct stat st {};
        ::stat(path.c_str(), &st);
        return "doc-" + std::to_string(st.st_ino);
    }

private:
    static Result<vfs::DocumentInfo> info(const std::filesystem::path &path) {
        struct stat st {};
        if (::stat(path.c_str(), &st) == -1) {
            return failErrno();
        }
        return vfs::DocumentInfo{
                .id = idOf(path),
                .name = path.filename().string(),
                .directory = S_ISDIR(st.st_mode),
                .size = static_cast<std::uint64_t>(st.st_size),
                .modifiedMs = static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1000 + st.st_mtim.tv_nsec / 1'000'000,
                .flags = vfs::DocumentInfo::kSupportsWrite | vfs::DocumentInfo::kSupportsDelete |
                         vfs::DocumentInfo::kSupportsRename | vfs::DocumentInfo::kSupportsMove};
    }

    // Where the document with id is, by walking the tree.
    static std::optional<std::filesystem::path> find(std::string_view tree, std::string_view id) {
        const std::filesystem::path top{std::string(tree)};
        if (idOf(top) == id) {
            return top;
        }
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(top, ec);
             it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (idOf(it->path()) == id) {
                return it->path();
            }
        }
        return std::nullopt;
    }
};

}  // namespace filepan::test
