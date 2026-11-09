// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/app/remote_media.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <string_view>
#include <utility>

namespace filepan::app {

namespace {

constexpr std::size_t kOwnBudget = 16 << 20;

media::MediaError toMediaError(const Error &error) {
    switch (error.code) {
        case ENOTCONN:
        case ECONNRESET:
        case ECONNABORTED:
        case EPIPE:
        case ETIMEDOUT:
        case ENETDOWN:
        case ENETUNREACH:
        case EHOSTUNREACH:
            return {"Lost the connection to the server"};
        case ECONNREFUSED:
            return {error.message.empty() ? std::string("Couldn't connect to the server")
                                          : "Couldn't connect to the server: " + error.message};
        case ECANCELED:
            return {"Cancelled"};
        default:
            return {"Couldn't read the file: " + (error.message.empty() ? std::string(std::strerror(error.code))
                                                                         : error.message)};
    }
}

media::MediaError cancelledError() { return {"Cancelled"}; }

// An unlinked file in directory, or -1.
int openSpill(const std::string &directory) {
    if (::mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST) {
        return -1;
    }
    int fd = ::open(directory.c_str(), O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
    if (fd >= 0) {
        return fd;
    }
    // A file system without O_TMPFILE: create and unlink at once.
    std::string name = directory + "/spill-XXXXXX";
    fd = ::mkostemp(name.data(), O_CLOEXEC);
    if (fd >= 0) {
        ::unlink(name.c_str());
    }
    return fd;
}

}  // namespace

namespace detail {

// What a RemoteByteSource and its worker share; the worker keeps it alive until it stops, so the
// source can go without waiting for a fetch to finish.
struct RemoteSourceState : std::enable_shared_from_this<RemoteSourceState> {
    RemoteSourceState(vfs::BackendResolver &backends, vfs::Location location, RemoteByteSourceOptions options)
        : backends(backends),
          location(std::move(location)),
          spillDirectory(std::move(options.spillDirectory)),
          budget(options.budget ? std::move(options.budget) : std::make_shared<RemoteMemoryBudget>(kOwnBudget)) {}

    ~RemoteSourceState() {
        budget->release(memoryBytes.load());
        if (spillFd >= 0) {
            ::close(spillFd);
        }
    }

    struct Block {
        std::vector<std::uint8_t> data;
        std::uint64_t used = 0;  // When it was last read, by clock.
    };
    struct Prefetch {
        std::uint64_t id = 0;
        std::vector<ByteRange> ranges;
        std::optional<std::vector<std::uint64_t>> blocks;  // Once the size is known.
        std::size_t next = 0;
        std::function<void(bool)> done;
    };

    vfs::BackendResolver &backends;
    const vfs::Location location;
    const std::string spillDirectory;
    const std::shared_ptr<RemoteMemoryBudget> budget;

    // Guards what's below, but for the stream. Taken after streamMutex when both are.
    mutable std::mutex mutex;
    // Blocks arriving, cancellation, and work for the worker.
    std::condition_variable changed;
    bool cancelled = false;
    bool active = true;
    std::optional<std::uint64_t> size;
    std::unordered_map<std::uint64_t, Block> memory;
    std::vector<bool> onDisk;
    std::unordered_set<std::uint64_t> inFlight;
    std::uint64_t clock = 0;
    int spillFd = -1;
    bool spillFailed = false;
    // Read-ahead: the last block a read missed, and whether reads go in sequence.
    std::optional<std::uint64_t> lastMiss;
    bool sequential = false;
    std::deque<std::uint64_t> ahead;
    // The first is the one being fetched.
    std::deque<Prefetch> prefetches;
    std::uint64_t nextPrefetch = 1;
    bool workerRunning = false;

    std::atomic<std::size_t> memoryBytes{0};
    std::atomic<std::size_t> distance{0};
    std::atomic<int> waiters{0};

    // The stream, opened on first use and closed once everything is in.
    std::mutex streamMutex;
    std::unique_ptr<vfs::InputStream> stream;
    std::atomic<bool> streamIsOpen{false};

    [[nodiscard]] std::size_t blockCount() const {
        return size ? static_cast<std::size_t>((*size + RemoteByteSource::kBlockSize - 1) / RemoteByteSource::kBlockSize)
                    : 0;
    }
    [[nodiscard]] std::size_t blockLength(std::uint64_t block) const {
        const std::uint64_t start = block * RemoteByteSource::kBlockSize;
        return static_cast<std::size_t>(std::min<std::uint64_t>(RemoteByteSource::kBlockSize, *size - start));
    }
    [[nodiscard]] bool present(std::uint64_t block) const {
        return memory.contains(block) || (block < onDisk.size() && onDisk[block]);
    }
    [[nodiscard]] bool complete() const {
        for (std::uint64_t b = 0; b < blockCount(); ++b) {
            if (!present(b)) {
                return false;
            }
        }
        return true;
    }

    // With streamMutex held.
    media::MediaResult<void> openStream() {
        if (stream) {
            return {};
        }
        {
            std::lock_guard lock(mutex);
            if (cancelled) {
                return std::unexpected(cancelledError());
            }
        }
        vfs::Backend *backend = backends.backendFor(location);
        if (backend == nullptr) {
            return std::unexpected(media::MediaError{"The server isn't set up any more"});
        }
        auto opened = backend->openReadParallel(location);
        if (!opened) {
            return std::unexpected(toMediaError(opened.error()));
        }
        auto length = (*opened)->size();
        if (!length) {
            return std::unexpected(toMediaError(length.error()));
        }
        stream = std::move(*opened);
        streamIsOpen = true;
        std::lock_guard lock(mutex);
        if (!size) {
            size = *length;
            onDisk.assign(blockCount(), false);
        }
        return {};
    }


    media::MediaResult<std::uint64_t> knownSize() {
        {
            std::lock_guard lock(mutex);
            if (cancelled) {
                return std::unexpected(cancelledError());
            }
            if (size) {
                return *size;
            }
        }
        std::lock_guard streamLock(streamMutex);
        {
            // Another thread may have opened it meanwhile (and closed it again, all fetched).
            std::lock_guard lock(mutex);
            if (size) {
                return *size;
            }
        }
        if (auto opened = openStream(); !opened) {
            return std::unexpected(opened.error());
        }
        std::lock_guard lock(mutex);
        return *size;
    }

    // Reads a block from the stream (on the calling thread, which isn't the UI thread).
    media::MediaResult<std::vector<std::uint8_t>> fetch(std::uint64_t block) {
        std::lock_guard streamLock(streamMutex);
        if (auto opened = openStream(); !opened) {
            return std::unexpected(opened.error());
        }
        std::size_t length;
        {
            std::lock_guard lock(mutex);
            if (cancelled) {
                return std::unexpected(cancelledError());
            }
            length = blockLength(block);
        }
        if (auto sought = stream->seek(static_cast<std::int64_t>(block * RemoteByteSource::kBlockSize),
                                       vfs::SeekOrigin::kBegin);
            !sought) {
            return std::unexpected(toMediaError(sought.error()));
        }
        std::vector<std::uint8_t> data(length);
        std::size_t done = 0;
        while (done < length) {
            auto read = stream->read(std::span(data).subspan(done));
            if (!read) {
                return std::unexpected(toMediaError(read.error()));
            }
            if (*read == 0) {
                break;  // Shorter than it was.
            }
            done += *read;
        }
        data.resize(done);
        return data;
    }

    // With mutex held. The bytes added to memory.
    std::size_t store(std::uint64_t block, std::vector<std::uint8_t> data) {
        if (cancelled || memory.contains(block)) {
            return 0;
        }
        const std::size_t bytes = data.size();
        memory[block] = {std::move(data), ++clock};
        memoryBytes += bytes;
        return bytes;
    }

    void charge(std::size_t bytes) {
        if (bytes > 0) {
            budget->add(bytes);
            budget->enforce();
        }
    }

    // Moves the least recently read block to disk (or drops it). The bytes freed.
    std::size_t evictOne() {
        std::size_t freed = 0;
        {
            std::lock_guard lock(mutex);
            auto victim = memory.end();
            for (auto it = memory.begin(); it != memory.end(); ++it) {
                if (victim == memory.end() || it->second.used < victim->second.used) {
                    victim = it;
                }
            }
            if (victim == memory.end()) {
                return 0;
            }
            if (spillFd < 0 && !spillFailed && !spillDirectory.empty()) {
                spillFd = openSpill(spillDirectory);
                spillFailed = spillFd < 0;
            }
            const std::uint64_t block = victim->first;
            const std::vector<std::uint8_t> &data = victim->second.data;
            if (spillFd >= 0 && block < onDisk.size()) {
                std::size_t written = 0;
                while (written < data.size()) {
                    const ssize_t n = ::pwrite(spillFd, data.data() + written, data.size() - written,
                                               static_cast<off_t>(block * RemoteByteSource::kBlockSize + written));
                    if (n < 0 && errno == EINTR) {
                        continue;
                    }
                    if (n <= 0) {
                        break;  // A full disk: it's fetched again if needed.
                    }
                    written += static_cast<std::size_t>(n);
                }
                onDisk[block] = written == data.size();
            }
            freed = data.size();
            memory.erase(victim);
            memoryBytes -= freed;
        }
        budget->release(freed);
        return freed;
    }

    // With mutex held: reads from the spill file; false if it can't.
    bool readSpilled(std::uint64_t block, std::size_t within, std::span<std::uint8_t> out) {
        std::size_t done = 0;
        while (done < out.size()) {
            const ssize_t n =
                    ::pread(spillFd, out.data() + done, out.size() - done,
                            static_cast<off_t>(block * RemoteByteSource::kBlockSize + within + done));
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n <= 0) {
                return false;
            }
            done += static_cast<std::size_t>(n);
        }
        return true;
    }

    // With mutex held: notes a read of block for read-ahead.
    void noteRead(std::uint64_t block, bool missed) {
        if (missed) {
            // Two misses in a row: reads go in sequence. A miss elsewhere is a seek.
            sequential = lastMiss && *lastMiss + 1 == block;
            lastMiss = block;
        }
        if (!sequential || !active) {
            return;
        }
        const std::uint64_t count = blockCount();
        for (std::uint64_t b = block + 1; b <= block + RemoteByteSource::kReadAhead && b < count; ++b) {
            if (!present(b) && !inFlight.contains(b) && std::ranges::find(ahead, b) == ahead.end()) {
                ahead.push_back(b);
            }
        }
        if (!ahead.empty()) {
            startWorker();
        }
    }

    // With mutex held.
    void startWorker() {
        if (workerRunning || cancelled) {
            return;
        }
        workerRunning = true;
        std::thread([self = shared_from_this()] { self->work(); }).detach();
        changed.notify_all();
    }

    media::MediaResult<std::size_t> readAt(std::uint64_t offset, std::span<std::uint8_t> buffer,
                                           const std::atomic<bool> *viewCancelled, std::atomic<int> *viewWaiting) {
        auto total = knownSize();
        if (!total) {
            return std::unexpected(total.error());
        }
        if (offset >= *total || buffer.empty()) {
            return 0;
        }
        const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), *total - offset));
        std::size_t done = 0;
        while (done < n) {
            const std::uint64_t at = offset + done;
            const std::uint64_t block = at / RemoteByteSource::kBlockSize;
            const std::size_t within = static_cast<std::size_t>(at % RemoteByteSource::kBlockSize);
            const std::size_t take = std::min(RemoteByteSource::kBlockSize - within, n - done);
            if (auto read = readBlock(block, within, buffer.subspan(done, take), viewCancelled, viewWaiting); !read) {
                return std::unexpected(read.error());
            }
            done += take;
        }
        return n;
    }

    media::MediaResult<void> readBlock(std::uint64_t block, std::size_t within, std::span<std::uint8_t> out,
                                       const std::atomic<bool> *viewCancelled, std::atomic<int> *viewWaiting) {
        const auto wait = [&](int delta) {
            waiters += delta;
            if (viewWaiting != nullptr) {
                *viewWaiting += delta;
            }
        };
        std::unique_lock lock(mutex);
        while (true) {
            if (cancelled || (viewCancelled != nullptr && viewCancelled->load())) {
                return std::unexpected(cancelledError());
            }
            if (auto it = memory.find(block); it != memory.end()) {
                const std::size_t available = it->second.data.size() > within ? it->second.data.size() - within : 0;
                const std::size_t copied = std::min(available, out.size());
                std::copy_n(it->second.data.begin() + static_cast<std::ptrdiff_t>(within), copied, out.begin());
                std::fill(out.begin() + static_cast<std::ptrdiff_t>(copied), out.end(), 0);
                it->second.used = ++clock;
                noteRead(block, false);
                return {};
            }
            if (block < onDisk.size() && onDisk[block]) {
                if (readSpilled(block, within, out)) {
                    noteRead(block, false);
                    return {};
                }
                onDisk[block] = false;
                continue;
            }
            if (inFlight.contains(block)) {
                // Another reader or the worker is fetching it.
                wait(1);
                changed.wait(lock);
                wait(-1);
                continue;
            }
            break;
        }
        inFlight.insert(block);
        lock.unlock();
        wait(1);
        auto data = fetch(block);
        wait(-1);
        lock.lock();
        inFlight.erase(block);
        changed.notify_all();
        if (!data) {
            return std::unexpected(data.error());
        }
        const std::size_t available = data->size() > within ? data->size() - within : 0;
        const std::size_t copied = std::min(available, out.size());
        std::copy_n(data->begin() + static_cast<std::ptrdiff_t>(within), copied, out.begin());
        std::fill(out.begin() + static_cast<std::ptrdiff_t>(copied), out.end(), 0);
        const std::size_t added = store(block, std::move(*data));
        noteRead(block, true);
        lock.unlock();
        charge(added);
        return {};
    }

    // The worker: read-ahead first while active, then prefetches. It stops when there's nothing
    // to do and is started again for more.
    void work() {
        std::unique_lock lock(mutex);
        while (!cancelled) {
            if (!active) {
                ahead.clear();
            }
            std::optional<std::uint64_t> block;
            std::uint64_t prefetch = 0;
            if (!ahead.empty()) {
                block = ahead.front();
                ahead.pop_front();
            } else if (!prefetches.empty()) {
                Prefetch &job = prefetches.front();
                prefetch = job.id;
                if (!job.blocks) {
                    lock.unlock();
                    auto total = knownSize();
                    lock.lock();
                    if (!total) {
                        finishPrefetch(lock, prefetch, false);
                        continue;
                    }
                    if (prefetches.empty() || prefetches.front().id != prefetch) {
                        continue;  // Stopped meanwhile.
                    }
                    resolve(prefetches.front());
                }
                Prefetch &resolved = prefetches.front();
                if (resolved.next == resolved.blocks->size()) {
                    finishPrefetch(lock, prefetch, true);
                    continue;
                }
                block = (*resolved.blocks)[resolved.next++];
            } else {
                break;
            }
            if (present(*block)) {
                continue;
            }
            if (inFlight.contains(*block)) {
                // A reader has it; a prefetch waits for it to be in.
                changed.wait(lock, [&] { return cancelled || !inFlight.contains(*block); });
                if (prefetch != 0 && !prefetches.empty() && prefetches.front().id == prefetch && !present(*block)) {
                    --prefetches.front().next;  // It failed there: try here.
                }
                continue;
            }
            inFlight.insert(*block);
            lock.unlock();
            auto data = fetch(*block);
            lock.lock();
            inFlight.erase(*block);
            changed.notify_all();
            if (!data) {
                if (prefetch != 0) {
                    finishPrefetch(lock, prefetch, false);
                } else {
                    ahead.clear();
                }
                continue;
            }
            const std::size_t added = store(*block, std::move(*data));
            lock.unlock();
            charge(added);
            lock.lock();
        }
        workerRunning = false;
        if (cancelled) {
            auto dropped = std::move(prefetches);
            prefetches.clear();
            lock.unlock();
            for (auto &job : dropped) {
                if (job.done) {
                    job.done(false);
                }
            }
        }
    }

    // With mutex held: the blocks job's ranges cover, in order.
    void resolve(Prefetch &job) const {
        std::vector<std::uint64_t> blocks;
        for (const ByteRange &range : job.ranges) {
            const std::uint64_t start =
                    range.fromEnd ? (*size > range.length ? *size - range.length : 0) : std::min(range.offset, *size);
            const std::uint64_t end = std::min(*size, start + std::min(range.length, *size - start));
            for (std::uint64_t b = start / RemoteByteSource::kBlockSize; b * RemoteByteSource::kBlockSize < end; ++b) {
                if (std::ranges::find(blocks, b) == blocks.end()) {
                    blocks.push_back(b);
                }
            }
        }
        job.blocks = std::move(blocks);
        job.next = 0;
    }

    // With mutex held through lock: ends the prefetch with id if it's still queued, and closes
    // the stream once everything is in.
    void finishPrefetch(std::unique_lock<std::mutex> &lock, std::uint64_t id, bool ok) {
        auto it = std::ranges::find_if(prefetches, [&](const Prefetch &job) { return job.id == id; });
        if (it == prefetches.end()) {
            return;
        }
        auto done = std::move(it->done);
        prefetches.erase(it);
        const bool close = ok && size && complete();
        lock.unlock();
        if (close) {
            std::lock_guard streamLock(streamMutex);
            stream.reset();
            streamIsOpen = false;
        }
        if (done) {
            done(ok);
        }
        lock.lock();
    }

    void wake() {
        std::lock_guard lock(mutex);
        changed.notify_all();
    }
};

}  // namespace detail

// --- RemoteMemoryBudget ---

void RemoteMemoryBudget::attach(const std::shared_ptr<detail::RemoteSourceState> &source) {
    std::lock_guard lock(mutex_);
    std::erase_if(sources_, [](const auto &weak) { return weak.expired(); });
    sources_.push_back(source);
}

void RemoteMemoryBudget::enforce() {
    // One at a time, or two would both evict for the same excess.
    std::lock_guard enforcing(enforcing_);
    while (used_.load() > limit_) {
        std::shared_ptr<detail::RemoteSourceState> victim;
        {
            std::lock_guard lock(mutex_);
            std::erase_if(sources_, [](const auto &weak) { return weak.expired(); });
            for (const auto &weak : sources_) {
                auto source = weak.lock();
                if (source && source->memoryBytes.load() > 0 &&
                    (!victim || source->distance.load() > victim->distance.load())) {
                    victim = std::move(source);
                }
            }
        }
        if (!victim || victim->evictOne() == 0) {
            return;
        }
    }
}

// --- RemoteByteSource ---

class RemoteByteSource::View final : public media::ByteSource {
public:
    explicit View(std::shared_ptr<RemoteByteSource> source) : source_(std::move(source)) {}

    media::MediaResult<std::uint64_t> size() override {
        if (cancelled_) {
            return std::unexpected(cancelledError());
        }
        return source_->size();
    }
    media::MediaResult<std::size_t> readAt(std::uint64_t offset, std::span<std::uint8_t> buffer) override {
        return source_->state_->readAt(offset, buffer, &cancelled_, &waiting_);
    }
    void cancel() override {
        cancelled_ = true;
        source_->state_->wake();
    }
    void setActive(bool active) override { source_->setActive(active); }
    [[nodiscard]] bool waiting() const override { return waiting_.load() > 0; }

private:
    std::shared_ptr<RemoteByteSource> source_;
    std::atomic<bool> cancelled_{false};
    std::atomic<int> waiting_{0};
};

std::shared_ptr<RemoteByteSource> RemoteByteSource::create(vfs::BackendResolver &backends, vfs::Location location,
                                                           RemoteByteSourceOptions options) {
    auto state = std::make_shared<detail::RemoteSourceState>(backends, std::move(location), std::move(options));
    state->budget->attach(state);
    return std::shared_ptr<RemoteByteSource>(new RemoteByteSource(std::move(state)));
}

RemoteByteSource::~RemoteByteSource() { cancel(); }

media::MediaResult<std::uint64_t> RemoteByteSource::size() { return state_->knownSize(); }

media::MediaResult<std::size_t> RemoteByteSource::readAt(std::uint64_t offset, std::span<std::uint8_t> buffer) {
    return state_->readAt(offset, buffer, nullptr, nullptr);
}

void RemoteByteSource::cancel() {
    std::deque<detail::RemoteSourceState::Prefetch> dropped;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->cancelled) {
            return;
        }
        state_->cancelled = true;
        state_->ahead.clear();
        if (!state_->workerRunning) {
            dropped = std::move(state_->prefetches);
            state_->prefetches.clear();
        }
        state_->changed.notify_all();
    }
    for (auto &job : dropped) {
        if (job.done) {
            job.done(false);
        }
    }
}

void RemoteByteSource::setActive(bool active) {
    std::lock_guard lock(state_->mutex);
    state_->active = active;
    if (!active) {
        state_->ahead.clear();
    }
}

bool RemoteByteSource::waiting() const { return state_->waiters.load() > 0; }

std::shared_ptr<media::ByteSource> RemoteByteSource::view() { return std::make_shared<View>(shared_from_this()); }

void RemoteByteSource::prefetch(std::vector<ByteRange> ranges, std::function<void(bool ok)> done) {
    {
        std::lock_guard lock(state_->mutex);
        if (!state_->cancelled) {
            state_->prefetches.push_back({.id = state_->nextPrefetch++, .ranges = std::move(ranges), .done = std::move(done)});
            state_->startWorker();
            return;
        }
    }
    if (done) {
        done(false);
    }
}

void RemoteByteSource::stopPrefetch() {
    std::deque<detail::RemoteSourceState::Prefetch> dropped;
    {
        std::lock_guard lock(state_->mutex);
        dropped = std::move(state_->prefetches);
        state_->prefetches.clear();
    }
    for (auto &job : dropped) {
        if (job.done) {
            job.done(false);
        }
    }
}

void RemoteByteSource::setDistance(std::size_t distance) { state_->distance = distance; }

std::size_t RemoteByteSource::blocksInMemory() const {
    std::lock_guard lock(state_->mutex);
    return state_->memory.size();
}

std::size_t RemoteByteSource::blocksOnDisk() const {
    std::lock_guard lock(state_->mutex);
    return static_cast<std::size_t>(std::ranges::count(state_->onDisk, true));
}

bool RemoteByteSource::streamOpen() const { return state_->streamIsOpen.load(); }

// --- RemoteMediaCache ---

namespace detail {

// Shared with the prefetches' callbacks, which come on the sources' workers.
struct RemoteCacheState : std::enable_shared_from_this<RemoteCacheState> {
    RemoteCacheState(vfs::BackendResolver &backends, std::vector<RemoteMediaItem> items,
                     RemoteMediaCacheOptions options)
        : backends(backends),
          items(std::move(items)),
          options(std::move(options)),
          budget(std::make_shared<RemoteMemoryBudget>(this->options.memoryBytes)) {}

    struct Entry {
        std::shared_ptr<RemoteByteSource> source;
        enum class State : std::uint8_t { idle, running, done } state = State::idle;
        std::uint64_t job = 0;
    };

    vfs::BackendResolver &backends;
    const std::vector<RemoteMediaItem> items;
    const RemoteMediaCacheOptions options;
    const std::shared_ptr<RemoteMemoryBudget> budget;

    mutable std::mutex mutex;
    std::size_t index = 0;
    bool closed = false;
    bool paused = false;
    std::map<std::size_t, Entry> entries;
    std::size_t running = 0;
    std::uint64_t nextJob = 1;
    std::vector<std::size_t> started;

    [[nodiscard]] std::size_t distance(std::size_t i) const { return i > index ? i - index : index - i; }

    // With mutex held: items[i]'s entry, made if need be; null for a local item.
    Entry *entry(std::size_t i) {
        if (i >= items.size() || items[i].location.isLocal()) {
            return nullptr;
        }
        auto [it, made] = entries.try_emplace(i);
        if (made) {
            it->second.source = RemoteByteSource::create(
                    backends, items[i].location, {.spillDirectory = options.spillDirectory, .budget = budget});
            it->second.source->setDistance(distance(i));
        }
        return &it->second;
    }

    [[nodiscard]] std::vector<ByteRange> ranges(const RemoteMediaItem &item) const {
        if (item.video) {
            if (paused || options.videoHeadBytes == 0) {
                return {};
            }
            return {{.offset = 0, .length = options.videoHeadBytes},
                    {.length = options.videoTailBytes, .fromEnd = true}};
        }
        if (item.size > options.maxImageBytes) {
            return {};
        }
        return {{.offset = 0, .length = UINT64_MAX}};
    }

    // Starts the next prefetches in order, while there's room.
    void schedule() {
        struct Start {
            std::shared_ptr<RemoteByteSource> source;
            std::vector<ByteRange> ranges;
            std::size_t item;
            std::uint64_t job;
        };
        std::vector<Start> starts;
        {
            std::lock_guard lock(mutex);
            if (closed) {
                return;
            }
            // index, index + 1, index - 1, index + 2...
            const std::size_t steps = paused ? 0 : 2 * options.around;
            for (std::size_t step = 0; step <= steps && running < options.concurrency; ++step) {
                const std::size_t offset = (step + 1) / 2;
                if (step % 2 == 0 ? offset > index : index + offset >= items.size()) {
                    continue;
                }
                const std::size_t i = step % 2 == 0 ? index - offset : index + offset;
                Entry *e = entry(i);
                if (e == nullptr || e->state != Entry::State::idle) {
                    continue;
                }
                auto wanted = ranges(items[i]);
                if (wanted.empty()) {
                    e->state = Entry::State::done;
                    continue;
                }
                e->state = Entry::State::running;
                e->job = nextJob++;
                ++running;
                started.push_back(i);
                starts.push_back({e->source, std::move(wanted), i, e->job});
            }
        }
        for (Start &start : starts) {
            start.source->prefetch(std::move(start.ranges),
                                   [weak = weak_from_this(), item = start.item, job = start.job](bool) {
                                       if (auto self = weak.lock()) {
                                           self->finished(item, job);
                                       }
                                   });
        }
    }

    void finished(std::size_t item, std::uint64_t job) {
        {
            std::lock_guard lock(mutex);
            --running;
            if (auto it = entries.find(item); it != entries.end() && it->second.job == job) {
                // Failed ones aren't tried again: the page's own reads say what's wrong.
                it->second.state = Entry::State::done;
            }
        }
        schedule();
    }
};

}  // namespace detail

RemoteMediaCache::RemoteMediaCache(vfs::BackendResolver &backends, std::vector<RemoteMediaItem> items,
                                   RemoteMediaCacheOptions options)
    : state_(std::make_shared<detail::RemoteCacheState>(backends, std::move(items), std::move(options))) {}

RemoteMediaCache::~RemoteMediaCache() {
    std::vector<std::shared_ptr<RemoteByteSource>> sources;
    {
        std::lock_guard lock(state_->mutex);
        state_->closed = true;
        for (auto &[i, entry] : state_->entries) {
            sources.push_back(std::move(entry.source));
        }
        state_->entries.clear();
    }
    for (auto &source : sources) {
        source->stopPrefetch();
    }
}

void RemoteMediaCache::setIndex(std::size_t index) {
    std::vector<std::shared_ptr<RemoteByteSource>> dropped;
    {
        std::lock_guard lock(state_->mutex);
        state_->index = index;
        for (auto it = state_->entries.begin(); it != state_->entries.end();) {
            const std::size_t distance = state_->distance(it->first);
            if (distance > state_->options.around + 1) {
                dropped.push_back(std::move(it->second.source));
                it = state_->entries.erase(it);
            } else {
                it->second.source->setDistance(distance);
                ++it;
            }
        }
    }
    // Cancelled as they go, unless a player still has one.
    for (auto &source : dropped) {
        source->stopPrefetch();
    }
    dropped.clear();
    state_->schedule();
}

void RemoteMediaCache::setPaused(bool paused) {
    std::vector<std::shared_ptr<RemoteByteSource>> stopped;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->paused == paused) {
            return;
        }
        state_->paused = paused;
        if (paused) {
            // Running prefetches of other items stop; idle again, they start over once resumed. A
            // new job number makes their finishing leave them idle.
            for (auto &[i, entry] : state_->entries) {
                if (i != state_->index && entry.state != detail::RemoteCacheState::Entry::State::done) {
                    if (entry.state == detail::RemoteCacheState::Entry::State::running) {
                        stopped.push_back(entry.source);
                    }
                    entry.state = detail::RemoteCacheState::Entry::State::idle;
                    entry.job = state_->nextJob++;
                }
            }
        }
    }
    for (auto &source : stopped) {
        source->stopPrefetch();
    }
    state_->schedule();
}

std::shared_ptr<media::ByteSource> RemoteMediaCache::open(std::size_t index) {
    std::lock_guard lock(state_->mutex);
    auto *entry = state_->entry(index);
    return entry != nullptr ? entry->source->view() : nullptr;
}

std::shared_ptr<RemoteByteSource> RemoteMediaCache::source(std::size_t index) const {
    std::lock_guard lock(state_->mutex);
    const auto it = state_->entries.find(index);
    return it != state_->entries.end() ? it->second.source : nullptr;
}

std::vector<std::size_t> RemoteMediaCache::started() const {
    std::lock_guard lock(state_->mutex);
    return state_->started;
}

std::size_t RemoteMediaCache::running() const {
    std::lock_guard lock(state_->mutex);
    return state_->running;
}

bool RemoteMediaCache::prefetched(std::size_t index) const {
    std::lock_guard lock(state_->mutex);
    const auto it = state_->entries.find(index);
    return it != state_->entries.end() &&
           it->second.state == detail::RemoteCacheState::Entry::State::done;
}

const RemoteMemoryBudget &RemoteMediaCache::budget() const { return *state_->budget; }

void clearRemoteSpill(const std::string &directory) {
    DIR *dir = ::opendir(directory.c_str());
    if (dir == nullptr) {
        return;
    }
    while (const dirent *entry = ::readdir(dir)) {
        const std::string_view name = entry->d_name;
        if (name != "." && name != "..") {
            ::unlinkat(::dirfd(dir), entry->d_name, 0);
        }
    }
    ::closedir(dir);
}

}  // namespace filepan::app
