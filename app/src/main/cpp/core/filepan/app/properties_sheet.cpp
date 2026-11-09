// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/app/properties_sheet.hpp"

#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <memory>
#include <optional>

#include "filepan/app/app.hpp"
#include "filepan/app/format.hpp"
#include "filepan/app/jobs_ui.hpp"
#include "filepan/app/sheet_parts.hpp"
#include "filepan/cancellation.hpp"
#include "filepan/checksum.hpp"

namespace filepan::app {

using namespace ui;
using namespace ui::components;

std::string formatPermissions(std::uint32_t mode) {
    std::string out(9, '-');
    const char letters[] = "rwxrwxrwx";
    for (int i = 0; i < 9; ++i) {
        if ((mode & (0400u >> i)) != 0) {
            out[i] = letters[i];
        }
    }
    if ((mode & S_ISUID) != 0) {
        out[2] = out[2] == 'x' ? 's' : 'S';
    }
    if ((mode & S_ISGID) != 0) {
        out[5] = out[5] == 'x' ? 's' : 'S';
    }
    if ((mode & S_ISVTX) != 0) {
        out[8] = out[8] == 'x' ? 't' : 'T';
    }
    return out;
}

std::string formatDateTime(vfs::Timestamp time) {
    const std::time_t seconds = static_cast<std::time_t>(time.seconds);
    std::tm local{};
    if (localtime_r(&seconds, &local) == nullptr) {
        return {};
    }
    char clock[8];
    std::snprintf(clock, sizeof(clock), "%02d:%02d", local.tm_hour, local.tm_min);
    return formatDate(time) + ", " + clock;
}

std::string hexDigest(const std::vector<std::uint8_t> &digest) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(digest.size() * 2);
    for (const std::uint8_t byte : digest) {
        out += kHex[byte >> 4];
        out += kHex[byte & 15];
    }
    return out;
}

namespace {

// "1,234,567".
std::string grouped(std::uint64_t value) {
    std::string digits = std::to_string(value);
    for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3) {
        digits.insert(static_cast<std::size_t>(i), ",");
    }
    return digits;
}

std::string printableFolder(const vfs::Location &folder) {
    if (folder.isLocal()) {
        return folder.path();
    }
    if (const vfs::Location *archive = folder.archiveFile()) {
        return printableFolder(*archive) + " › " + folder.path();
    }
    if (folder.kind() == vfs::BackendKind::kDocument) {
        return folder.serverId() + " › " + folder.path();
    }
    return "sftp://" + folder.serverId() + folder.path();
}

std::string userName(std::uint32_t uid) {
    passwd entry{};
    passwd *found = nullptr;
    std::array<char, 1024> buffer{};
    if (getpwuid_r(uid, &entry, buffer.data(), buffer.size(), &found) == 0 && found != nullptr) {
        return std::string(found->pw_name) + " (" + std::to_string(uid) + ")";
    }
    return std::to_string(uid);
}

std::string groupName(std::uint32_t gid) {
    group entry{};
    group *found = nullptr;
    std::array<char, 1024> buffer{};
    if (getgrgid_r(gid, &entry, buffer.data(), buffer.size(), &found) == 0 && found != nullptr) {
        return std::string(found->gr_name) + " (" + std::to_string(gid) + ")";
    }
    return std::to_string(gid);
}

struct Tally {
    std::uint64_t bytes = 0;
    std::uint64_t files = 0;
    std::uint64_t folders = 0;
    bool done = false;

    bool operator==(const Tally &) const = default;
};

// On a worker: walks the trees without following links, reporting every 150 ms and at the end.
void countTrees(vfs::BackendResolver &backends, const std::vector<vfs::Location> &roots, const CancellationToken &cancel,
                const std::function<void(Tally)> &report) {
    Tally tally;
    auto last = std::chrono::steady_clock::now();
    std::vector<vfs::Location> pending;
    for (const vfs::Location &root : roots) {
        vfs::Backend *backend = backends.backendFor(root);
        if (backend == nullptr) {
            continue;
        }
        const auto entry = backend->lstat(root);
        if (!entry) {
            continue;
        }
        if (entry->type == vfs::EntryType::kDirectory) {
            pending.push_back(root);
        } else {
            ++tally.files;
            tally.bytes += entry->type == vfs::EntryType::kFile ? entry->size : 0;
        }
    }
    while (!pending.empty() && !cancel.isCancelled()) {
        const vfs::Location folder = std::move(pending.back());
        pending.pop_back();
        vfs::Backend *backend = backends.backendFor(folder);
        if (backend == nullptr) {
            continue;
        }
        (void)backend->list(folder, {.withStat = true}, [&](std::vector<vfs::Entry> batch) {
            for (const vfs::Entry &child : batch) {
                if (child.type == vfs::EntryType::kDirectory) {
                    ++tally.folders;
                    pending.push_back(folder.child(child.name));
                } else {
                    ++tally.files;
                    tally.bytes += child.type == vfs::EntryType::kFile ? child.size : 0;
                }
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - last > std::chrono::milliseconds(150)) {
                last = now;
                report(tally);
            }
        }, cancel);
    }
    tally.done = !cancel.isCancelled();
    report(tally);
}

constexpr std::array<std::pair<ChecksumAlgorithm, const char *>, 4> kShownDigests = {{
        {ChecksumAlgorithm::kCrc32, "CRC32"},
        {ChecksumAlgorithm::kMd5, "MD5"},
        {ChecksumAlgorithm::kSha1, "SHA-1"},
        {ChecksumAlgorithm::kSha256, "SHA-256"},
}};

struct SheetState {
    CancellationToken counting;
    std::optional<jobs::JobId> checksumJob;
};

}  // namespace

void showProperties(PropertiesHost host, const FileTargets &targets) {
    if (targets.entries.empty()) {
        return;
    }
    runtime::AppServices &services = host.services;
    const bool single = targets.entries.size() == 1;
    const vfs::Entry &first = targets.entries.front();
    const bool tree = !single || first.type == vfs::EntryType::kDirectory;
    auto state = std::make_shared<SheetState>();
    // The line's icon turns into a check; a screen reader hears it.
    const auto copyable = [&services](std::string what, Prop<std::string> value) {
        return [&services, what, value] {
            const std::string text = untrack([&] { return value.get(); });
            if (text.empty()) {
                return;
            }
            services.platform.setClipboardText(text);
            services.platform.announce("Copied " + what);
        };
    };

    auto column = parts::sheetColumn(0);
    column->setDebugName("PropertiesSheet");
    if (single) {
        column->appendChild(parts::fileRow(first, kindName(first)));
    } else {
        std::string names;
        for (std::size_t i = 0; i < targets.entries.size() && i < 3; ++i) {
            names += (i == 0 ? "" : ", ") + targets.entries[i].name;
        }
        if (targets.entries.size() > 3) {
            names += ", +" + std::to_string(targets.entries.size() - 3);
        }
        auto heading = parts::bodyText(formatCount(targets.entries.size()) + " selected");
        column->appendChild(heading);
        column->appendChild(parts::monoText(names, 2, false));
    }

    // --- General ---
    column->appendChild(parts::section("General"));
    if (single) {
        column->appendChild(parts::infoLine({.label = "Name", .value = first.name, .onTap = copyable("name", first.name)}));
    }
    // Shown as the breadcrumb names it; copied as the real path.
    const std::string location = printableFolder(targets.directory);
    const std::string shownLocation = host.locationLabel.empty() ? location : host.locationLabel;
    column->appendChild(
            parts::infoLine({.label = "Location", .value = shownLocation, .onTap = copyable("location", location)}));
    if (single) {
        std::string type = kindName(first);
        if (!first.isDirectoryLike() && first.targetType == vfs::EntryType::kFile) {
            type += " · " + std::string(first.mimeType().type);
        }
        column->appendChild(parts::infoLine({.label = "Type", .value = type}));
    }
    if (tree) {
        Signal<Tally> tally{Tally{}};
        column->appendChild(parts::infoLine({.label = "Size", .value = [tally] {
                                                 const Tally &t = tally();
                                                 const std::string size = formatSize(t.bytes) + " · " + grouped(t.bytes) + " bytes";
                                                 return t.done ? size : "Counting… " + formatSize(t.bytes);
                                             }}));
        column->appendChild(parts::infoLine({.label = "Contains", .value = [tally] {
                                                 const Tally &t = tally();
                                                 return std::to_string(t.files) + (t.files == 1 ? " file, " : " files, ") +
                                                        std::to_string(t.folders) + (t.folders == 1 ? " folder" : " folders");
                                             }}));
        vfs::BackendResolver *backends = &services.backends;
        ui::UiLoop *loop = &services.loop;
        services.io.execute([backends, loop, roots = targets.locations(), cancel = state->counting, tally] {
            countTrees(*backends, roots, cancel, [loop, tally, cancel](Tally t) {
                loop->post([tally, t, cancel] {
                    if (!cancel.isCancelled()) {
                        tally.set(t);
                    }
                });
            });
        });
    } else if (first.hasStat) {
        column->appendChild(parts::infoLine(
                {.label = "Size", .value = formatSize(first.size) + " · " + grouped(first.size) + " bytes"}));
    }
    if (single && first.hasStat) {
        column->appendChild(parts::infoLine({.label = "Modified", .value = formatDateTime(first.modifiedTime)}));
        if (first.hasBirthTime) {
            column->appendChild(parts::infoLine({.label = "Created", .value = formatDateTime(first.birthTime)}));
        }
        if (first.accessTime.seconds != 0) {
            column->appendChild(parts::infoLine({.label = "Accessed", .value = formatDateTime(first.accessTime)}));
        }
    }

    // --- Permissions and links ---
    const bool local = targets.directory.isLocal();
    const bool link = first.type == vfs::EntryType::kSymlink;
    if (single && ((local && first.hasStat) || link)) {
        column->appendChild(parts::section(link && !local ? "Link" : "Access"));
        if (local && first.hasStat) {
            char octal[8];
            std::snprintf(octal, sizeof(octal), "%04o", first.mode & 07777u);
            column->appendChild(parts::infoLine(
                    {.label = "Permissions", .value = formatPermissions(first.mode) + " · " + octal, .mono = true}));
            column->appendChild(parts::infoLine(
                    {.label = "Owner", .value = userName(first.uid) + " · " + groupName(first.gid)}));
        }
        if (link) {
            const std::string target = first.linkTarget + (first.brokenLink ? " (broken)" : "");
            column->appendChild(parts::infoLine(
                    {.label = "Links to", .value = target, .onTap = copyable("link target", first.linkTarget), .mono = true}));
        }
    }

    // --- Checksums ---
    if (single && first.targetType == vfs::EntryType::kFile) {
        column->appendChild(parts::section("Checksums"));
        enum Phase : int { idle, running, done, failed };
        Signal<int> phase{static_cast<int>(idle)};
        Signal<float> fraction{0.0f};
        Signal<std::array<std::string, kChecksumAlgorithmCount>> digests;
        const vfs::Location file = targets.directory.child(first.name);
        JobsModel *checksums = &host.jobs;
        const auto compute = [state, checksums, &services, file, phase, fraction, digests] {
            if (phase.peek() == running) {
                return;
            }
            phase.set(running);
            fraction.set(0);
            std::vector<ChecksumAlgorithm> algorithms;
            for (const auto &[algorithm, name] : kShownDigests) {
                algorithms.push_back(algorithm);
            }
            state->checksumJob = checksums->submit(
                    jobs::ChecksumJob{.file = file, .algorithms = algorithms},
                    {.title = "Checksums",
                     .quiet = true,
                     .onFinished =
                             [state, &services, phase, digests](const jobs::JobProgress &p) {
                                 state->checksumJob.reset();
                                 const auto result = services.jobs.result(p.id);
                                 if (p.state != jobs::JobState::kSucceeded || !result) {
                                     phase.set(p.state == jobs::JobState::kCancelled ? idle : failed);
                                     return;
                                 }
                                 std::array<std::string, kChecksumAlgorithmCount> hex;
                                 for (std::size_t i = 0; i < kChecksumAlgorithmCount; ++i) {
                                     hex[i] = hexDigest(result->checksums[i]);
                                 }
                                 digests.set(hex);
                                 phase.set(done);
                             },
                     .onProgress = [fraction](const jobs::JobProgress &p) { fraction.set(std::max(0.0f, jobFraction(p))); }});
        };
        auto start = Button::create({.label = "Compute checksums", .variant = ButtonVariant::glass, .icon = "fingerprint", .onTap = compute});
        auto progress = Box::create({.gap = 6, .padding = {0, 8, 0, 8}});
        progress->appendChild(parts::progressTrack([fraction] { return fraction(); }));
        auto status = parts::monoText({}, 1);
        status->bind([s = status.get(), fraction] {
            const float f = fraction();
            untrack([&] { s->setText("Hashing · " + std::to_string(static_cast<int>(f * 100)) + "%"); });
        });
        progress->appendChild(status);
        auto results = Box::create();
        for (const auto &[algorithm, name] : kShownDigests) {
            const auto index = static_cast<std::size_t>(algorithm);
            Prop<std::string> value = [digests, index] { return digests()[index]; };
            results->appendChild(parts::infoLine({.label = name, .value = value, .onTap = copyable(name, value), .mono = true}));
        }
        auto failure = parts::bodyText("Couldn't read the file to hash it.");
        column->appendChild(start);
        column->appendChild(progress);
        column->appendChild(results);
        column->appendChild(failure);
        column->bind([phase, start = start.get(), progress = progress.get(), results = results.get(),
                      failure = failure.get()] {
            const int p = phase();
            untrack([&] {
                start->updateStyle([&](Style &s) {
                    s.display = p == idle || p == failed;
                    s.alignSelf = Align::start;
                    s.margin = {0, 6, 0, 0};
                });
                progress->updateStyle([&](Style &s) { s.display = p == running; });
                results->updateStyle([&](Style &s) { s.display = p == done; });
                failure->updateStyle([&](Style &s) { s.display = p == failed; });
            });
        });
    }

    JobsModel *jobsModel = &host.jobs;
    host.overlays.showSheet(single ? "Properties" : "Properties", parts::scrolling(column), true, [state, jobsModel] {
        state->counting.cancel();
        if (state->checksumJob) {
            jobsModel->cancel(*state->checksumJob);
        }
    });
}

}  // namespace filepan::app
