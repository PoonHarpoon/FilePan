// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/checksum.hpp"

#include <barrier>
#include <cerrno>
#include <chrono>
#include <memory>
#include <thread>

#include <fcntl.h>
#include <lzma.h>
#include <mbedtls/md.h>
#include <unistd.h>

#include "filepan/eintr.hpp"

namespace filepan {

namespace {

constexpr std::size_t kChunkSize = 4 * 1024 * 1024;
constexpr auto kProgressInterval = std::chrono::milliseconds(200);

class Hasher {
public:
    virtual ~Hasher() = default;
    virtual void update(const std::uint8_t *data, std::size_t size) = 0;
    virtual std::vector<std::uint8_t> finish() = 0;
};

// liblzma's CRC-32 is the same IEEE polynomial as zlib's and uses the ARMv8 CRC32 instructions.
class Crc32Hasher : public Hasher {
public:
    void update(const std::uint8_t *data, std::size_t size) override {
        crc_ = lzma_crc32(data, size, crc_);
    }

    std::vector<std::uint8_t> finish() override {
        return {static_cast<std::uint8_t>(crc_ >> 24), static_cast<std::uint8_t>(crc_ >> 16),
                static_cast<std::uint8_t>(crc_ >> 8), static_cast<std::uint8_t>(crc_)};
    }

private:
    std::uint32_t crc_ = 0;
};

class MdHasher : public Hasher {
public:
    explicit MdHasher(mbedtls_md_type_t type) {
        mbedtls_md_init(&context_);
        info_ = mbedtls_md_info_from_type(type);
        mbedtls_md_setup(&context_, info_, 0);
        mbedtls_md_starts(&context_);
    }
    ~MdHasher() override { mbedtls_md_free(&context_); }

    void update(const std::uint8_t *data, std::size_t size) override {
        mbedtls_md_update(&context_, data, size);
    }

    std::vector<std::uint8_t> finish() override {
        std::vector<std::uint8_t> digest(mbedtls_md_get_size(info_));
        mbedtls_md_finish(&context_, digest.data());
        return digest;
    }

private:
    mbedtls_md_context_t context_;
    const mbedtls_md_info_t *info_;
};

std::unique_ptr<Hasher> createHasher(ChecksumAlgorithm algorithm) {
    switch (algorithm) {
        case ChecksumAlgorithm::kCrc32:
            return std::make_unique<Crc32Hasher>();
        case ChecksumAlgorithm::kMd5:
            return std::make_unique<MdHasher>(MBEDTLS_MD_MD5);
        case ChecksumAlgorithm::kSha1:
            return std::make_unique<MdHasher>(MBEDTLS_MD_SHA1);
        case ChecksumAlgorithm::kSha256:
            return std::make_unique<MdHasher>(MBEDTLS_MD_SHA256);
        case ChecksumAlgorithm::kSha512:
            return std::make_unique<MdHasher>(MBEDTLS_MD_SHA512);
    }
    return nullptr;
}

}  // namespace

namespace {

class FdInput : public ChecksumInput {
public:
    explicit FdInput(int fd) : fd_(fd) {}
    std::int64_t read(std::uint8_t *buffer, std::size_t size) override {
        const ssize_t length = FILEPAN_RETRY(::read(fd_, buffer, size));
        return length == -1 ? -errno : length;
    }

private:
    int fd_;
};

}  // namespace

ChecksumResult computeChecksums(const std::string &path,
                                const std::vector<ChecksumAlgorithm> &algorithms,
                                ChecksumCallbacks &callbacks) {
    const int fd = FILEPAN_RETRY(open(path.c_str(), O_RDONLY | O_CLOEXEC));
    if (fd == -1) {
        ChecksumResult result;
        result.error = errno;
        return result;
    }
    // Sequential reads; lets the kernel read ahead aggressively.
    posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
    FdInput input(fd);
    auto result = computeChecksums(input, algorithms, callbacks);
    ::close(fd);
    return result;
}

ChecksumResult computeChecksums(ChecksumInput &input,
                                const std::vector<ChecksumAlgorithm> &algorithms,
                                ChecksumCallbacks &callbacks) {
    ChecksumResult result;
    std::vector<std::unique_ptr<Hasher>> hashers;
    for (const auto algorithm : algorithms) {
        hashers.push_back(createHasher(algorithm));
    }

    // Double buffering: hashers work on one buffer while this thread reads into the other. Each
    // round ends at the barrier, where the buffers swap.
    std::unique_ptr<std::uint8_t[]> buffers[2] = {
        std::make_unique<std::uint8_t[]>(kChunkSize), std::make_unique<std::uint8_t[]>(kChunkSize)};
    std::size_t sizes[2] = {0, 0};
    int current = 0;
    bool done = false;
    std::barrier barrier(static_cast<std::ptrdiff_t>(hashers.size() + 1));

    std::vector<std::thread> threads;
    for (auto &hasher : hashers) {
        threads.emplace_back([&, hasher = hasher.get()] {
            while (true) {
                barrier.arrive_and_wait();  // A chunk is ready, or we're done.
                if (done) {
                    return;
                }
                hasher->update(buffers[current].get(), sizes[current]);
                barrier.arrive_and_wait();  // Finished with the chunk.
            }
        });
    }

    auto read = [&](int index) {
        std::size_t filled = 0;
        while (filled < kChunkSize) {
            const std::int64_t length =
                input.read(buffers[index].get() + filled, kChunkSize - filled);
            if (length < 0) {
                return static_cast<int>(-length);
            }
            if (length == 0) {
                break;
            }
            filled += static_cast<std::size_t>(length);
        }
        sizes[index] = filled;
        return 0;
    };

    std::uint64_t pendingProgress = 0;
    auto lastProgress = std::chrono::steady_clock::now();
    result.error = read(current);
    while (!result.error && sizes[current] > 0) {
        barrier.arrive_and_wait();  // Hashers start on the current chunk.
        const int next = 1 - current;
        const int readError = read(next);
        barrier.arrive_and_wait();  // Hashers are done with the current chunk.
        pendingProgress += sizes[current];
        current = next;
        result.error = readError;
        const auto now = std::chrono::steady_clock::now();
        if (now - lastProgress >= kProgressInterval) {
            lastProgress = now;
            if (!callbacks.onProgress(pendingProgress)) {
                result.error = ECANCELED;
            }
            pendingProgress = 0;
        }
    }
    done = true;
    barrier.arrive_and_wait();
    for (auto &thread : threads) {
        thread.join();
    }
    if (!result.error && pendingProgress && !callbacks.onProgress(pendingProgress)) {
        result.error = ECANCELED;
    }
    if (!result.error) {
        for (std::size_t i = 0; i < algorithms.size(); ++i) {
            result.digests[static_cast<std::size_t>(algorithms[i])] = hashers[i]->finish();
        }
    }
    return result;
}

}  // namespace filepan
