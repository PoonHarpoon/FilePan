// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "filepan/app/app.hpp"
#include "filepan/app/screen_parts.hpp"
#include "filepan/runtime/document_picker.hpp"
#include "filepan/sftp_client.hpp"
#include "filepan/vfs/sftp_backend.hpp"

// The SFTP servers: the saved list ("sftp-servers") and the editor ("sftp-server").
//
//   List    A glass card per server: its name, user@host:port, the jump hosts and an ID tag.
//           Tapping one connects (sheets ask for host keys and passwords as needed) and opens
//           its home directory in the current tab; its menu edits or deletes it.
//   Editor  Name, host, port and user; password or private key login, the key read from a key
//           file (the system picker) or pasted, and checked as it's given; jump hosts to connect
//           through, in order; a connection test on the I/O pool; save and delete.
//
// ServerSync pushes the saved servers to the SFTP backend as they change.
namespace filepan::app {

inline constexpr const char *kServersScreen = "sftp-servers";
inline constexpr const char *kServerEditorScreen = "sftp-server";
// The editor on a new server, whatever the list last opened (the intro's "Add an SFTP server").
inline constexpr const char *kNewServerScreen = "sftp-server-new";

// Both screens, sharing which server the editor opens.
std::vector<ScreenSpec> sftpScreenSpecs();

// --- The editor's model ---

struct EndpointDraft {
    std::string host;
    // Empty: 22.
    std::string port;
    std::string username;
    bool privateKey = false;
    // Logs in with an OpenPGP card's authentication key; privateKey is false then.
    bool card = false;
    std::string password;
    // The private key to save, as OpenSSH or PEM text: one saved before, or a key file's.
    std::string key;
    // Where key came from, to show: the file's name, or empty for a saved key.
    std::string keySource;
    // The paste field's text. When it isn't blank, it's the key instead (see keyText).
    std::string pastedKey;
    // Why the last key file picked wasn't taken; cleared by the next pick or paste.
    std::string pickError;
    std::string passphrase;
    // The card's public key, as an OpenSSH line (CardKey::publicKey), once read.
    std::string cardKey;
    // Why the last reading didn't work; cleared by the next.
    std::string cardError;
    bool readingCard = false;

    // The key that'll be saved: the pasted one, or else key.
    [[nodiscard]] const std::string &keyText() const;
};

struct ServerDraft {
    std::string name;
    EndpointDraft server;
    std::vector<EndpointDraft> jumps;
};

struct EndpointErrors {
    std::string host;
    std::string port;
    std::string username;
    std::string key;

    [[nodiscard]] bool empty() const { return host.empty() && port.empty() && username.empty() && key.empty(); }
    bool operator==(const EndpointErrors &) const = default;
};

struct DraftErrors {
    std::string name;
    EndpointErrors server;
    std::vector<EndpointErrors> jumps;

    [[nodiscard]] bool empty() const;
    bool operator==(const DraftErrors &) const = default;
};

ServerDraft draftFrom(const vfs::SftpServerConfig &config);
// Checks every field; a private key is parsed with libssh (parsePrivateKey), its error shown.
DraftErrors validate(const ServerDraft &draft);
// parsePrivateKey, remembering the last few answers: validate runs on every keystroke once errors
// show. The answer is valid until the next call. UI thread only.
const std::expected<SshPrivateKey, std::string> &checkKey(const std::string &text);
// The config a valid draft saves as, under id.
vfs::SftpServerConfig configFrom(const ServerDraft &draft, const std::string &id, const std::string &knownHostsPath);
// A new id from the name ("Home NAS" -> "home-nas"), unique among existing.
std::string newServerId(const std::string &name, const std::vector<vfs::SftpServerConfig> &existing);
// "user@host:port", the port left out when it's 22.
std::string endpointLine(const vfs::SftpEndpoint &endpoint);
// Where trusted host keys are kept: <filesDir>/known_hosts, or "" (this session only).
std::string knownHostsPath(const AppContext &context);

// Connects to a saved server on the I/O pool and opens its home directory in the current tab;
// says why in a snackbar if it can't.
void openServer(AppContext &context, const std::string &id);

// Tells the SFTP backend about added, changed and removed servers, and nothing else: setServer
// drops the connection and the credentials entered, so unchanged servers are left alone.
class ServerSync {
public:
    explicit ServerSync(vfs::SftpBackend &sftp) : sftp_(sftp) {}
    void apply(const std::vector<vfs::SftpServerConfig> &servers);

private:
    vfs::SftpBackend &sftp_;
    std::vector<vfs::SftpServerConfig> pushed_;
};

// The screens' nodes, for tests.
class ServersScreen;
class ServerEditor;

class ServersScreen final : public ui::Node {
public:
    explicit ServersScreen(AppContext &context) : context_(context) {}
    void construct(std::shared_ptr<std::optional<std::string>> editing);

    [[nodiscard]] const std::vector<ui::Node *> &cards() const { return cards_; }
    [[nodiscard]] ui::Node *addButton() const { return add_; }
    // What a card's menu does.
    void edit(const std::string &id);
    void remove(const std::string &id);
    void add();

private:
    void rebuild();

    AppContext &context_;
    ScrollPage page_;
    std::shared_ptr<std::optional<std::string>> editing_;
    std::vector<ui::Node *> cards_;
    ui::Node *add_ = nullptr;
};

class ServerEditor final : public ui::Node {
public:
    explicit ServerEditor(AppContext &context) : context_(context) {}
    // editing: the server's id, or nullopt for a new one.
    void construct(std::optional<std::string> editing);

    [[nodiscard]] ServerDraft &draft() { return *draft_; }
    [[nodiscard]] const ui::Signal<DraftErrors> &errors() const { return errors_; }
    [[nodiscard]] bool isNew() const { return !editing_.has_value(); }
    // The editor's fields, by spoken label ("Host", "Jump host 1 host"), for tests.
    [[nodiscard]] ui::components::TextField *field(const std::string &label) const;
    [[nodiscard]] ui::Node *saveButton() const { return save_; }
    [[nodiscard]] ui::Node *testButton() const { return test_; }

    // Validates and saves, then goes back to the list; false (showing the errors) if invalid.
    bool save();
    void testConnection();
    void addJump();
    void removeJump(std::size_t index);
    void setPrivateKey(std::optional<std::size_t> jump, bool privateKey);
    enum class Login : std::uint8_t { password, privateKey, card };
    void setLogin(std::optional<std::size_t> jump, Login login);
    // Reads the authentication key from the OpenPGP card the user presents, on the I/O pool, for
    // the endpoint to log in with; what went wrong shows in its cardError.
    void readCard(std::optional<std::size_t> jump);
    // What reading gave, for an endpoint (as readCard's answer, and for tests).
    void cardRead(std::optional<std::size_t> jump, const Result<vfs::CardKey> &key);
    // Opens the system picker for a key file; a valid key read from it becomes the endpoint's
    // key, and anything else says why in pickError.
    void pickKey(std::optional<std::size_t> jump);
    // What the picker gave, for an endpoint (as pickKey's answer, and for tests).
    void keyPicked(std::optional<std::size_t> jump, const runtime::PickedDocument &document);
    // Deletes the server being edited and goes back.
    void deleteServer();

    // The last test's outcome: empty while none ran.
    enum class TestState : std::uint8_t { idle, running, passed, failed };
    ui::Signal<TestState> testState{TestState::idle};
    ui::Signal<std::string> testMessage;

private:
    void rebuild();
    ui::NodePtr endpointFields(EndpointDraft &endpoint, std::optional<std::size_t> jump);
    ui::NodePtr keyFields(EndpointDraft &endpoint, std::optional<std::size_t> jump, const std::string &prefix);
    ui::NodePtr cardFields(EndpointDraft &endpoint, std::optional<std::size_t> jump, const std::string &prefix);
    EndpointDraft *endpoint(std::optional<std::size_t> jump);
    void revalidate();

    AppContext &context_;
    // Bumped when a key changes, for what shows it.
    ui::Signal<std::uint64_t> keyRevision_{0};
    std::optional<std::string> editing_;
    std::shared_ptr<ServerDraft> draft_ = std::make_shared<ServerDraft>();
    ui::Signal<DraftErrors> errors_;
    bool showErrors_ = false;
    ScrollPage page_;
    std::vector<std::pair<std::string, ui::components::TextField *>> fields_;
    ui::Node *save_ = nullptr;
    ui::Node *test_ = nullptr;
    std::uint64_t testGeneration_ = 0;
};

}  // namespace filepan::app
