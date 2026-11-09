// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// parsePrivateKey on keys made by OpenSSH's ssh-keygen in each format it writes, and on what
// people paste or pick by mistake.

#include <cstdio>
#include <filesystem>
#include <string>

#include "filepan/sftp_client.hpp"
#include "sshd_harness.hpp"
#include "test_util.hpp"

using filepan::parsePrivateKey;
using filepan::test::read;
using filepan::test::run;
using filepan::test::TempDir;
namespace fs = std::filesystem;

namespace {

const std::string kKeygen = FILEPAN_SSH_KEYGEN;

// Makes a key with ssh-keygen; format is "" (OpenSSH), "PEM" or "PKCS8".
fs::path keygen(const fs::path &dir, const std::string &name, const std::string &type,
                const std::string &format = "", const std::string &passphrase = "") {
    const fs::path path = dir / name;
    std::string command = kKeygen + " -q -t " + type + " -N '" + passphrase + "' -C test -f " + path.string();
    if (!format.empty()) {
        command += " -m " + format;
    }
    run(command);
    return path;
}

// ssh-keygen's own "SHA256:..." for a key file.
std::string fingerprint(const fs::path &path) {
    const std::string line = run(kKeygen + " -l -E sha256 -f " + path.string());
    const auto start = line.find("SHA256:");
    EXPECT(start != std::string::npos);
    return line.substr(start, line.find(' ', start) - start);
}

void testFormats() {
    TempDir dir;
    struct Case {
        const char *name;
        const char *type;
        const char *format;
        const char *libsshType;
    };
    const Case cases[] = {
            {"ed25519", "ed25519", "", "ssh-ed25519"},
            {"rsa_openssh", "rsa", "", "ssh-rsa"},
            {"rsa_pem", "rsa", "PEM", "ssh-rsa"},
            {"rsa_pkcs8", "rsa", "PKCS8", "ssh-rsa"},
            {"ecdsa_pem", "ecdsa", "PEM", "ecdsa-sha2-nistp256"},
            {"ecdsa_openssh", "ecdsa", "", "ecdsa-sha2-nistp256"},
    };
    for (const Case &c : cases) {
        const fs::path path = keygen(dir.path(), c.name, c.type, c.format);
        const std::string bytes = read(path);
        auto key = parsePrivateKey(bytes);
        if (!key) {
            std::fprintf(stderr, "%s: %s\n", c.name, key.error().c_str());
        }
        EXPECT(key.has_value());
        EXPECT(!key->encrypted && key->type == c.libsshType);
        EXPECT(key->fingerprint == fingerprint(path));
        EXPECT(key->text == bytes);
    }
}

void testEncrypted() {
    TempDir dir;
    // OpenSSH: checked up to the encrypted part, without asking for the passphrase.
    const fs::path locked = keygen(dir.path(), "locked", "ed25519", "", "secret phrase");
    auto key = parsePrivateKey(read(locked));
    EXPECT(key && key->encrypted && key->type.empty() && key->fingerprint.empty());
    // PEM and PKCS#8: by their markers.
    const fs::path pem = keygen(dir.path(), "locked_pem", "rsa", "PEM", "secret phrase");
    EXPECT(read(pem).contains("Proc-Type: 4,ENCRYPTED"));
    key = parsePrivateKey(read(pem));
    EXPECT(key && key->encrypted);
    const fs::path pkcs8 = keygen(dir.path(), "locked_pkcs8", "ecdsa", "PKCS8", "secret phrase");
    key = parsePrivateKey(read(pkcs8));
    EXPECT(key && key->encrypted);
    // A damaged OpenSSH key isn't taken for an encrypted one.
    std::string damaged = read(locked);
    damaged.replace(damaged.find('\n') + 1, 40, std::string(40, 'A'));
    EXPECT(!parsePrivateKey(damaged));
}

void testNormalization() {
    TempDir dir;
    const fs::path path = keygen(dir.path(), "key", "ed25519");
    const std::string bytes = read(path);
    // Pasted with CRLF, a byte order mark, blank space around it and no final newline.
    std::string pasted = "\xEF\xBB\xBF  \n";
    for (const char c : bytes) {
        if (c == '\n') {
            pasted += '\r';
        }
        pasted += c;
    }
    while (pasted.back() == '\n' || pasted.back() == '\r') {
        pasted.pop_back();
    }
    pasted += "\t \n\n";
    auto key = parsePrivateKey(pasted);
    EXPECT(key && key->text == bytes && key->fingerprint == fingerprint(path));
}

void testMistakes() {
    TempDir dir;
    const fs::path path = keygen(dir.path(), "key", "ed25519");
    const auto error = [](std::string_view text) {
        auto key = parsePrivateKey(text);
        EXPECT(!key);
        return key.error();
    };
    EXPECT(error("").starts_with("Empty."));
    EXPECT(error(" \n\t").starts_with("Empty."));
    EXPECT(error(read(fs::path(path.string() + ".pub"))).starts_with("A public key."));
    const fs::path pem = keygen(dir.path(), "rsa", "rsa", "PEM");
    const std::string pemPublic = run(kKeygen + " -e -m PKCS8 -f " + (pem.string() + ".pub"));
    EXPECT(error(pemPublic).starts_with("A public key."));
    EXPECT(error("PuTTY-User-Key-File-3: ssh-ed25519\nEncryption: none\n").starts_with("A PuTTY key."));
    EXPECT(error("-----BEGIN CERTIFICATE-----\nMIIB\n-----END CERTIFICATE-----\n").starts_with("A certificate"));
    EXPECT(error("hunter2").starts_with("Not an OpenSSH or PEM private key,"));
    EXPECT(error(std::string("\x30\x82\x01\x00\x02\x01", 6)).starts_with("Not a text key file."));
    EXPECT(error(std::string(filepan::kMaxPrivateKeyBytes + 1, 'A')).starts_with("Too large"));
    // Cut off, or with its body garbled.
    const std::string bytes = read(path);
    EXPECT(error(bytes.substr(0, bytes.size() / 2)).starts_with("Not an OpenSSH or PEM private key,"));
    std::string garbled = bytes;
    garbled.replace(garbled.find('\n') + 1, 30, std::string(30, '*'));
    EXPECT(error(garbled).starts_with("Couldn't read this private key:"));
}

}  // namespace

int main() {
    testFormats();
    testEncrypted();
    testNormalization();
    testMistakes();
    std::puts("private_key_test: OK");
    return 0;
}
