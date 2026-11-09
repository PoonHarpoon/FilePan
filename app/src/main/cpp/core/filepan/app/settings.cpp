// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/app/settings.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>

#include "filepan/app/format.hpp"
#include "filepan/app/secrets.hpp"
#include "filepan/app/user_items.hpp"
#include "filepan/eintr.hpp"

namespace filepan::app {

namespace {

constexpr std::string_view kHeader = "filepan-settings";

const char *presetName(ui::ThemePreset preset) {
    switch (preset) {
    case ui::ThemePreset::ultraviolet:
        return "ultraviolet";
    case ui::ThemePreset::clean:
        return "clean";
    case ui::ThemePreset::acid:
        return "acid";
    }
    return "acid";
}

std::optional<ui::ThemePreset> presetFromName(std::string_view name) {
    if (name == "ultraviolet") return ui::ThemePreset::ultraviolet;
    if (name == "acid") return ui::ThemePreset::acid;
    if (name == "clean") return ui::ThemePreset::clean;
    // Clean was called Basic at first.
    if (name == "basic") return ui::ThemePreset::clean;
    // Crossfader was dropped: too noisy on a small screen. Its users get Ultraviolet, its nearest.
    if (name == "crossfader") return ui::ThemePreset::ultraviolet;
    return std::nullopt;
}

const char *sortKeyName(vfs::SortKey key) {
    switch (key) {
    case vfs::SortKey::kName:
        return "name";
    case vfs::SortKey::kSize:
        return "size";
    case vfs::SortKey::kModified:
        return "modified";
    case vfs::SortKey::kType:
        return "type";
    }
    return "name";
}

std::optional<vfs::SortKey> sortKeyFromName(std::string_view name) {
    if (name == "name") return vfs::SortKey::kName;
    if (name == "size") return vfs::SortKey::kSize;
    if (name == "modified") return vfs::SortKey::kModified;
    if (name == "type") return vfs::SortKey::kType;
    return std::nullopt;
}

std::vector<std::string_view> splitFields(std::string_view line) {
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    while (start < line.size()) {
        const std::size_t end = line.find(' ', start);
        const std::size_t stop = end == std::string_view::npos ? line.size() : end;
        if (stop > start) {
            fields.push_back(line.substr(start, stop - start));
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return fields;
}

std::optional<long long> parseInt(std::string_view text) {
    long long value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

// An empty string as a field: "%" alone can't occur in an encoding, so "-" stands for empty and
// a literal "-" is escaped.
std::string field(std::string_view value) {
    if (value.empty()) {
        return "-";
    }
    if (value == "-") {
        return "%2D";
    }
    return percentEncode(value);
}

std::optional<std::string> unfield(std::string_view token) {
    if (token == "-") {
        return std::string();
    }
    return percentDecode(token);
}

// The endpoint fields, in a fixed order: host port user auth password key passphrase; auth is
// "password", "key" or "card". The password, key and passphrase are always written empty: they go
// in "secret" lines, sealed. (The key field once held a path to a key file; that's read as nothing
// now.) For "card", the key field holds the card's public key, which isn't secret.
void writeEndpoint(std::string &out, const vfs::SftpEndpoint &endpoint) {
    out += ' ' + field(endpoint.host);
    out += ' ' + std::to_string(endpoint.port);
    out += ' ' + field(endpoint.username);
    switch (endpoint.authentication.type) {
    case vfs::SftpAuthentication::Type::kPassword: out += " password - - -"; break;
    case vfs::SftpAuthentication::Type::kPrivateKey: out += " key - - -"; break;
    case vfs::SftpAuthentication::Type::kOpenPgpCard:
        out += " card - " + field(endpoint.authentication.cardPublicKey) + " -";
        break;
    }
}

// "secret <server id> <hop> <password|key|passphrase> <sealed>": hop 0 is the server, n the nth
// jump host.
void writeSecrets(std::string &out, const std::string &id, std::size_t hop, const vfs::SftpEndpoint &endpoint,
                  SecretCodec *secrets) {
    if (secrets == nullptr) {
        return;
    }
    const auto write = [&](const char *kind, const std::string &secret) {
        if (secret.empty()) {
            return;
        }
        if (auto sealed = secrets->protect(secret)) {
            out += "secret " + field(id) + ' ' + std::to_string(hop) + ' ' + kind + ' ' + field(*sealed) + '\n';
        }
    };
    write("password", endpoint.authentication.password);
    if (endpoint.authentication.type == vfs::SftpAuthentication::Type::kPrivateKey) {
        write("key", endpoint.authentication.privateKey);
    }
    write("passphrase", endpoint.authentication.passphrase);
}

constexpr std::size_t kEndpointFields = 7;

std::optional<vfs::SftpEndpoint> readEndpoint(const std::vector<std::string_view> &fields, std::size_t at) {
    if (at + kEndpointFields > fields.size()) {
        return std::nullopt;
    }
    vfs::SftpEndpoint endpoint;
    auto host = unfield(fields[at]);
    auto port = parseInt(fields[at + 1]);
    auto user = unfield(fields[at + 2]);
    const std::string_view auth = fields[at + 3];
    auto password = unfield(fields[at + 4]);
    auto key = unfield(fields[at + 5]);
    auto passphrase = unfield(fields[at + 6]);
    if (!host || !port || *port <= 0 || *port > 65535 || !user || !password || !key || !passphrase ||
        (auth != "key" && auth != "password" && auth != "card")) {
        return std::nullopt;
    }
    endpoint.host = *host;
    endpoint.port = static_cast<int>(*port);
    endpoint.username = *user;
    endpoint.authentication.type = auth == "key"    ? vfs::SftpAuthentication::Type::kPrivateKey
                                   : auth == "card" ? vfs::SftpAuthentication::Type::kOpenPgpCard
                                                    : vfs::SftpAuthentication::Type::kPassword;
    if (auth == "card") {
        endpoint.authentication.cardPublicKey = *key;
    }
    endpoint.authentication.password = *password;
    endpoint.authentication.passphrase = *passphrase;
    return endpoint;
}

bool sameEndpoint(const vfs::SftpEndpoint &a, const vfs::SftpEndpoint &b) {
    return a.host == b.host && a.port == b.port && a.username == b.username &&
           a.authentication.type == b.authentication.type && a.authentication.password == b.authentication.password &&
           a.authentication.privateKey == b.authentication.privateKey &&
           a.authentication.passphrase == b.authentication.passphrase &&
           a.authentication.cardPublicKey == b.authentication.cardPublicKey;
}

}  // namespace

bool sameServer(const vfs::SftpServerConfig &a, const vfs::SftpServerConfig &b) {
    if (a.id != b.id || a.name != b.name || !sameEndpoint(a.server, b.server) || a.knownHostsPath != b.knownHostsPath ||
        a.timeoutSeconds != b.timeoutSeconds || a.jumpHosts.size() != b.jumpHosts.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.jumpHosts.size(); ++i) {
        if (!sameEndpoint(a.jumpHosts[i], b.jumpHosts[i])) {
            return false;
        }
    }
    return true;
}

namespace {

Result<void> writeAll(int fd, std::string_view data) {
    while (!data.empty()) {
        const ssize_t written = FILEPAN_RETRY(::write(fd, data.data(), data.size()));
        if (written < 0) {
            return fail(errno, "write");
        }
        data.remove_prefix(static_cast<std::size_t>(written));
    }
    return {};
}

}  // namespace

bool Settings::operator==(const Settings &other) const {
    if (preset != other.preset || accent != other.accent || sort != other.sort || view != other.view ||
        showHidden != other.showHidden || thumbnails != other.thumbnails ||
        thumbnailCacheSize != other.thumbnailCacheSize || thumbnailDecayDays != other.thumbnailDecayDays ||
        remoteThumbnails != other.remoteThumbnails || remoteThumbnailMaxSize != other.remoteThumbnailMaxSize ||
        viewerPrefetch != other.viewerPrefetch || viewerPrefetchVideoMb != other.viewerPrefetchVideoMb ||
        remoteConnections != other.remoteConnections || dataSaver != other.dataSaver || onboarded != other.onboarded ||
        cardPinLetters != other.cardPinLetters ||
        tabs != other.tabs || activeTab != other.activeTab ||
        favorites != other.favorites || homeTarget != other.homeTarget || hiddenItems != other.hiddenItems || documentTrees != other.documentTrees ||
        diskUsage != other.diskUsage || trees != other.trees || servers.size() != other.servers.size()) {
        return false;
    }
    for (std::size_t i = 0; i < servers.size(); ++i) {
        if (!sameServer(servers[i], other.servers[i])) {
            return false;
        }
    }
    return true;
}

std::uint64_t thumbnailCacheBytes(const Settings &settings) {
    return static_cast<std::uint64_t>(settings.thumbnailCacheSize) * kThumbnailCacheUnit;
}

std::int64_t thumbnailDecaySeconds(const Settings &settings) {
    return static_cast<std::int64_t>(settings.thumbnailDecayDays) * 86400;
}

std::uint64_t remoteThumbnailMaxBytes(const Settings &settings) {
    return static_cast<std::uint64_t>(settings.remoteThumbnailMaxSize) * kThumbnailCacheUnit;
}

std::string colorToHex(gfx::Color color) {
    const auto channel = [](float v) {
        return static_cast<unsigned>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255));
    };
    char buffer[8];
    std::snprintf(buffer, sizeof buffer, "#%02X%02X%02X", channel(color.r), channel(color.g), channel(color.b));
    return buffer;
}

std::optional<gfx::Color> colorFromHex(std::string_view hex) {
    if (hex.size() != 7 || hex[0] != '#') {
        return std::nullopt;
    }
    std::uint32_t rgb = 0;
    const auto [end, error] = std::from_chars(hex.data() + 1, hex.data() + 7, rgb, 16);
    if (error != std::errc() || end != hex.data() + 7) {
        return std::nullopt;
    }
    return gfx::Color::fromRgb(rgb);
}

std::string serializeSettings(const Settings &settings, SecretCodec *secrets) {
    std::string out;
    out += kHeader;
    out += ' ' + std::to_string(kSettingsVersion) + '\n';
    out += "theme " + std::string(presetName(settings.preset)) + '\n';
    out += "accent " + (settings.accent ? colorToHex(*settings.accent) : std::string("-")) + '\n';
    out += "sort " + std::string(sortKeyName(settings.sort.key)) + (settings.sort.descending ? " desc" : " asc") +
           (settings.sort.directoriesFirst ? " 1" : " 0") + '\n';
    out += std::string("view ") + (settings.view == ViewMode::grid ? "grid" : "list") + '\n';
    out += std::string("hidden ") + (settings.showHidden ? "1" : "0") + '\n';
    out += std::string("thumbnails ") + (settings.thumbnails ? "1" : "0") + '\n';
    out += "thumbnail-cache " + std::to_string(settings.thumbnailCacheSize) + '\n';
    out += "thumbnail-decay " + std::to_string(settings.thumbnailDecayDays) + '\n';
    out += std::string("remote-thumbnails ") + (settings.remoteThumbnails ? "1" : "0") + '\n';
    out += "remote-thumbnail-max " + std::to_string(settings.remoteThumbnailMaxSize) + '\n';
    out += "viewer-prefetch " + std::to_string(settings.viewerPrefetch) + '\n';
    out += "viewer-prefetch-video " + std::to_string(settings.viewerPrefetchVideoMb) + '\n';
    out += std::string("data-saver ") + (settings.dataSaver ? "1" : "0") + '\n';
    out += std::string("card-pin-letters ") + (settings.cardPinLetters ? "1" : "0") + '\n';
    out += "onboarded " + std::to_string(settings.onboarded) + '\n';
    out += "remote-connections " + std::to_string(settings.remoteConnections) + '\n';
    out += std::string("document-trees ") + (settings.documentTrees ? "1" : "0") + '\n';
    out += std::string("disk-usage ") + (settings.diskUsage ? "1" : "0") + '\n';
    for (const DocumentTree &tree : settings.trees) {
        out += "tree " + field(tree.id) + ' ' + field(tree.uri) + '\n';
    }
    for (const SavedTab &tab : settings.tabs) {
        out += "tab";
        for (const vfs::Location &location : tab.history) {
            out += ' ' + percentEncode(encodeLocation(location));
        }
        out += '\n';
    }
    out += "active-tab " + std::to_string(settings.activeTab) + '\n';
    writeUserItems(out, settings);
    for (const vfs::SftpServerConfig &server : settings.servers) {
        out += "server " + field(server.id);
        writeEndpoint(out, server.server);
        out += ' ' + field(server.knownHostsPath);
        out += ' ' + std::to_string(server.timeoutSeconds);
        for (const vfs::SftpEndpoint &jump : server.jumpHosts) {
            out += " jump";
            writeEndpoint(out, jump);
        }
        out += '\n';
        if (!server.name.empty()) {
            out += "server-name " + field(server.id) + ' ' + field(server.name) + '\n';
        }
        writeSecrets(out, server.id, 0, server.server, secrets);
        for (std::size_t i = 0; i < server.jumpHosts.size(); ++i) {
            writeSecrets(out, server.id, i + 1, server.jumpHosts[i], secrets);
        }
    }
    return out;
}

Result<Settings> parseSettings(std::string_view text, SecretCodec *secrets, bool *plaintextSecrets) {
    Settings settings;
    if (plaintextSecrets != nullptr) {
        *plaintextSecrets = false;
    }
    const auto findServer = [&](std::string_view id) -> vfs::SftpServerConfig * {
        for (auto &server : settings.servers) {
            if (server.id == id) {
                return &server;
            }
        }
        return nullptr;
    };
    bool sawHeader = false;
    std::optional<long long> active;
    while (!text.empty()) {
        const std::size_t newline = text.find('\n');
        std::string_view line = text.substr(0, newline);
        text = newline == std::string_view::npos ? std::string_view() : text.substr(newline + 1);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        const std::vector<std::string_view> f = splitFields(line);
        if (f.empty()) {
            continue;
        }
        if (!sawHeader) {
            const auto version = f.size() == 2 ? parseInt(f[1]) : std::nullopt;
            if (f[0] != kHeader || !version || *version < 1) {
                return fail(EINVAL, "not a settings file");
            }
            sawHeader = true;
            // A file from before the intro: its user has been here.
            settings.onboarded = 1;
            continue;
        }
        const std::string_view key = f[0];
        if (key == "theme" && f.size() == 2) {
            if (auto preset = presetFromName(f[1])) {
                settings.preset = *preset;
            }
        } else if (key == "accent" && f.size() == 2) {
            settings.accent = f[1] == "-" ? std::nullopt : colorFromHex(f[1]);
        } else if (key == "sort" && f.size() == 4) {
            if (auto sortKey = sortKeyFromName(f[1])) {
                settings.sort = {*sortKey, f[2] == "desc", f[3] == "1"};
            }
        } else if (key == "view" && f.size() == 2) {
            settings.view = f[1] == "grid" ? ViewMode::grid : ViewMode::list;
        } else if (key == "hidden" && f.size() == 2) {
            settings.showHidden = f[1] == "1";
        } else if (key == "thumbnails" && f.size() == 2) {
            settings.thumbnails = f[1] == "1";
        } else if (key == "thumbnail-cache" && f.size() == 2) {
            if (auto size = parseInt(f[1]); size && *size > 0 && *size <= (1 << 20)) {
                settings.thumbnailCacheSize = static_cast<std::uint32_t>(*size);
            }
        } else if (key == "thumbnail-decay" && f.size() == 2) {
            if (auto days = parseInt(f[1]); days && *days >= 0 && *days <= 3650) {
                settings.thumbnailDecayDays = static_cast<std::uint32_t>(*days);
            }
        } else if (key == "remote-thumbnails" && f.size() == 2) {
            settings.remoteThumbnails = f[1] == "1";
        } else if (key == "data-saver" && f.size() == 2) {
            settings.dataSaver = f[1] == "1";
        } else if (key == "card-pin-letters" && f.size() == 2) {
            settings.cardPinLetters = f[1] == "1";
        } else if (key == "onboarded" && f.size() == 2) {
            if (auto version = parseInt(f[1]); version && *version >= 0 && *version <= 1000) {
                settings.onboarded = static_cast<std::uint32_t>(*version);
            }
        } else if (key == "remote-thumbnail-max" && f.size() == 2) {
            if (auto size = parseInt(f[1]); size && *size > 0 && *size <= (1 << 20)) {
                settings.remoteThumbnailMaxSize = static_cast<std::uint32_t>(*size);
            }
        } else if (key == "viewer-prefetch" && f.size() == 2) {
            if (auto count = parseInt(f[1]); count && *count >= 0 && *count <= 10) {
                settings.viewerPrefetch = static_cast<std::uint32_t>(*count);
            }
        } else if (key == "viewer-prefetch-video" && f.size() == 2) {
            if (auto size = parseInt(f[1]); size && *size >= 0 && *size <= 1024) {
                settings.viewerPrefetchVideoMb = static_cast<std::uint32_t>(*size);
            }
        } else if (key == "remote-connections" && f.size() == 2) {
            if (auto count = parseInt(f[1]); count && *count >= 1 && *count <= 6) {
                settings.remoteConnections = static_cast<std::uint32_t>(*count);
            }
        } else if (key == "document-trees" && f.size() == 2) {
            settings.documentTrees = f[1] == "1";
        } else if (key == "disk-usage" && f.size() == 2) {
            settings.diskUsage = f[1] == "1";
        } else if (key == "tree" && f.size() == 3) {
            auto id = unfield(f[1]);
            auto uri = unfield(f[2]);
            if (id && uri && !id->empty() && !uri->empty() &&
                std::ranges::none_of(settings.trees, [&](const DocumentTree &tree) { return tree.id == *id; })) {
                settings.trees.push_back({std::move(*id), std::move(*uri)});
            }
        } else if (key == "server-name" && f.size() == 3) {
            auto id = unfield(f[1]);
            auto name = unfield(f[2]);
            if (auto *server = id ? findServer(*id) : nullptr; server && name) {
                server->name = *name;
            }
        } else if (key == "secret" && f.size() == 5) {
            auto id = unfield(f[1]);
            auto hop = parseInt(f[2]);
            auto sealed = unfield(f[4]);
            auto *server = id ? findServer(*id) : nullptr;
            if (server == nullptr || !hop || *hop < 0 || static_cast<std::size_t>(*hop) > server->jumpHosts.size() ||
                !sealed || secrets == nullptr || (f[3] != "password" && f[3] != "key" && f[3] != "passphrase")) {
                continue;
            }
            auto secret = secrets->unprotect(*sealed);
            if (!secret) {
                continue;  // Sealed by a key that's gone: it'll be asked for (a private key, re-entered).
            }
            vfs::SftpAuthentication &auth =
                    (*hop == 0 ? server->server : server->jumpHosts[static_cast<std::size_t>(*hop - 1)]).authentication;
            (f[3] == "password" ? auth.password : f[3] == "key" ? auth.privateKey : auth.passphrase) = *secret;
        } else if (key == "tab") {
            SavedTab tab;
            bool ok = true;
            for (std::size_t i = 1; i < f.size() && ok; ++i) {
                auto decoded = percentDecode(f[i]);
                auto location = decoded ? decodeLocation(*decoded) : std::nullopt;
                ok = location.has_value();
                if (ok) {
                    tab.history.push_back(*location);
                }
            }
            if (ok) {
                settings.tabs.push_back(std::move(tab));
            }
        } else if (key == "active-tab" && f.size() == 2) {
            active = parseInt(f[1]);
        } else if (readUserItem(f, settings)) {
            // A favorite (or an older bookmark): user_items.cpp.
        } else if (key == "server" && f.size() >= 2 + kEndpointFields + 2) {
            vfs::SftpServerConfig server;
            auto id = unfield(f[1]);
            auto endpoint = readEndpoint(f, 2);
            const std::size_t after = 2 + kEndpointFields;
            auto knownHosts = unfield(f[after]);
            auto timeout = parseInt(f[after + 1]);
            if (!id || id->empty() || !endpoint || !knownHosts || !timeout) {
                continue;
            }
            server.id = *id;
            server.server = *endpoint;
            server.knownHostsPath = *knownHosts;
            server.timeoutSeconds = static_cast<int>(*timeout);
            bool ok = true;
            for (std::size_t i = after + 2; i < f.size() && ok;) {
                auto jump = f[i] == "jump" ? readEndpoint(f, i + 1) : std::nullopt;
                ok = jump.has_value();
                if (ok) {
                    server.jumpHosts.push_back(*jump);
                    i += 1 + kEndpointFields;
                }
            }
            if (ok) {
                const auto plain = [](const vfs::SftpEndpoint &e) {
                    return !e.authentication.password.empty() || !e.authentication.passphrase.empty();
                };
                if (plaintextSecrets != nullptr &&
                    (plain(server.server) || std::ranges::any_of(server.jumpHosts, plain))) {
                    *plaintextSecrets = true;
                }
                settings.servers.push_back(std::move(server));
            }
        }
    }
    if (!sawHeader) {
        return fail(EINVAL, "empty settings file");
    }
    if (active && *active >= 0 && static_cast<std::size_t>(*active) < settings.tabs.size()) {
        settings.activeTab = static_cast<std::size_t>(*active);
    }
    return settings;
}

SettingsStore::SettingsStore(std::string path) : path_(std::move(path)) {}

Settings SettingsStore::load(SecretCodec *secrets, bool *plaintextSecrets) const {
    if (plaintextSecrets != nullptr) {
        *plaintextSecrets = false;
    }
    if (path_.empty()) {
        return {};
    }
    const int fd = FILEPAN_RETRY(::open(path_.c_str(), O_RDONLY | O_CLOEXEC));
    if (fd < 0) {
        return {};
    }
    std::string text;
    char buffer[16384];
    while (true) {
        const ssize_t got = FILEPAN_RETRY(::read(fd, buffer, sizeof buffer));
        if (got <= 0) {
            break;
        }
        text.append(buffer, static_cast<std::size_t>(got));
        if (text.size() > (8u << 20)) {
            break;  // Not ours.
        }
    }
    ::close(fd);
    auto parsed = parseSettings(text, secrets, plaintextSecrets);
    return parsed ? std::move(*parsed) : Settings{};
}

std::uint64_t SettingsStore::nextGeneration() {
    std::lock_guard lock(mutex_);
    return ++generation_;
}

Result<void> SettingsStore::save(const std::string &serialized, std::uint64_t generation) {
    if (path_.empty()) {
        return {};
    }
    // One writer at a time, and never an older generation after a newer one.
    std::lock_guard lock(mutex_);
    if (generation <= written_) {
        return {};
    }
    const std::string temporary = path_ + ".tmp";
    const int fd = FILEPAN_RETRY(::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600));
    if (fd < 0) {
        return fail(errno, "open " + temporary);
    }
    auto written = writeAll(fd, serialized);
    if (written && ::fsync(fd) != 0) {
        written = fail(errno, "fsync");
    }
    if (::close(fd) != 0 && written) {
        written = fail(errno, "close");
    }
    if (!written) {
        ::unlink(temporary.c_str());
        return written;
    }
    if (::rename(temporary.c_str(), path_.c_str()) != 0) {
        const int error = errno;
        ::unlink(temporary.c_str());
        return fail(error, "rename");
    }
    written_ = generation;
    return {};
}

}  // namespace filepan::app
