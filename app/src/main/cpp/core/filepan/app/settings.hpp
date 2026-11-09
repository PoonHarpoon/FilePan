// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "filepan/app/user_items.hpp"
#include "filepan/error.hpp"
#include "filepan/gfx/geometry.hpp"
#include "filepan/ui/theme.hpp"
#include "filepan/vfs/location.hpp"
#include "filepan/vfs/sftp_backend.hpp"
#include "filepan/vfs/sort.hpp"

namespace filepan::app {
class SecretCodec;
}

// FilePan's persisted settings and the file they live in.
//
// The file is a small line-based text format of our own, versioned by its first line
// ("filepan-settings 1"). Each following line is a key and space-separated, percent-encoded
// values; unknown keys are skipped, so an older build reads a newer file's known parts. It's
// written atomically: to a temporary file, synced, then renamed over the old one.
//
// SFTP passwords, private keys and passphrases are never written as entered: each goes in a
// "secret" line, sealed by a SecretCodec (secrets.hpp). Without a codec they're left out. Files
// from before that kept passwords and passphrases in the "server" lines; they're read (and
// reported, so the app rewrites the file at once) but never written that way again. Older files
// named a key file by its path instead of holding the key; that path is ignored.
namespace filepan::app {

enum class ViewMode : std::uint8_t { list, grid };

struct SortSettings {
    vfs::SortKey key = vfs::SortKey::kName;
    bool descending = false;
    bool directoriesFirst = true;

    bool operator==(const SortSettings &) const = default;
};

// A folder granted through the system's picker (a document tree): kDocument locations are rooted
// at its id, which is also the name it shows under.
struct DocumentTree {
    std::string id;
    std::string uri;  // As the platform granted it.

    bool operator==(const DocumentTree &) const = default;
};

// A tab as it's restored at launch: the folders of its back stack, oldest first. Empty means
// it was on the storage home.
struct SavedTab {
    std::vector<vfs::Location> history;

    bool operator==(const SavedTab &) const = default;
};

struct Settings {
    ui::ThemePreset preset = ui::ThemePreset::acid;
    std::optional<gfx::Color> accent;
    SortSettings sort;
    ViewMode view = ViewMode::list;
    bool showHidden = false;
    // Image thumbnails in lists and grids.
    bool thumbnails = true;
    // The cap on the thumbnails of local files kept on disk, in kThumbnailCacheUnit.
    std::uint32_t thumbnailCacheSize = 1024;
    // Thumbnails kept on disk that weren't viewed for this many days are removed; 0 keeps them.
    std::uint32_t thumbnailDecayDays = 3;
    // Image thumbnails on SFTP servers, never kept on disk past the session. On by default: they
    // cost a fetch per image shown, capped by the size below, and nothing is kept.
    bool remoteThumbnails = true;
    // The largest remote file fetched for a thumbnail, in kThumbnailCacheUnit.
    std::uint32_t remoteThumbnailMaxSize = 10;
    // The media viewer fetches this many files each side of the one shown from an SFTP server
    // ahead of time, so that swiping to them needn't wait.
    std::uint32_t viewerPrefetch = 2;
    // How much of each such video it fetches from the start, in MB (and its last 1 MB, where the
    // index may be); 0 fetches none.
    std::uint32_t viewerPrefetchVideoMb = 8;
    // Connections per server those fetches run over at once, besides the one for browsing.
    std::uint32_t remoteConnections = 3;
    // Mobile data mode: on a metered network, remote thumbnails, viewer prefetch and folder item
    // counts on servers pause (AppContext::dataSaving).
    bool dataSaver = false;
    // An OpenPGP card or security key PIN is typed with letters, not the number pad, by default.
    bool cardPinLetters = false;
    // The intro (onboarding_screen.hpp) seen, as the kOnboardingVersion it was; it shows while this
    // is lower. A fresh install has 0; a settings file from before the intro reads as 1.
    std::uint32_t onboarded = 0;
    std::vector<SavedTab> tabs;
    std::size_t activeTab = 0;
    // Favorites and hidden items: user_items.hpp.
    std::vector<Favorite> favorites;
    // Where the top bar's home button goes: the favorite at this location; unset, the storage home.
    std::optional<vfs::Location> homeTarget;
    std::vector<vfs::Location> hiddenItems;
    std::vector<vfs::SftpServerConfig> servers;
    // Experimental: document trees (system storage through the picker) on the storage home.
    bool documentTrees = false;
    // Experimental, opt out: the disk usage screen in the overflow menus, in builds with the adirstat
    // module.
    bool diskUsage = true;
    // The trees granted on this device. Backups leave them out: grants don't move with them.
    std::vector<DocumentTree> trees;

    // SftpServerConfig has no comparison of its own; this compares every field.
    bool operator==(const Settings &other) const;
};

inline constexpr int kSettingsVersion = 1;
// Bumped when the intro gains something everyone should see.
inline constexpr std::uint32_t kOnboardingVersion = 1;

// Settings::thumbnailCacheSize's unit, and the sizes and decay times the settings screen offers.
inline constexpr std::uint64_t kThumbnailCacheUnit = 1ull << 20;
inline constexpr const char *kThumbnailCacheUnitName = "MiB";
inline constexpr std::uint32_t kThumbnailCacheSizes[] = {128, 256, 512, 1024, 2048};
inline constexpr std::uint32_t kThumbnailDecayChoices[] = {1, 3, 7, 30, 0};
inline constexpr std::uint32_t kRemoteThumbnailMaxSizes[] = {2, 5, 10, 25};
// The viewer's prefetching: files each side, MiB of each video, and connections per server.
inline constexpr std::uint32_t kViewerPrefetchChoices[] = {0, 1, 2, 3, 5};
inline constexpr std::uint32_t kViewerPrefetchVideoChoices[] = {0, 4, 8, 16, 32};
inline constexpr std::uint32_t kRemoteConnectionChoices[] = {1, 2, 3, 4, 5, 6};

// The disk cap in bytes and the decay time in seconds that the settings give, and the largest
// remote file in bytes.
std::uint64_t thumbnailCacheBytes(const Settings &settings);
std::int64_t thumbnailDecaySeconds(const Settings &settings);
std::uint64_t remoteThumbnailMaxBytes(const Settings &settings);

// secrets seals the SFTP passwords, private keys and passphrases; without one they're left out.
std::string serializeSettings(const Settings &settings, SecretCodec *secrets = nullptr);
// Fails with EINVAL if the header is missing or names a version this build can't read at all.
// Malformed lines are skipped, and so are secrets that secrets (or its absence) can't open.
// plaintextSecrets, if given, is set when the file kept a secret as entered (an older file).
Result<Settings> parseSettings(std::string_view text, SecretCodec *secrets = nullptr,
                               bool *plaintextSecrets = nullptr);

// Whether two server configs are the same in every field.
bool sameServer(const vfs::SftpServerConfig &a, const vfs::SftpServerConfig &b);

// Reads and writes the settings file. save() may run on any thread; saves that finish out of
// order never replace a newer file with an older one.
class SettingsStore {
public:
    // An empty path keeps nothing: load() gives the defaults and save() does nothing.
    explicit SettingsStore(std::string path);

    [[nodiscard]] const std::string &path() const { return path_; }
    // The defaults if the file doesn't exist or can't be read. See parseSettings.
    [[nodiscard]] Settings load(SecretCodec *secrets = nullptr, bool *plaintextSecrets = nullptr) const;
    // Writes atomically. generation orders saves: a save older than the newest one written is
    // dropped. Thread-safe.
    Result<void> save(const std::string &serialized, std::uint64_t generation);
    Result<void> save(const Settings &settings) { return save(serializeSettings(settings), nextGeneration()); }
    std::uint64_t nextGeneration();

private:
    std::string path_;
    std::mutex mutex_;
    std::uint64_t generation_ = 0;
    std::uint64_t written_ = 0;
};

// "#RRGGBB" for a color's opaque sRGB, and back.
std::string colorToHex(gfx::Color color);
std::optional<gfx::Color> colorFromHex(std::string_view hex);

}  // namespace filepan::app
