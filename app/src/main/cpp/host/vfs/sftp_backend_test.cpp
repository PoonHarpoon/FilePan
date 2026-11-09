// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// The SFTP backend against a real OpenSSH sshd on localhost, whose paths are the local ones.

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <archive.h>
#include <libssh/libssh.h>
#include <libssh/sk_api.h>
#include <libssh/sftp.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "filepan/jobs/job_queue.hpp"
#include "filepan/vfs/archive_backend.hpp"
#include "filepan/vfs/local_backend.hpp"
#include "filepan/vfs/save.hpp"
#include "filepan/vfs/sftp_backend.hpp"
#include "jobs/job_test_util.hpp"
#include "software_authenticator.hpp"
#include "software_openpgp_card.hpp"
#include "sshd_harness.hpp"
#include "test_util.hpp"

namespace fs = std::filesystem;
using namespace filepan::vfs;
using filepan::CancellationToken;
using filepan::DecisionPromise;
using filepan::SftpError;
using filepan::test::CardLink;
using filepan::test::CardOptions;
using filepan::test::AuthenticatorLink;
using filepan::test::enrollSshKey;
using filepan::test::SoftwareAuthenticator;
using filepan::test::SoftwareOpenPgpCard;
using filepan::test::Sshd;
using filepan::test::TempDir;

namespace {

std::string randomContent(std::size_t size, unsigned seed) {
    std::string content(size, '\0');
    std::mt19937 random(seed);
    for (auto &c : content) {
        c = static_cast<char>(random());
    }
    return content;
}

class ScriptedPrompts : public SftpPromptProvider {
public:
    void requestHostKey(HostKeyPrompt prompt, DecisionPromise<HostKeyDecision> promise) override {
        std::lock_guard lock(mutex);
        hostKeys.push_back(prompt);
        const auto decision = hostKeyAnswers.empty() ? HostKeyDecision::kReject
                                                     : hostKeyAnswers.front();
        if (!hostKeyAnswers.empty()) {
            hostKeyAnswers.pop_front();
        }
        std::thread([promise, decision] { promise.fulfil(decision); }).detach();
    }

    void requestCredential(CredentialPrompt prompt,
                           DecisionPromise<std::optional<std::string>> promise) override {
        std::lock_guard lock(mutex);
        credentials.push_back(prompt);
        std::optional<std::string> answer;
        if (!credentialAnswers.empty()) {
            answer = credentialAnswers.front();
            credentialAnswers.pop_front();
        }
        promise.fulfil(answer);
    }

    void requestSecurityKey(SecurityKeyPrompt prompt,
                            std::shared_ptr<SecurityKeySession> session) override {
        std::lock_guard lock(mutex);
        securityKeys.push_back(prompt);
        if (cancelSecurityKeys) {
            session->cancel();
            return;
        }
        SecurityKeySession *raw = session.get();
        session->setOnChange([this, raw] {
            const auto state = raw->state();
            std::lock_guard lock(mutex);
            securityKeySteps.push_back(state.step);
            if (state.step != SecurityKeySession::Step::kPin) {
                return;
            }
            pinStates.push_back(state);
            if (securityKeyPins.empty()) {
                raw->cancel();
            } else {
                raw->submitPin(securityKeyPins.front());
                securityKeyPins.pop_front();
            }
        });
    }

    int steps(SecurityKeySession::Step step) {
        std::lock_guard lock(mutex);
        return static_cast<int>(std::ranges::count(securityKeySteps, step));
    }

    std::mutex mutex;
    std::deque<HostKeyDecision> hostKeyAnswers;
    std::deque<std::optional<std::string>> credentialAnswers;
    std::vector<HostKeyPrompt> hostKeys;
    std::vector<CredentialPrompt> credentials;
    std::vector<SecurityKeyPrompt> securityKeys;
    std::vector<SecurityKeySession::Step> securityKeySteps;
    std::vector<SecurityKeySession::State> pinStates;
    std::deque<std::string> securityKeyPins;
    bool cancelSecurityKeys = false;
};

// Presents the authenticator as soon as it's looked for, as many times over as asked.
class FakeKeySource final : public SecurityKeySource {
public:
    explicit FakeKeySource(SoftwareAuthenticator &authenticator, int offers = 1)
        : authenticator_(authenticator), offers_(offers) {}

    void start(std::shared_ptr<SecurityKeySession> session) override {
        ++starts;
        for (int i = 0; i < offers_; ++i) {
            session->offerTransport(std::make_unique<AuthenticatorLink>(authenticator_));
        }
    }
    void stop() override { ++stops; }

    std::atomic<int> starts = 0;
    std::atomic<int> stops = 0;

private:
    SoftwareAuthenticator &authenticator_;
    int offers_;
};

// Presents a software OpenPGP card as soon as one is looked for, as many times over as asked.
class FakeCardSource final : public SecurityKeySource {
public:
    explicit FakeCardSource(SoftwareOpenPgpCard &card, int offers = 1) : card_(card), offers_(offers) {}

    void start(std::shared_ptr<SecurityKeySession> session) override {
        ++starts;
        EXPECT(session->kind() == SecurityKeySession::Kind::kOpenPgpCard);
        for (int i = 0; i < offers_; ++i) {
            auto card = filepan::securitykey::OpenPgpCard::open(std::make_unique<CardLink>(card_), false);
            EXPECT(card.has_value());
            session->offerTransport(std::move(*card));
        }
    }
    void stop() override { ++stops; }

    std::atomic<int> starts = 0;
    std::atomic<int> stops = 0;

private:
    SoftwareOpenPgpCard &card_;
    int offers_;
};

SftpServerConfig serverConfig(const Sshd &sshd, const fs::path &dir,
                              const std::string &key = "client_key") {
    SftpServerConfig config;
    config.id = "test";
    config.server.host = "127.0.0.1";
    config.server.port = sshd.port();
    config.server.username = getenv("USER") ? getenv("USER") : "root";
    config.server.authentication.type = SftpAuthentication::Type::kPrivateKey;
    config.server.authentication.privateKey = filepan::test::read(dir / key);
    config.knownHostsPath = (dir / "known_hosts").string();
    config.timeoutSeconds = 10;
    return config;
}

std::string readAll(InputStream &stream, std::size_t chunk = 300'000) {
    std::string data;
    std::vector<std::uint8_t> buffer(chunk);
    while (true) {
        const auto length = stream.read(buffer);
        EXPECT(length);
        if (*length == 0) {
            return data;
        }
        data.append(reinterpret_cast<const char *>(buffer.data()), *length);
    }
}

std::map<std::string, Entry> listAll(Backend &backend, const Location &directory) {
    std::map<std::string, Entry> entries;
    const auto result = backend.list(
        directory, {.minBatchSize = 2},
        [&](std::vector<Entry> batch) {
            EXPECT(!batch.empty());
            for (auto &entry : batch) {
                entries.emplace(entry.name, std::move(entry));
            }
        },
        CancellationToken());
    if (!result) {
        std::fprintf(stderr, "list: %d %s\n", result.error().code, result.error().message.c_str());
    }
    EXPECT(result);
    return entries;
}

void testErrorMapping() {
    EXPECT(toError({SSH_FX_NO_SUCH_FILE, "x", -1}).code == ENOENT);
    EXPECT(toError({SSH_FX_PERMISSION_DENIED, "x", -1}).code == EACCES);
    EXPECT(toError({SSH_FX_FAILURE, "x", -1}).code == EIO);
    EXPECT(toError({SSH_FX_OP_UNSUPPORTED, "x", -1}).code == ENOTSUP);
    EXPECT(toError({SftpError::kConnectFailed, "x", 0}).code == ECONNREFUSED);
    EXPECT(toError({SftpError::kAuthenticationFailed, "x", 0}).code == EACCES);
    EXPECT(toError({SftpError::kConnectionLost, "gone", -1}, "ctx").message == "ctx: gone");
}

// Trust on first use, then a changed key.
void testHostKeys(const Sshd &sshd, const fs::path &dir) {
    const auto prompts = std::make_shared<ScriptedPrompts>();
    {
        SftpBackend backend(prompts);
        backend.setServer(serverConfig(sshd, dir));
        // Rejected: no connection, nothing saved.
        const auto rejected = backend.connect("test");
        EXPECT(!rejected && rejected.error().code == EACCES);
        EXPECT(prompts->hostKeys.size() == 1u);
        EXPECT(!fs::exists(dir / "known_hosts"));
        const auto &prompt = prompts->hostKeys[0];
        EXPECT(prompt.status == HostKeyPrompt::Status::kUnknown);
        EXPECT(prompt.fingerprint == sshd.hostFingerprint() && prompt.keyType == "ssh-ed25519");
        EXPECT(prompt.serverId == "test" && prompt.hop == 0 && !prompt.isJumpHost);

        prompts->hostKeyAnswers = {HostKeyDecision::kAcceptAndSave};
        EXPECT(backend.connect("test"));
        EXPECT(prompts->hostKeys.size() == 2u);
        const std::string known = filepan::test::read(dir / "known_hosts");
        EXPECT(known.starts_with("[127.0.0.1]:" + std::to_string(sshd.port()) + " ssh-ed25519 "));
    }
    {
        // Known now: a new backend connects without asking.
        SftpBackend backend(prompts);
        backend.setServer(serverConfig(sshd, dir));
        EXPECT(backend.connect("test"));
        EXPECT(prompts->hostKeys.size() == 2u);
    }
    // Another key for the same host: a changed-key warning, with the old fingerprint.
    const std::string saved = filepan::test::read(dir / "known_hosts");
    filepan::test::run(std::string(FILEPAN_SSH_KEYGEN) + " -q -t ed25519 -N '' -f "
                       + (dir / "impostor").string());
    const std::string impostor = filepan::test::read(dir / "impostor.pub");
    const auto keyStart = impostor.find(' ') + 1;
    const std::string impostorKey = impostor.substr(keyStart, impostor.find(' ', keyStart) - keyStart);
    std::ofstream(dir / "known_hosts") << "# a comment\n[127.0.0.1]:" << sshd.port()
                                       << " ssh-ed25519 " << impostorKey << "\n";
    {
        SftpBackend backend(prompts);
        backend.setServer(serverConfig(sshd, dir));
        const auto rejected = backend.connect("test");
        EXPECT(!rejected && rejected.error().code == EACCES);
        EXPECT(prompts->hostKeys.size() == 3u);
        const auto &prompt = prompts->hostKeys[2];
        EXPECT(prompt.status == HostKeyPrompt::Status::kChanged);
        EXPECT(prompt.fingerprint == sshd.hostFingerprint());
        EXPECT(!prompt.knownFingerprint.empty() && prompt.knownFingerprint != prompt.fingerprint);

        // Accepting once doesn't touch the file but holds for the session.
        prompts->hostKeyAnswers = {HostKeyDecision::kAcceptOnce};
        EXPECT(backend.connect("test"));
        backend.disconnect("test");
        EXPECT(backend.connect("test"));
        EXPECT(prompts->hostKeys.size() == 4u);
        EXPECT(filepan::test::read(dir / "known_hosts").find(impostorKey) != std::string::npos);
    }
    std::ofstream(dir / "known_hosts") << saved;
}

// A passphrase-protected key: asked for, wrong once, then right; and giving up.
void testPassphrase(const Sshd &sshd, const fs::path &dir) {
    const auto prompts = std::make_shared<ScriptedPrompts>();
    SftpBackend backend(prompts);
    backend.setServer(serverConfig(sshd, dir, "locked_key"));
    prompts->credentialAnswers = {"wrong", "secret phrase"};
    EXPECT(backend.connect("test"));
    EXPECT(prompts->credentials.size() == 2u);
    EXPECT(prompts->credentials[0].kind == CredentialPrompt::Kind::kPassphrase);
    EXPECT(!prompts->credentials[0].retry && prompts->credentials[1].retry);
    EXPECT(prompts->credentials[0].host == "127.0.0.1");
    // Remembered for reconnecting.
    backend.disconnect("test");
    EXPECT(backend.connect("test"));
    EXPECT(prompts->credentials.size() == 2u);

    // Replacing the server forgets it; declining fails.
    backend.setServer(serverConfig(sshd, dir, "locked_key"));
    const auto declined = backend.connect("test");
    EXPECT(!declined && declined.error().code == EACCES);
    EXPECT(prompts->credentials.size() == 3u);

    // Unknown servers, and a server whose key wasn't saved, which no prompt can supply.
    EXPECT(backend.connect("nope").error().code == ENODEV);
    auto missingKey = serverConfig(sshd, dir, "no_such_key");
    EXPECT(missingKey.server.authentication.privateKey.empty());
    backend.setServer(missingKey);
    const auto noKey = backend.connect("test");
    EXPECT(!noKey && noKey.error().code == ENOKEY);
    EXPECT(prompts->credentials.size() == 3u);
}

// Signs in with "sk-" keys enrolled on a software authenticator: through a jump host, with a
// PIN, cancelled, with the wrong key, and with nowhere to find a key. Parallel reads keep to
// the one connection rather than asking for a touch each.
void testSecurityKeys(const Sshd &sshd, const fs::path &dir) {
    using Step = SecurityKeySession::Step;
    SoftwareAuthenticator authenticator({.pin = "1234"});
    const auto [touchKey, touchAuthorized] = enrollSshKey(authenticator);
    const auto [pinKey, pinAuthorized] =
        enrollSshKey(authenticator, SSH_SK_USER_PRESENCE_REQD | SSH_SK_USER_VERIFICATION_REQD);
    std::ofstream(dir / "authorized_keys", std::ios::app)
        << touchAuthorized << "verify-required " << pinAuthorized;
    std::ofstream(dir / "sk_touch") << touchKey;
    std::ofstream(dir / "sk_pin") << pinKey;
    const auto source = std::make_shared<FakeKeySource>(authenticator);
    const auto prompts = std::make_shared<ScriptedPrompts>();
    SftpBackend backend(prompts, {.securityKeys = source});
    int limited = 0;
    backend.setOnPoolLimited([&](const PoolLimit &) { ++limited; });

    // A touch for the jump host, then one for the server, each with a prompt of its own that
    // closes when it's done.
    auto config = serverConfig(sshd, dir, "sk_touch");
    config.jumpHosts = {config.server};
    backend.setServer(config);
    EXPECT(backend.connect("test"));
    EXPECT(prompts->securityKeys.size() == 2u);
    EXPECT(prompts->securityKeys[0].isJumpHost && prompts->securityKeys[0].hop == 0);
    EXPECT(!prompts->securityKeys[1].isJumpHost && prompts->securityKeys[1].hop == 1);
    EXPECT(!prompts->securityKeys[1].userVerification);
    EXPECT(source->starts == 2 && source->stops == 2);
    EXPECT(authenticator.touches == 2);
    EXPECT(prompts->steps(Step::kTouch) == 2 && prompts->steps(Step::kDone) == 2);
    EXPECT(prompts->steps(Step::kPin) == 0);

    std::ofstream(dir / "sk_file") << "touched";
    const auto file = Location::sftp("test", (dir / "sk_file").string());
    for (int i = 0; i < 2; ++i) {
        auto stream = backend.openReadParallel(file);
        EXPECT(stream && readAll(**stream) == "touched");
    }
    EXPECT(backend.parallelConnectionCount("test") == 0u && limited == 0);
    EXPECT(authenticator.touches == 2 && prompts->securityKeys.size() == 2u);

    // The PIN, wrong the first time.
    prompts->securityKeyPins = {"0000", "1234"};
    backend.setServer(serverConfig(sshd, dir, "sk_pin"));
    EXPECT(backend.connect("test"));
    EXPECT(prompts->securityKeys.size() == 3u && prompts->securityKeys[2].userVerification);
    EXPECT(prompts->pinStates.size() == 2u);
    EXPECT(prompts->pinStates[0].pinRetries == 8 && !prompts->pinStates[0].pinWrong);
    EXPECT(prompts->pinStates[1].pinRetries == 7 && prompts->pinStates[1].pinWrong);

    // No PIN given.
    backend.setServer(serverConfig(sshd, dir, "sk_pin"));
    const auto noPin = backend.connect("test");
    EXPECT(!noPin && noPin.error().code == ECANCELED);

    // Cancelled from the prompt.
    prompts->cancelSecurityKeys = true;
    backend.setServer(serverConfig(sshd, dir, "sk_touch"));
    const auto cancelled = backend.connect("test");
    EXPECT(!cancelled && cancelled.error().code == ECANCELED);
    prompts->cancelSecurityKeys = false;

    // Another key, presented three times over, doesn't have it.
    SoftwareAuthenticator other;
    const auto otherSource = std::make_shared<FakeKeySource>(other, 3);
    SftpBackend wrong(prompts, {.securityKeys = otherSource});
    wrong.setServer(serverConfig(sshd, dir, "sk_touch"));
    const auto wrongKey = wrong.connect("test");
    EXPECT(!wrongKey && wrongKey.error().code == EACCES);
    EXPECT(wrongKey.error().message.find("doesn't have this SSH key") != std::string::npos);
    EXPECT(otherSource->starts == 1);

    // Nowhere to find a key.
    SftpBackend bare(prompts);
    bare.setServer(serverConfig(sshd, dir, "sk_touch"));
    const auto unavailable = bare.connect("test");
    EXPECT(!unavailable && unavailable.error().code == EACCES);
}

// Logs in with OpenPGP cards' authentication keys through the in-process agent: the key read off
// the card, through a jump host with the PIN wrong once and then remembered, an RSA key, the
// wrong card, a key the server doesn't take, cancelled, and with nowhere to find a card.
void testOpenPgpCards(const Sshd &sshd, const fs::path &dir) {
    using Algorithm = filepan::securitykey::OpenPgpCard::Algorithm;
    // The software cards' Ed25519 keys come from libssh's random generator.
    EXPECT(ssh_init() == SSH_OK);
    SoftwareOpenPgpCard card(CardOptions{});
    SoftwareOpenPgpCard rsa(CardOptions{.algorithm = Algorithm::kRsa, .serial = 0x0BADCAFE});
    SoftwareOpenPgpCard stranger(CardOptions{.serial = 0x5EE5});
    const std::string cardLine = filepan::formatPublicKey(card.publicBlob(), "cardno:000612345678");
    const std::string rsaLine = filepan::formatPublicKey(rsa.publicBlob(), "");
    std::ofstream(dir / "authorized_keys", std::ios::app) << cardLine << '\n' << rsaLine << '\n';
    const auto source = std::make_shared<FakeCardSource>(card);
    const auto prompts = std::make_shared<ScriptedPrompts>();
    SftpBackend backend(prompts, {.securityKeys = source});
    int limited = 0;
    backend.setOnPoolLimited([&](const PoolLimit &) { ++limited; });

    // Read off the card, without its PIN.
    const auto read = backend.readCardKey({.serverId = "test",
                                           .hop = 0,
                                           .isJumpHost = false,
                                           .host = "127.0.0.1",
                                           .port = sshd.port(),
                                           .username = "me",
                                           .userVerification = false});
    EXPECT(read.has_value());
    EXPECT(read->publicKey == cardLine && read->type == "ssh-ed25519");
    EXPECT(read->fingerprint.starts_with("SHA256:"));
    EXPECT(prompts->securityKeys.size() == 1u && !prompts->securityKeys[0].userVerification);
    EXPECT(card.verifies == 0 && source->starts == 1 && source->stops == 1);

    const auto cardConfig = [&](const std::string &publicKey) {
        auto config = serverConfig(sshd, dir);
        config.server.authentication = {};
        config.server.authentication.type = SftpAuthentication::Type::kOpenPgpCard;
        config.server.authentication.cardPublicKey = publicKey;
        return config;
    };

    // The jump host asks for the PIN, wrong the first time; the server then uses the one that
    // worked.
    prompts->securityKeyPins = {"000000", "123456"};
    auto config = cardConfig(read->publicKey);
    config.jumpHosts = {config.server};
    backend.setServer(config);
    EXPECT(backend.connect("test"));
    EXPECT(prompts->securityKeys.size() == 3u);
    EXPECT(prompts->securityKeys[1].isJumpHost && prompts->securityKeys[1].userVerification);
    EXPECT(!prompts->securityKeys[2].isJumpHost);
    EXPECT(prompts->pinStates.size() == 2u);
    EXPECT(prompts->pinStates[0].pinRetries == 3 && !prompts->pinStates[0].pinWrong);
    EXPECT(prompts->pinStates[1].pinRetries == 2 && prompts->pinStates[1].pinWrong);
    EXPECT(card.verifies == 3 && card.signatures == 2);
    EXPECT(prompts->steps(SecurityKeySession::Step::kTouch) == 0);

    // Parallel reads keep to the one connection.
    std::ofstream(dir / "card_file") << "carded";
    const auto file = Location::sftp("test", (dir / "card_file").string());
    for (int i = 0; i < 2; ++i) {
        auto stream = backend.openReadParallel(file);
        EXPECT(stream && readAll(**stream) == "carded");
    }
    EXPECT(backend.parallelConnectionCount("test") == 0u && limited == 0);
    EXPECT(card.signatures == 2);

    // Connecting again doesn't ask for the PIN.
    backend.setServer(cardConfig(read->publicKey));
    EXPECT(backend.connect("test"));
    EXPECT(prompts->pinStates.size() == 2u && card.signatures == 3);

    // An RSA key, signed with SHA-2 as the server asks.
    SftpBackend rsaBackend(prompts, {.securityKeys = std::make_shared<FakeCardSource>(rsa)});
    prompts->securityKeyPins = {"123456"};
    rsaBackend.setServer(cardConfig(rsaLine));
    EXPECT(rsaBackend.connect("test"));
    EXPECT(rsa.signatures == 1);

    // Another card, presented three times over, doesn't have the key.
    const auto strangerSource = std::make_shared<FakeCardSource>(stranger, 3);
    SftpBackend wrong(prompts, {.securityKeys = strangerSource});
    wrong.setServer(cardConfig(read->publicKey));
    const auto wrongCard = wrong.connect("test");
    EXPECT(!wrongCard && wrongCard.error().code == EACCES);
    EXPECT(wrongCard.error().message.find("doesn't have the key saved") != std::string::npos);
    EXPECT(strangerSource->starts == 1 && stranger.verifies == 0);

    // A key the server doesn't take: refused before the card is asked for.
    const auto pinsAsked = prompts->pinStates.size();
    const auto asked = prompts->securityKeys.size();
    SftpBackend refused(prompts, {.securityKeys = std::make_shared<FakeCardSource>(stranger)});
    refused.setServer(cardConfig(filepan::formatPublicKey(stranger.publicBlob(), "")));
    EXPECT(!refused.connect("test"));
    EXPECT(prompts->securityKeys.size() == asked && prompts->pinStates.size() == pinsAsked);
    EXPECT(stranger.signatures == 0);

    // Cancelled from the prompt.
    prompts->cancelSecurityKeys = true;
    SftpBackend cancelling(prompts, {.securityKeys = std::make_shared<FakeCardSource>(card)});
    cancelling.setServer(cardConfig(read->publicKey));
    const auto cancelled = cancelling.connect("test");
    EXPECT(!cancelled && cancelled.error().code == ECANCELED);
    EXPECT(cancelled.error().message.find("OpenPGP card") != std::string::npos);
    prompts->cancelSecurityKeys = false;

    // No key read, and nowhere to find a card.
    backend.setServer(cardConfig(""));
    const auto noKey = backend.connect("test");
    EXPECT(!noKey && noKey.error().code == ENOKEY);
    SftpBackend bare(prompts);
    bare.setServer(cardConfig(read->publicKey));
    const auto unavailable = bare.connect("test");
    EXPECT(!unavailable && unavailable.error().code == EACCES);
    SecurityKeyPrompt nowhere;
    nowhere.serverId = "test";
    EXPECT(!bare.readCardKey(nowhere));
    ssh_finalize();
}

void testCardPinCache() {
    CardPinCache pins(std::chrono::milliseconds(200));
    EXPECT(!pins.find("a"));
    pins.remember("a", "123456");
    EXPECT(pins.find("a") == "123456" && !pins.find("b"));
    // Each use keeps it longer.
    for (int i = 0; i < 3; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        EXPECT(pins.find("a") == "123456");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    EXPECT(!pins.find("a"));
    pins.remember("a", "123456");
    pins.forget("a");
    EXPECT(!pins.find("a"));
}

void testFileOperations(const Sshd &sshd, const fs::path &dir) {
    const auto prompts = std::make_shared<ScriptedPrompts>();
    SftpBackend backend(prompts);
    backend.setServer(serverConfig(sshd, dir));
    const fs::path remote = dir / "remote";
    const Location root = Location::sftp("test", remote.string());
    EXPECT(backend.makeDirectory(root, 0755));
    EXPECT(backend.makeDirectory(root).error().code == EEXIST);
    EXPECT(backend.homeDirectory("test"));

    // Write in uneven pieces, then read back with small and large reads.
    const std::string content = randomContent(5 * 1024 * 1024 + 77, 1);
    {
        auto output = backend.openWrite(root.child("data.bin"), {.mode = WriteMode::kCreateNew,
                                                                 .permissions = 0640});
        EXPECT(output);
        const auto *bytes = reinterpret_cast<const std::uint8_t *>(content.data());
        for (std::size_t offset = 0; offset < content.size(); offset += 333'333) {
            const std::size_t length = std::min<std::size_t>(333'333, content.size() - offset);
            EXPECT((*output)->write({bytes + offset, length}));
        }
        EXPECT(*(*output)->size() == content.size());
        EXPECT((*output)->close());
    }
    EXPECT(filepan::test::read(remote / "data.bin") == content);
    EXPECT((fs::status(remote / "data.bin").permissions() & fs::perms::all)
           == (fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read));
    EXPECT(backend.openWrite(root.child("data.bin"), {}).error().code == EEXIST);
    {
        auto input = backend.openRead(root.child("data.bin"));
        EXPECT(input);
        EXPECT(*(*input)->size() == content.size());
        EXPECT(readAll(**input, 4096) == content);
        EXPECT(*(*input)->seek(1'000'000, SeekOrigin::kBegin) == 1'000'000);
        std::vector<std::uint8_t> chunk(16);
        EXPECT(*(*input)->read(chunk) == 16);
        EXPECT(std::string(chunk.begin(), chunk.end()) == content.substr(1'000'000, 16));
        EXPECT(*(*input)->seek(-10, SeekOrigin::kCurrent) == 1'000'006);
        EXPECT(*(*input)->read(chunk) == 16);
        EXPECT(std::string(chunk.begin(), chunk.end()) == content.substr(1'000'006, 16));
        EXPECT(*(*input)->seek(-5, SeekOrigin::kEnd) == content.size() - 5);
        EXPECT(readAll(**input) == content.substr(content.size() - 5));
    }
    // Replace and existing modes.
    {
        auto output = backend.openWrite(root.child("data.bin"), {.mode = WriteMode::kExisting});
        EXPECT(output);
        EXPECT((*output)->seek(2, SeekOrigin::kBegin));
        EXPECT((*output)->write({reinterpret_cast<const std::uint8_t *>("XY"), 2}));
        EXPECT((*output)->close());
        EXPECT(filepan::test::read(remote / "data.bin").substr(0, 4) == content.substr(0, 2) + "XY");
        auto replaced = backend.openWrite(root.child("small.txt"), {.mode = WriteMode::kReplace});
        EXPECT(replaced && (*replaced)->write({reinterpret_cast<const std::uint8_t *>("hi"), 2}));
        EXPECT((*replaced)->close());
    }

    // Stat, symlinks and listing.
    EXPECT(backend.createFile(root.child("empty")));
    EXPECT(backend.createFile(root.child("empty")).error().code == EEXIST);
    EXPECT(backend.createSymlink(root.child("link"), "small.txt"));
    EXPECT(backend.createSymlink(root.child("broken"), "missing"));
    EXPECT(backend.makeDirectory(root.child("sub")));
    const auto stat = backend.stat(root.child("data.bin"));
    EXPECT(stat && stat->type == EntryType::kFile && stat->size == content.size());
    EXPECT(stat->name == "data.bin" && stat->modifiedTime.seconds > 0);
    const auto link = backend.lstat(root.child("link"));
    EXPECT(link && link->type == EntryType::kSymlink && link->linkTarget == "small.txt");
    EXPECT(link->targetType == EntryType::kFile && link->size == 2);
    EXPECT(backend.stat(root.child("link"))->type == EntryType::kFile);
    EXPECT(*backend.readLink(root.child("link")) == "small.txt");
    EXPECT(backend.stat(root.child("missing")).error().code == ENOENT);
    EXPECT(backend.openRead(root.child("missing")).error().code == ENOENT);
    const auto entries = listAll(backend, root);
    EXPECT(entries.size() == 6u);
    EXPECT(entries.at("sub").type == EntryType::kDirectory && entries.at("sub").hasStat);
    EXPECT(entries.at("data.bin").size == content.size());
    EXPECT(entries.at("link").linkTarget == "small.txt" && entries.at("link").isDirectoryLike() == false);
    EXPECT(entries.at("broken").brokenLink);
    EXPECT(backend.list(root.child("data.bin"), {}, [](auto) {}, CancellationToken()).error().code
           == ENOTDIR);

    // Times and permissions.
    EXPECT(backend.setTimes(root.child("small.txt"), std::nullopt, Timestamp{1'600'000'000, 0}));
    EXPECT(backend.stat(root.child("small.txt"))->modifiedTime.seconds == 1'600'000'000);
    EXPECT(backend.chmod(root.child("small.txt"), 0600));
    EXPECT((backend.stat(root.child("small.txt"))->mode & 07777) == 0600);

    // Rename, with and without replacing.
    EXPECT(backend.rename(root.child("small.txt"), root.child("empty"), false).error().code == EEXIST);
    EXPECT(backend.rename(root.child("small.txt"), root.child("empty"), true));
    EXPECT(filepan::test::read(remote / "empty") == "hi" && !fs::exists(remote / "small.txt"));
    EXPECT(backend.rename(root.child("empty"), root.child("sub").child("moved"), false));
    EXPECT(fs::exists(remote / "sub" / "moved"));
    EXPECT(backend.rename(root, Location::sftp("other", "/x"), false).error().code == EXDEV);

    // Remove: a non-empty directory needs recursion, which reports progress.
    for (int i = 0; i < 100; ++i) {
        std::ofstream(remote / "sub" / ("f" + std::to_string(i))) << i;
    }
    fs::create_directories(remote / "sub" / "deep" / "er");
    std::ofstream(remote / "sub" / "deep" / "er" / "x") << "x";
    EXPECT(backend.remove(root.child("sub"), false, nullptr, CancellationToken()).error().code
           == ENOTEMPTY);
    class Progress : public RemoveCallbacks {
    public:
        bool onProgress(std::uint64_t count, std::string_view) override {
            total += count;
            ++calls;
            return true;
        }
        ErrorAction onError(std::string_view, const filepan::Error &) override {
            return ErrorAction::kAbort;
        }
        std::uint64_t total = 0;
        int calls = 0;
    } progress;
    const auto removed = backend.remove(root.child("sub"), true, &progress, CancellationToken());
    EXPECT(removed && removed->removedCount == 105 && removed->skippedCount == 0);
    EXPECT(progress.total == 105 && progress.calls > 1);
    EXPECT(!fs::exists(remote / "sub"));
    EXPECT(backend.remove(root.child("link"), false, nullptr, CancellationToken()));
    EXPECT(fs::exists(remote / "data.bin"));
}

// Dropped connections: calls reconnect, and open streams carry on.
// saveFile over SFTP, whose rename can't replace: the old file goes, then the new one takes its name.
void testSave(const Sshd &sshd, const fs::path &dir) {
    const auto prompts = std::make_shared<ScriptedPrompts>();
    SftpBackend backend(prompts);
    backend.setServer(serverConfig(sshd, dir));
    const fs::path remote = dir / "save";
    fs::create_directories(remote);
    std::ofstream(remote / "notes.txt", std::ios::binary) << "old";
    fs::permissions(remote / "notes.txt", fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read);
    const Location file = Location::sftp("test", (remote / "notes.txt").string());
    const auto before = backend.stat(file);
    EXPECT(before);
    const std::string content = "new\ncontents\n";
    const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t *>(content.data()), content.size());
    auto saved = saveFile(backend, file, bytes, FileStamp::of(*before));
    EXPECT(saved && saved->size == content.size() && (saved->mode & 07777) == 0640);
    std::ifstream in(remote / "notes.txt", std::ios::binary);
    EXPECT(std::string(std::istreambuf_iterator<char>(in), {}) == content);
    EXPECT(std::distance(fs::directory_iterator(remote), fs::directory_iterator()) == 1);
    // Against the old stamp, it's a conflict.
    saved = saveFile(backend, file, bytes, FileStamp::of(*before));
    EXPECT(!saved && saved.error().code == kSaveConflict);
}

void testReconnect(const Sshd &sshd, const fs::path &dir) {
    const auto prompts = std::make_shared<ScriptedPrompts>();
    SftpBackend backend(prompts);
    backend.setServer(serverConfig(sshd, dir));
    const fs::path remote = dir / "reconnect";
    fs::create_directories(remote);
    const std::string content = randomContent(3 * 1024 * 1024, 2);
    std::ofstream(remote / "file", std::ios::binary) << content;
    const Location file = Location::sftp("test", (remote / "file").string());

    auto input = backend.openRead(file);
    EXPECT(input);
    std::vector<std::uint8_t> head(1000);
    EXPECT(*(*input)->read(head) == 1000);
    auto output = backend.openWrite(Location::sftp("test", (remote / "out").string()),
                                    {.mode = WriteMode::kCreateNew});
    EXPECT(output);
    EXPECT((*output)->write({reinterpret_cast<const std::uint8_t *>(content.data()), 2'000'000}));

    sshd.dropConnections();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT(listAll(backend, Location::sftp("test", remote.string())).contains("file"));
    // Past the read-ahead buffer, so it reads from the server again.
    EXPECT(*(*input)->seek(2'000'000, SeekOrigin::kBegin) == 2'000'000);
    EXPECT(readAll(**input) == content.substr(2'000'000));
    EXPECT((*output)->write({reinterpret_cast<const std::uint8_t *>(content.data()) + 2'000'000,
                             content.size() - 2'000'000}));
    EXPECT((*output)->close());
    EXPECT(filepan::test::read(remote / "out") == content);
    EXPECT(prompts->hostKeys.empty());
}

filepan::jobs::JobProgress runJob(filepan::jobs::JobQueue &queue, filepan::jobs::JobSpec spec,
                                  filepan::jobs::JobResult *result = nullptr) {
    const auto id = queue.submit(std::move(spec));
    queue.waitUntilIdle();
    const auto progress = queue.progress(id);
    EXPECT(progress && filepan::jobs::isFinished(progress->state));
    if (progress->error) {
        std::fprintf(stderr, "job failed: %d %s\n", progress->error->code,
                     progress->error->message.c_str());
    }
    if (result) {
        *result = *queue.result(id);
    }
    return *progress;
}

// Jobs across backends: local to SFTP and back with equal checksums, archives on the server,
// and moving and deleting there.
void testJobs(const Sshd &sshd, const fs::path &dir) {
    using namespace filepan::jobs;
    auto resolver = std::make_shared<SimpleBackendResolver>();
    auto sftp = std::make_shared<SftpBackend>(std::make_shared<ScriptedPrompts>());
    sftp->setServer(serverConfig(sshd, dir));
    resolver->set(BackendKind::kLocal, std::make_shared<LocalBackend>());
    resolver->set(BackendKind::kSftp, sftp);
    auto archives = std::make_shared<ArchiveBackend>(*resolver);
    resolver->set(BackendKind::kArchive, archives);
    auto decisions = std::make_shared<filepan::test::ScriptedDecisions>();
    JobQueue queue(resolver, decisions, nullptr);

    const fs::path source = dir / "jobsrc";
    fs::create_directories(source / "sub");
    const std::string big = randomContent(6 * 1024 * 1024 + 11, 3);
    std::ofstream(source / "big.bin", std::ios::binary) << big;
    std::ofstream(source / "sub" / "small.txt") << "small";
    fs::create_symlink("big.bin", source / "link");
    const fs::path remote = dir / "jobremote";
    fs::create_directories(remote);
    const auto at = [](const fs::path &path) { return Location::sftp("test", path.string()); };
    const auto local = [](const fs::path &path) { return Location::local(path.string()); };

    // Up, then down again.
    const auto up = runJob(queue, CopyJob{{local(source)}, at(remote)});
    EXPECT(up.state == JobState::kSucceeded && up.bytesDone == big.size() + 5);
    EXPECT(fs::read_symlink(remote / "jobsrc" / "link") == "big.bin");
    fs::create_directories(dir / "jobback");
    const auto down = runJob(queue, CopyJob{{at(remote / "jobsrc")}, local(dir / "jobback")});
    EXPECT(down.state == JobState::kSucceeded && down.bytesDone == big.size() + 5);
    EXPECT(filepan::test::readFile(dir / "jobback" / "jobsrc" / "sub" / "small.txt") == "small");
    // Modification times survive both ways.
    EXPECT(fs::last_write_time(dir / "jobback" / "jobsrc" / "big.bin")
           >= fs::last_write_time(source / "big.bin") - std::chrono::seconds(1));

    const std::vector<filepan::ChecksumAlgorithm> algorithms = {
        filepan::ChecksumAlgorithm::kSha256, filepan::ChecksumAlgorithm::kCrc32};
    JobResult original, uploaded, roundTrip;
    EXPECT(runJob(queue, ChecksumJob{local(source / "big.bin"), algorithms}, &original).state
           == JobState::kSucceeded);
    EXPECT(runJob(queue, ChecksumJob{at(remote / "jobsrc" / "big.bin"), algorithms}, &uploaded)
               .state
           == JobState::kSucceeded);
    EXPECT(runJob(queue, ChecksumJob{local(dir / "jobback" / "jobsrc" / "big.bin"), algorithms},
                  &roundTrip)
               .state
           == JobState::kSucceeded);
    EXPECT(!original.checksums[3].empty());
    EXPECT(original.checksums == uploaded.checksums && original.checksums == roundTrip.checksums);

    // An archive made on the server from files there, read back through the archive backend and
    // extracted locally.
    const auto created = runJob(queue, CreateArchiveJob{{at(remote / "jobsrc")},
                                                        at(remote / "a.zip"),
                                                        {ARCHIVE_FORMAT_ZIP, ARCHIVE_FILTER_NONE,
                                                         "", 0}});
    EXPECT(created.state == JobState::kSucceeded);
    const Location zipRoot = Location::archive(at(remote / "a.zip"));
    const auto inZip = listAll(*archives, zipRoot.child("jobsrc"));
    EXPECT(inZip.size() == 3u && inZip.at("big.bin").size == big.size());
    EXPECT(inZip.at("link").type == EntryType::kSymlink);
    fs::create_directories(dir / "jobextract");
    const auto extracted =
        runJob(queue, ExtractArchiveJob{at(remote / "a.zip"), local(dir / "jobextract"), {}});
    EXPECT(extracted.state == JobState::kSucceeded);
    EXPECT(filepan::test::readFile(dir / "jobextract" / "jobsrc" / "big.bin") == big);
    EXPECT(fs::read_symlink(dir / "jobextract" / "jobsrc" / "link") == "big.bin");

    // Move up (copy and delete), rename on the server, then delete there.
    std::ofstream(dir / "moveme") << "moved";
    EXPECT(runJob(queue, MoveJob{{local(dir / "moveme")}, at(remote)}).state
           == JobState::kSucceeded);
    EXPECT(!fs::exists(dir / "moveme") && filepan::test::readFile(remote / "moveme") == "moved");
    EXPECT(runJob(queue, MoveJob{{at(remote / "moveme")}, at(remote / "jobsrc")}).state
           == JobState::kSucceeded);
    EXPECT(fs::exists(remote / "jobsrc" / "moveme"));
    const auto deleted = runJob(queue, DeleteJob{{at(remote / "jobsrc")}});
    EXPECT(deleted.state == JobState::kSucceeded && deleted.itemsDone == deleted.itemsTotal);
    EXPECT(!fs::exists(remote / "jobsrc"));
}

// Backends without a pool open parallel streams as openRead() does.
void testParallelDefault(const fs::path &dir) {
    class Counting : public filepan::test::FakeRemoteBackend {
    public:
        using FakeRemoteBackend::FakeRemoteBackend;
        filepan::Result<std::unique_ptr<InputStream>> openRead(
            const Location &location) override {
            ++opened;
            return FakeRemoteBackend::openRead(location);
        }
        int opened = 0;
    };
    fs::create_directories(dir / "fake");
    std::ofstream(dir / "fake" / "file") << "parallel";
    Counting backend(dir / "fake");
    auto stream = static_cast<Backend &>(backend).openReadParallel(Counting::at("/file"));
    EXPECT(stream && backend.opened == 1);
    EXPECT(readAll(**stream) == "parallel");
    EXPECT(backend.openReadParallel(Counting::at("/missing")).error().code == ENOENT);
}

std::vector<std::string> writeFiles(const fs::path &directory, std::size_t count, unsigned seed) {
    fs::create_directories(directory);
    std::vector<std::string> contents;
    for (std::size_t i = 0; i < count; ++i) {
        contents.push_back(randomContent(3 * 1024 * 1024 + i * 1001, seed + i));
        std::ofstream(directory / std::to_string(i), std::ios::binary) << contents.back();
    }
    return contents;
}

// Reads each stream to its end on a thread of its own, all at once.
void readTogether(std::vector<std::unique_ptr<InputStream>> &streams,
                  const std::vector<std::string> &contents) {
    std::vector<std::string> read(streams.size());
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < streams.size(); ++i) {
        threads.emplace_back([&, i] { read[i] = readAll(*streams[i], 64 * 1024); });
    }
    for (auto &thread : threads) {
        thread.join();
    }
    for (std::size_t i = 0; i < streams.size(); ++i) {
        EXPECT(read[i] == contents[i % contents.size()]);
    }
}

// Parallel streams get connections of their own, made without asking the user again, which
// close once idle.
void testParallelReads(const fs::path &root) {
    const fs::path dir = root / "parallel";
    fs::create_directories(dir);
    Sshd sshd(dir, "PerSourcePenalties no\n");
    const auto prompts = std::make_shared<ScriptedPrompts>();
    // A host key accepted for the session and a passphrase typed in: neither is saved anywhere
    // the extra connections could read them from but the backend.
    prompts->hostKeyAnswers = {HostKeyDecision::kAcceptOnce};
    prompts->credentialAnswers = {"secret phrase"};
    SftpBackend backend(prompts);
    auto config = serverConfig(sshd, dir, "locked_key");
    config.knownHostsPath.clear();
    backend.setServer(config);
    int limited = 0;
    backend.setOnPoolLimited([&](const PoolLimit &) { ++limited; });
    const auto contents = writeFiles(dir / "files", 3, 10);
    const auto at = [&](std::size_t i) {
        return Location::sftp("test", (dir / "files" / std::to_string(i)).string());
    };

    // Not connected yet: the main connection is made first, asking as usual.
    std::vector<std::unique_ptr<InputStream>> streams;
    for (std::size_t i = 0; i < 3; ++i) {
        auto stream = backend.openReadParallel(at(i));
        EXPECT(stream);
        streams.push_back(std::move(*stream));
    }
    EXPECT(backend.parallelConnectionCount("test") == 3u);
    EXPECT(sshd.awaitConnectionCount(4));
    EXPECT(prompts->hostKeys.size() == 1u && prompts->credentials.size() == 1u);
    readTogether(streams, contents);
    // Past the default most, a fourth shares one, and openRead() keeps to the main one.
    auto fourth = backend.openReadParallel(at(0));
    auto plain = backend.openRead(at(1));
    EXPECT(fourth && plain);
    EXPECT(backend.parallelConnectionCount("test") == 3u && sshd.connectionCount() == 4u);
    EXPECT(readAll(**fourth) == contents[0] && readAll(**plain) == contents[1]);
    fourth->reset();
    streams.clear();

    // Idle ones close on the next call once their time is up; ones in use stay.
    backend.setIdleTimeout(std::chrono::milliseconds(300));
    auto kept = backend.openReadParallel(at(2));
    EXPECT(kept && backend.parallelConnectionCount("test") == 3u);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT(backend.stat(at(0)));
    EXPECT(backend.parallelConnectionCount("test") == 1u);
    EXPECT(sshd.awaitConnectionCount(2));
    EXPECT(readAll(**kept) == contents[2]);

    // A lost connection: the stream reconnects and carries on at its offset, still unasked.
    std::vector<std::uint8_t> head(1000);
    auto again = backend.openReadParallel(at(1));
    EXPECT(again && *(*again)->read(head) == 1000);
    sshd.dropConnections();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT(*(*again)->seek(2'000'000, SeekOrigin::kBegin) == 2'000'000);
    EXPECT(readAll(**again) == contents[1].substr(2'000'000));
    EXPECT(prompts->hostKeys.size() == 1u && prompts->credentials.size() == 1u);

    // Fewer allowed: the extras beyond close once idle.
    again->reset();
    kept->reset();
    backend.setIdleTimeout(std::chrono::seconds(60));
    backend.setParallelConnections(0);  // Clamped to 1.
    streams.clear();
    for (std::size_t i = 0; i < 3; ++i) {
        streams.push_back(std::move(*backend.openReadParallel(at(i))));
    }
    EXPECT(backend.parallelConnectionCount("test") == 1u);
    readTogether(streams, contents);

    // Disconnecting closes them all.
    streams.clear();
    backend.disconnect("test");
    EXPECT(backend.parallelConnectionCount("test") == 0u);
    EXPECT(sshd.awaitConnectionCount(0));
    EXPECT(limited == 0);
}

// A server that takes no more connections: sshd's MaxStartups drops every new one while an
// unauthenticated one is open, as when many clients connect at once.
void testParallelLimitedConnections(const fs::path &root) {
    const fs::path dir = root / "startups";
    fs::create_directories(dir);
    Sshd sshd(dir, "MaxStartups 1:100:1\nPerSourcePenalties no\n");
    const auto prompts = std::make_shared<ScriptedPrompts>();
    prompts->hostKeyAnswers = {HostKeyDecision::kAcceptAndSave};
    SftpBackend backend(prompts);
    backend.setServer(serverConfig(sshd, dir));
    std::vector<PoolLimit> limits;
    backend.setOnPoolLimited([&](const PoolLimit &limit) { limits.push_back(limit); });
    const auto contents = writeFiles(dir / "files", 3, 20);
    const auto at = [&](std::size_t i) {
        return Location::sftp("test", (dir / "files" / std::to_string(i)).string());
    };
    EXPECT(backend.connect("test"));

    // Holds sshd's one unauthenticated slot.
    const int blocker = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(sshd.port()));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    EXPECT(::connect(blocker, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
    EXPECT(sshd.awaitConnectionCount(2));

    std::vector<std::unique_ptr<InputStream>> streams;
    for (std::size_t i = 0; i < 3; ++i) {
        auto stream = backend.openReadParallel(at(i));
        EXPECT(stream);
        streams.push_back(std::move(*stream));
    }
    EXPECT(limits.size() == 1u);
    EXPECT(limits[0].serverId == "test" && limits[0].working == 1u && !limits[0].reason.empty());
    EXPECT(backend.parallelConnectionCount("test") == 0u);
    readTogether(streams, contents);
    streams.clear();

    // The limit holds for the session, even once the server would take more...
    close(blocker);
    EXPECT(sshd.awaitConnectionCount(1));
    auto stream = backend.openReadParallel(at(0));
    EXPECT(stream && readAll(**stream) == contents[0]);
    EXPECT(backend.parallelConnectionCount("test") == 0u && limits.size() == 1u);
    stream->reset();
    // ...until disconnecting starts over.
    backend.disconnect("test");
    stream = backend.openReadParallel(at(1));
    EXPECT(stream && readAll(**stream) == contents[1]);
    EXPECT(backend.parallelConnectionCount("test") == 1u && limits.size() == 1u);
    EXPECT(prompts->hostKeys.size() == 1u);
}

// The server going away while an extra connection is made isn't a connection limit: the main
// connection is gone too, and the stream fails.
void testParallelServerDown(const fs::path &root) {
    const fs::path dir = root / "down";
    fs::create_directories(dir);
    auto sshd = std::make_unique<Sshd>(dir, "PerSourcePenalties no\n");
    const auto prompts = std::make_shared<ScriptedPrompts>();
    prompts->hostKeyAnswers = {HostKeyDecision::kAcceptAndSave};
    SftpBackend backend(prompts);
    backend.setServer(serverConfig(*sshd, dir));
    std::vector<PoolLimit> limits;
    backend.setOnPoolLimited([&](const PoolLimit &limit) { limits.push_back(limit); });
    const auto contents = writeFiles(dir / "files", 2, 1);
    const auto at = [&](std::size_t i) {
        return Location::sftp("test", (dir / "files" / std::to_string(i)).string());
    };
    auto first = backend.openReadParallel(at(0));
    EXPECT(first && backend.parallelConnectionCount("test") == 1u);
    sshd->dropConnections();
    EXPECT(sshd->awaitConnectionCount(0));
    sshd.reset();
    auto second = backend.openReadParallel(at(1));
    EXPECT(!second && limits.empty());
}

// A server that takes the connection but no more SFTP sessions than two at once, as with
// sshd's MaxSessions or a process limit: the third is refused at the subsystem.
void testParallelLimitedSessions(const fs::path &root) {
    const fs::path sftpServer =
        fs::canonical(FILEPAN_SSHD).parent_path().parent_path() / "libexec" / "sftp-server";
    if (!fs::exists(sftpServer)) {
        std::printf("no %s; skipping the session limit test\n", sftpServer.c_str());
        return;
    }
    const fs::path dir = root / "sessions";
    fs::create_directories(dir);
    // mkdir is atomic: whoever makes a slot holds it until its sftp-server exits.
    std::ofstream(dir / "limit.sh")
        << "s=" << (dir / "slot1") << "\n"
        << "mkdir \"$s\" 2>/dev/null || { s=" << (dir / "slot2")
        << "; mkdir \"$s\" 2>/dev/null; } || exit 1\n"
        << sftpServer << "\nrmdir \"$s\"\n";
    Sshd sshd(dir, "PerSourcePenalties no\n", "/bin/sh " + (dir / "limit.sh").string());
    const auto prompts = std::make_shared<ScriptedPrompts>();
    prompts->hostKeyAnswers = {HostKeyDecision::kAcceptAndSave};
    SftpBackend backend(prompts);
    backend.setServer(serverConfig(sshd, dir));
    std::vector<PoolLimit> limits;
    backend.setOnPoolLimited([&](const PoolLimit &limit) { limits.push_back(limit); });
    const auto contents = writeFiles(dir / "files", 3, 30);
    const auto at = [&](std::size_t i) {
        return Location::sftp("test", (dir / "files" / std::to_string(i)).string());
    };

    std::vector<std::unique_ptr<InputStream>> streams;
    for (std::size_t i = 0; i < 3; ++i) {
        auto stream = backend.openReadParallel(at(i));
        EXPECT(stream);
        streams.push_back(std::move(*stream));
    }
    EXPECT(limits.size() == 1u);
    EXPECT(limits[0].working == 2u && limits[0].reason.find("SFTP") != std::string::npos);
    EXPECT(backend.parallelConnectionCount("test") == 1u);
    readTogether(streams, contents);
    // More share the one that works, without trying again.
    streams.push_back(std::move(*backend.openReadParallel(at(0))));
    EXPECT(readAll(*streams.back()) == contents[0]);
    EXPECT(limits.size() == 1u && backend.parallelConnectionCount("test") == 1u);
    EXPECT(prompts->hostKeys.size() == 1u);
}

}  // namespace

int main() {
    TempDir dir;
    Sshd sshd(dir.path());
    testErrorMapping();
    testHostKeys(sshd, dir.path());
    testPassphrase(sshd, dir.path());
    testSecurityKeys(sshd, dir.path());
    testOpenPgpCards(sshd, dir.path());
    testCardPinCache();
    testFileOperations(sshd, dir.path());
    testSave(sshd, dir.path());
    testReconnect(sshd, dir.path());
    testJobs(sshd, dir.path());
    testParallelDefault(dir.path());
    testParallelReads(dir.path());
    testParallelLimitedConnections(dir.path());
    testParallelLimitedSessions(dir.path());
    testParallelServerDown(dir.path());
    std::puts("sftp_backend_test: OK");
    return 0;
}
