// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace filepan {

enum class ChecksumAlgorithm : std::int32_t {
    kCrc32 = 0,
    kMd5,
    kSha1,
    kSha256,
    kSha512,
};

constexpr std::size_t kChecksumAlgorithmCount = 5;

class ChecksumCallbacks {
public:
    virtual ~ChecksumCallbacks() = default;
    // Reports bytes hashed since the last call. Returns false to abort.
    virtual bool onProgress(std::uint64_t size) = 0;
};

struct ChecksumResult {
    // 0, ECANCELED, or the errno from reading the file.
    int error = 0;
    // Raw digests, indexed by ChecksumAlgorithm; empty for algorithms not requested.
    std::array<std::vector<std::uint8_t>, kChecksumAlgorithmCount> digests;
};

// Where computeChecksums() reads from when it isn't a local file.
class ChecksumInput {
public:
    virtual ~ChecksumInput() = default;
    // Reads up to size bytes. Returns the size read, 0 at the end, or a negated errno.
    virtual std::int64_t read(std::uint8_t *buffer, std::size_t size) = 0;
};

// Reads the file once, hashing it with each requested algorithm on its own thread.
ChecksumResult computeChecksums(const std::string &path,
                                const std::vector<ChecksumAlgorithm> &algorithms,
                                ChecksumCallbacks &callbacks);

// The same, reading from input.
ChecksumResult computeChecksums(ChecksumInput &input,
                                const std::vector<ChecksumAlgorithm> &algorithms,
                                ChecksumCallbacks &callbacks);

}  // namespace filepan
