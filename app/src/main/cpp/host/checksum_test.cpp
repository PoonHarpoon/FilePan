// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include "filepan/checksum.hpp"
#include "test_util.hpp"

using filepan::ChecksumAlgorithm;
using filepan::test::TempDir;

namespace {

class Progress : public filepan::ChecksumCallbacks {
public:
    std::uint64_t total = 0;
    bool abort = false;
    bool onProgress(std::uint64_t size) override {
        total += size;
        return !abort;
    }
};

std::string hex(const std::vector<std::uint8_t> &bytes) {
    std::string string;
    char buffer[3];
    for (const auto byte : bytes) {
        std::snprintf(buffer, sizeof(buffer), "%02x", byte);
        string += buffer;
    }
    return string;
}

const std::vector<ChecksumAlgorithm> kAll = {
    ChecksumAlgorithm::kCrc32, ChecksumAlgorithm::kMd5, ChecksumAlgorithm::kSha1,
    ChecksumAlgorithm::kSha256, ChecksumAlgorithm::kSha512};

void expectDigests(const std::string &content, const char *crc32, const char *md5,
                   const char *sha1, const char *sha256, const char *sha512) {
    TempDir dir;
    const auto path = dir.path() / "file";
    std::ofstream(path, std::ios::binary) << content;
    Progress progress;
    const auto result = filepan::computeChecksums(path.string(), kAll, progress);
    EXPECT(result.error == 0);
    EXPECT(hex(result.digests[0]) == crc32);
    EXPECT(hex(result.digests[1]) == md5);
    EXPECT(hex(result.digests[2]) == sha1);
    EXPECT(hex(result.digests[3]) == sha256);
    EXPECT(hex(result.digests[4]) == sha512);
    EXPECT(progress.total == content.size());
}

}  // namespace

int main() {
    // Well-known test vectors.
    expectDigests("", "00000000", "d41d8cd98f00b204e9800998ecf8427e",
                  "da39a3ee5e6b4b0d3255bfef95601890afd80709",
                  "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
                  "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
                  "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");
    expectDigests("abc", "352441c2", "900150983cd24fb0d6963f7d28e17f72",
                  "a9993e364706816aba3e25717850c26c9cd0d89d",
                  "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                  "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                  "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    // A million 'a's spans several read chunks only with small chunks, so also check a file
    // larger than the 4 MiB chunk size.
    expectDigests(std::string(1'000'000, 'a'), "dc25bfbc", "7707d6ae4e027c70eea2a935c2296f21",
                  "34aa973cd4c4daa4f61eeb2bdbad27316534016f",
                  "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
                  "e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973eb"
                  "de0ff244877ea60a4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b");

    {
        TempDir dir;
        const auto path = dir.path() / "big";
        const std::string content(9 * 1024 * 1024 + 5, 'x');
        std::ofstream(path, std::ios::binary) << content;
        Progress progress;
        const auto result = filepan::computeChecksums(
            path.string(), {ChecksumAlgorithm::kSha256}, progress);
        EXPECT(result.error == 0);
        EXPECT(progress.total == content.size());
        EXPECT(result.digests[1].empty());
        EXPECT(result.digests[3].size() == 32);
    }
    {
        Progress progress;
        EXPECT(filepan::computeChecksums("/nonexistent/filepan", kAll, progress).error == ENOENT);
    }
    {
        // From an input that returns short reads, then one that fails.
        class Input : public filepan::ChecksumInput {
        public:
            std::int64_t read(std::uint8_t *buffer, std::size_t size) override {
                if (failAt && offset >= *failAt) {
                    return -EIO;
                }
                const std::size_t length = std::min<std::size_t>({size, 1000, content.size() - offset});
                std::copy_n(content.begin() + static_cast<std::ptrdiff_t>(offset), length, buffer);
                offset += length;
                return static_cast<std::int64_t>(length);
            }
            std::string content = std::string(1'000'000, 'a');
            std::size_t offset = 0;
            std::optional<std::size_t> failAt;
        } input;
        Progress progress;
        const auto result = filepan::computeChecksums(input, {ChecksumAlgorithm::kSha1}, progress);
        EXPECT(result.error == 0 && progress.total == 1'000'000);
        EXPECT(hex(result.digests[2]) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
        input.offset = 0;
        input.failAt = 5000;
        EXPECT(filepan::computeChecksums(input, kAll, progress).error == EIO);
    }
    std::puts("checksum_test: OK");
    return 0;
}
