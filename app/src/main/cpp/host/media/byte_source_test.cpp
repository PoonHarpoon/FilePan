// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// fileByteSource: sizes, reads at offsets, EOF, cancelling, and reads from two threads at once.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "filepan/media/byte_source.hpp"
#include "test_util.hpp"

namespace {

using filepan::media::ByteSource;
using filepan::media::fileByteSource;
using filepan::media::MediaSource;

constexpr std::size_t kSize = 10'000;

std::uint8_t byteAt(std::size_t offset) {
    return static_cast<std::uint8_t>(offset * 7 + offset / 251);
}

std::string writeFile(const filepan::test::TempDir &dir) {
    const std::string path = (dir.path() / "data.bin").string();
    std::ofstream out(path, std::ios::binary);
    for (std::size_t i = 0; i < kSize; ++i) {
        out.put(static_cast<char>(byteAt(i)));
    }
    return path;
}

bool matches(const std::vector<std::uint8_t> &buffer, std::size_t offset, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        if (buffer[i] != byteAt(offset + i)) {
            return false;
        }
    }
    return true;
}

void testReads() {
    filepan::test::TempDir dir;
    const std::string path = writeFile(dir);
    std::shared_ptr<ByteSource> source = fileByteSource(path);
    EXPECT(source != nullptr);
    EXPECT(source->size() && *source->size() == kSize);

    std::vector<std::uint8_t> buffer(4096);
    auto read = source->readAt(0, buffer);
    EXPECT(read && *read == 4096 && matches(buffer, 0, 4096));
    read = source->readAt(1234, buffer);
    EXPECT(read && *read == 4096 && matches(buffer, 1234, 4096));
    // Short only at EOF, then nothing.
    read = source->readAt(9000, buffer);
    EXPECT(read && *read == 1000 && matches(buffer, 9000, 1000));
    read = source->readAt(kSize, buffer);
    EXPECT(read && *read == 0);
    read = source->readAt(kSize * 3, buffer);
    EXPECT(read && *read == 0);
    read = source->readAt(5, std::span(buffer).first(0));
    EXPECT(read && *read == 0);
    EXPECT(!source->waiting());

    // Two threads, as the video and audio extractors read.
    bool ok[2] = {true, true};
    std::thread threads[2];
    for (int t = 0; t < 2; ++t) {
        threads[t] = std::thread([&, t] {
            std::vector<std::uint8_t> chunk(333);
            for (std::size_t offset = static_cast<std::size_t>(t) * 17; offset < kSize; offset += 333) {
                auto got = source->readAt(offset, chunk);
                const std::size_t want = std::min<std::size_t>(333, kSize - offset);
                ok[t] = ok[t] && got && *got == want && matches(chunk, offset, want);
            }
        });
    }
    for (std::thread &thread : threads) {
        thread.join();
    }
    EXPECT(ok[0] && ok[1]);

    // Cancelled: reads fail from then on.
    source->cancel();
    EXPECT(!source->readAt(0, buffer));

    EXPECT(fileByteSource((dir.path() / "missing").string()) == nullptr);
}

void testMediaSource() {
    const MediaSource file = MediaSource::file("/a.mp4");
    EXPECT(file.path == "/a.mp4" && !file.bytes);
    filepan::test::TempDir dir;
    auto bytes = fileByteSource(writeFile(dir));
    const MediaSource stream = MediaSource::stream(bytes);
    EXPECT(stream.path.empty() && stream.bytes == bytes);
}

}  // namespace

int main() {
    testReads();
    testMediaSource();
    std::printf("byte_source_test passed\n");
    return 0;
}
