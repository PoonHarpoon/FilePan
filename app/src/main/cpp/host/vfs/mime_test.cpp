// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>

#include "filepan/vfs/mime.hpp"
#include "test_util.hpp"

using namespace filepan::vfs;

namespace table {
using filepan::vfs::FileKind;
// The generated tables themselves, for the invariants.
#include "mime_table.inc"
}  // namespace table

namespace {

const char *kindName(FileKind kind) {
    constexpr std::array kNames = {"Directory", "Image", "Video", "Audio", "Archive",
                                   "Apk", "Pdf", "Document", "Spreadsheet", "Presentation",
                                   "Text", "Code", "Font", "Other"};
    return kNames[static_cast<std::size_t>(kind)];
}

// A file name's type and kind, printing what it got on failure.
void expectFile(std::string_view name, std::string_view type, FileKind kind) {
    const auto mime = mimeTypeForFileName(name);
    if (mime.type != type || mime.kind != kind) {
        std::fprintf(stderr, "%.*s: got %.*s %s, expected %.*s %s\n",
                     static_cast<int>(name.size()), name.data(),
                     static_cast<int>(mime.type.size()), mime.type.data(), kindName(mime.kind),
                     static_cast<int>(type.size()), type.data(), kindName(kind));
        std::exit(1);
    }
}

void expectKind(std::string_view name, FileKind kind) {
    const auto mime = mimeTypeForFileName(name);
    if (mime.kind != kind) {
        std::fprintf(stderr, "%.*s: got %.*s %s, expected kind %s\n",
                     static_cast<int>(name.size()), name.data(),
                     static_cast<int>(mime.type.size()), mime.type.data(), kindName(mime.kind),
                     kindName(kind));
        std::exit(1);
    }
}

void testExtensionLookup() {
    EXPECT(mimeTypeForExtension("png") == "image/png");
    EXPECT(mimeTypeForExtension("PNG") == "image/png");
    EXPECT(mimeTypeForExtension("Jpg") == "image/jpeg");
    EXPECT(mimeTypeForExtension("pdf") == "application/pdf");
    EXPECT(mimeTypeForExtension("apk") == "application/vnd.android.package-archive");
    // shared-mime-info spells it audio/AMR; types are lower-cased.
    EXPECT(mimeTypeForExtension("AMR") == "audio/amr");
    // Case-sensitive in shared-mime-info: .C is C++, .c (in any case but "C") is C.
    EXPECT(mimeTypeForExtension("C") == "text/x-c++src");
    EXPECT(mimeTypeForExtension("c") == "text/x-csrc");
    // Additions in mime_policy.txt.
    EXPECT(mimeTypeForExtension("kts") == "text/x-kotlin");
    EXPECT(mimeTypeForExtension("SWIFT") == "text/x-swift");
    EXPECT(mimeTypeForExtension("BZ2") == "application/x-bzip2");
    EXPECT(mimeTypeForExtension("tar.gz") == "application/x-compressed-tar");
    EXPECT(mimeTypeForExtension("Tar.Zst") == "application/x-zstd-compressed-tar");
    EXPECT(mimeTypeForExtension("tar.nosuch").empty());
    EXPECT(mimeTypeForExtension("nosuchextension").empty());
    EXPECT(mimeTypeForExtension("").empty());
    EXPECT(mimeTypeForExtension(std::string_view("x").substr(0, 0)).empty());
    EXPECT(mimeTypeForExtension(std::string(200, 'a')).empty());
}

void testExtensionOf() {
    EXPECT(extensionOf("photo.JPG") == "JPG");
    EXPECT(extensionOf("a.b.c") == "c");
    EXPECT(extensionOf("noext").empty());
    EXPECT(extensionOf(".profile").empty());
    EXPECT(extensionOf("trailing.").empty());
    EXPECT(extensionOf("backup.tar.gz") == "tar.gz");
    EXPECT(extensionOf("backup.TAR.ZST") == "TAR.ZST");
    EXPECT(extensionOf("backup.tar.unknown") == "unknown");
    EXPECT(extensionOf(".tar.gz") == "gz");
    EXPECT(extensionOf("x.tar") == "tar");
    // Compound extensions are only archives' (tar.*, src.rpm); other compressed files and
    // two-part names go by the last part.
    EXPECT(extensionOf("pkg.src.rpm") == "src.rpm");
    EXPECT(extensionOf("x.svg.gz") == "gz");
    EXPECT(extensionOf("report.pdf.gz") == "gz");
    EXPECT(extensionOf("build.gradle.kts") == "kts");
    EXPECT(extensionOf("photo.backup.gz") == "gz");
    EXPECT(extensionOf("a.7z.001") == "001");
}

// What matters most on a phone: what opens where, and what Android apps filter intents on.
void testAndroidTypes() {
    using enum FileKind;
    for (const auto name : {"a.apk", "A.APK"}) {
        expectFile(name, "application/vnd.android.package-archive", kApk);
    }
    expectFile("a.3gp", "video/3gpp", kVideo);
    expectFile("a.3ga", "audio/3gpp", kAudio);
    expectFile("a.amr", "audio/amr", kAudio);
    expectFile("a.AMR", "audio/amr", kAudio);
    expectFile("a.awb", "audio/amr-wb", kAudio);
    expectFile("a.m4a", "audio/mp4", kAudio);
    expectFile("a.mp3", "audio/mpeg", kAudio);
    expectFile("a.aac", "audio/aac", kAudio);
    expectFile("a.flac", "audio/flac", kAudio);
    for (const auto name : {"a.ogg", "a.oga", "a.opus"}) {
        expectFile(name, "audio/ogg", kAudio);
    }
    expectFile("a.wav", "audio/x-wav", kAudio);
    expectFile("a.mid", "audio/midi", kAudio);
    expectFile("a.midi", "audio/midi", kAudio);
    expectFile("a.m3u", "audio/x-mpegurl", kAudio);
    expectFile("a.m3u8", "audio/x-mpegurl", kAudio);
    expectFile("a.mkv", "video/x-matroska", kVideo);
    expectFile("a.mka", "audio/x-matroska", kAudio);
    expectFile("a.mp4", "video/mp4", kVideo);
    expectFile("a.m4v", "video/mp4", kVideo);
    expectFile("a.webm", "video/webm", kVideo);
    expectFile("a.mov", "video/quicktime", kVideo);
    expectFile("a.avi", "video/avi", kVideo);
    expectFile("a.ts", "video/mp2t", kVideo);
    expectFile("a.m2ts", "video/mp2t", kVideo);

    expectFile("a.jpg", "image/jpeg", kImage);
    expectFile("a.jpeg", "image/jpeg", kImage);
    expectFile("a.png", "image/png", kImage);
    expectFile("a.webp", "image/webp", kImage);
    expectFile("a.gif", "image/gif", kImage);
    expectFile("IMG.HEIC", "image/heic", kImage);
    expectFile("a.heif", "image/heif", kImage);
    expectFile("a.heics", "image/heic-sequence", kImage);
    expectFile("a.avif", "image/avif", kImage);
    expectFile("a.jxl", "image/jxl", kImage);
    expectFile("a.dng", "image/x-adobe-dng", kImage);
    expectFile("a.svg", "image/svg+xml", kImage);
    expectFile("a.svgz", "image/svg+xml-compressed", kImage);
    // Compressed: gzip, which the archive browser opens, not the image viewer.
    expectFile("a.svg.gz", "application/gzip", kArchive);

    expectFile("a.pdf", "application/pdf", kPdf);
    expectFile("a.doc", "application/msword", kDocument);
    expectFile("a.docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
               kDocument);
    expectFile("a.odt", "application/vnd.oasis.opendocument.text", kDocument);
    expectFile("a.rtf", "application/rtf", kDocument);
    expectFile("a.xls", "application/vnd.ms-excel", kSpreadsheet);
    expectFile("a.xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
               kSpreadsheet);
    expectFile("a.ods", "application/vnd.oasis.opendocument.spreadsheet", kSpreadsheet);
    expectFile("a.csv", "text/csv", kSpreadsheet);
    expectFile("a.xlsm", "application/vnd.ms-excel.sheet.macroenabled.12", kSpreadsheet);
    expectFile("a.ppt", "application/vnd.ms-powerpoint", kPresentation);
    expectFile("a.pptx",
               "application/vnd.openxmlformats-officedocument.presentationml.presentation",
               kPresentation);
    expectFile("a.odp", "application/vnd.oasis.opendocument.presentation", kPresentation);
    expectFile("a.key", "application/vnd.apple.keynote", kPresentation);
    expectFile("a.epub", "application/epub+zip", kDocument);
    expectFile("a.mobi", "application/x-mobipocket-ebook", kDocument);
    expectFile("a.prc", "application/x-mobipocket-ebook", kDocument);
    expectFile("a.azw3", "application/vnd.amazon.mobi8-ebook", kDocument);
    expectFile("a.azw", "application/vnd.amazon.ebook", kDocument);
    expectFile("a.fb2", "application/x-fictionbook+xml", kDocument);
    expectFile("a.djvu", "image/vnd.djvu", kDocument);
    expectFile("a.cbz", "application/vnd.comicbook+zip", kDocument);
    expectFile("a.cbr", "application/vnd.comicbook-rar", kDocument);

    expectFile("a.txt", "text/plain", kText);
    expectFile("README.md", "text/markdown", kText);
    expectFile("a.log", "text/x-log", kText);
    expectFile("a.ini", "text/plain", kText);
    expectFile("a.json", "application/json", kCode);
    expectFile("a.xml", "application/xml", kCode);
    expectFile("a.yaml", "application/yaml", kCode);
    expectFile("a.yml", "application/yaml", kCode);
    expectFile("a.toml", "application/toml", kCode);
    expectFile("a.html", "text/html", kCode);
    expectFile("a.css", "text/css", kCode);
    expectFile("a.js", "text/javascript", kCode);
    expectFile("a.py", "text/x-python", kCode);
    expectFile("Main.kt", "text/x-kotlin", kCode);
    expectFile("a.kts", "text/x-kotlin", kCode);
    expectFile("a.swift", "text/x-swift", kCode);
    expectFile("a.java", "text/x-java", kCode);
    expectFile("a.go", "text/x-go", kCode);
    expectFile("a.rs", "text/rust", kCode);
    expectFile("a.sh", "text/x-shellscript", kCode);
    expectFile("main.c", "text/x-csrc", kCode);
    expectFile("MAIN.c", "text/x-csrc", kCode);
    expectFile("main.C", "text/x-c++src", kCode);
    expectFile("main.cpp", "text/x-c++src", kCode);
    expectFile("a.h", "text/x-chdr", kCode);

    expectFile("a.zip", "application/zip", kArchive);
    expectFile("a.rar", "application/vnd.rar", kArchive);
    expectFile("a.7z", "application/x-7z-compressed", kArchive);
    expectFile("a.tar", "application/x-tar", kArchive);
    expectFile("a.jar", "application/java-archive", kArchive);
    expectFile("a.deb", "application/vnd.debian.binary-package", kArchive);
    expectFile("a.cab", "application/vnd.ms-cab-compressed", kArchive);
    expectFile("a.gz", "application/gzip", kArchive);
    expectFile("a.bz2", "application/x-bzip2", kArchive);
    expectFile("a.xz", "application/x-xz", kArchive);
    expectFile("a.zst", "application/zstd", kArchive);
    expectFile("a.lz4", "application/x-lz4", kArchive);
    expectFile("a.lz", "application/x-lzip", kArchive);
    expectFile("a.lzma", "application/x-lzma", kArchive);
    expectFile("a.Z", "application/x-compress", kArchive);
    expectFile("a.bz", "application/x-bzip2", kArchive);
    // Compressed files that aren't archives are what they're compressed with: the last
    // extension, not shared-mime-info's compound types for them.
    expectFile("a.pdf.gz", "application/gzip", kArchive);
    expectFile("a.ps.gz", "application/gzip", kArchive);
    expectFile("a.xcf.gz", "application/gzip", kArchive);
    expectFile("a.img.xz", "application/x-xz", kArchive);
    expectFile("a.eps.bz2", "application/x-bzip2", kArchive);
    expectFile("a.pdf.xz", "application/x-xz", kArchive);
    // A split 7z volume: libarchive can't open multi-volume 7z.
    expectFile("a.7z.001", kGenericMimeType, kOther);
    for (const auto name : {"a.tgz", "a.tar.gz", "A.TAR.GZ"}) {
        expectFile(name, "application/x-compressed-tar", kArchive);
    }
    for (const auto name : {"a.tbz", "a.tbz2", "a.tar.bz2", "a.tar.bz"}) {
        expectFile(name, "application/x-bzip2-compressed-tar", kArchive);
    }
    expectFile("a.txz", "application/x-xz-compressed-tar", kArchive);
    expectFile("a.tar.xz", "application/x-xz-compressed-tar", kArchive);
    expectFile("a.tzst", "application/x-zstd-compressed-tar", kArchive);
    expectFile("a.tar.zst", "application/x-zstd-compressed-tar", kArchive);
    expectFile("a.tar.lz4", "application/x-lz4-compressed-tar", kArchive);
    expectFile("a.tlz", "application/x-lzma-compressed-tar", kArchive);
    expectFile("a.tar.lzma", "application/x-lzma-compressed-tar", kArchive);
    expectFile("a.tar.lz", "application/x-lzip-compressed-tar", kArchive);
    expectFile("a.tar.Z", "application/x-tarz", kArchive);
    expectFile("a.iso", "application/x-iso9660-image", kArchive);
    expectFile("a.rpm", "application/x-rpm", kArchive);
    expectFile("a.src.rpm", "application/x-source-rpm", kArchive);
    // libarchive is built without brotli and LZO.
    expectKind("a.tar.br", kOther);
    expectKind("a.tar.lzo", kOther);

    expectFile("a.ttf", "font/ttf", kFont);
    expectFile("a.otf", "font/otf", kFont);
    expectFile("a.woff2", "font/woff2", kFont);
    expectFile("a.eot", "application/vnd.ms-fontobject", kFont);

    expectFile("a.vcf", "text/vcard", kText);
    expectFile("a.ics", "text/calendar", kText);
    expectFile("a.srt", "application/x-subrip", kText);
    expectFile("a.vtt", "text/vtt", kText);
    // Mail opens in a mail app, not the editor.
    expectFile("a.eml", "message/rfc822", kOther);

    // The spellings Android's certificate installer filters on.
    expectFile("a.p12", "application/x-pkcs12", kOther);
    expectFile("a.pfx", "application/x-pkcs12", kOther);
    expectFile("a.crt", "application/x-x509-ca-cert", kOther);
    expectFile("a.cer", "application/x-x509-ca-cert", kOther);
    expectFile("a.cert", "application/x-x509-ca-cert", kOther);
    expectFile("a.der", "application/x-x509-ca-cert", kOther);
    expectFile("a.pem", "application/x-pem-file", kOther);

    expectFile("a.torrent", "application/x-bittorrent", kOther);
    expectFile("a.exe", "application/x-msdownload", kOther);
    expectFile("a.bin", kGenericMimeType, kOther);
    expectFile("noextension", kGenericMimeType, kOther);
    // A hidden file: ".tar.gz" is a gz file named ".tar", ".profile" has no extension.
    expectFile(".tar.gz", "application/gzip", kArchive);
    expectFile(".profile", kGenericMimeType, kOther);
    expectFile("trailing.", kGenericMimeType, kOther);
}

void testKinds() {
    EXPECT(directoryMimeType().kind == FileKind::kDirectory);
    EXPECT(fileKindForMimeType(kDirectoryMimeType) == FileKind::kDirectory);
    EXPECT(fileKindForMimeType("image/png") == FileKind::kImage);
    EXPECT(fileKindForMimeType("IMAGE/PNG") == FileKind::kImage);
    // Aliases get their type's kind.
    EXPECT(fileKindForMimeType("application/x-gzip") == FileKind::kArchive);
    EXPECT(fileKindForMimeType("text/xml") == FileKind::kCode);
    EXPECT(fileKindForMimeType("audio/x-wav") == FileKind::kAudio);
    EXPECT(fileKindForMimeType("application/x-zip-compressed") == FileKind::kArchive);
    EXPECT(fileKindForMimeType("application/vnd.ms-excel.sheet.macroEnabled.12")
           == FileKind::kSpreadsheet);
    // Zip-based formats aren't archives.
    EXPECT(fileKindForMimeType("application/vnd.android.package-archive") == FileKind::kApk);
    EXPECT(fileKindForMimeType("application/epub+zip") == FileKind::kDocument);
    // Unknown types go by their top-level type.
    EXPECT(fileKindForMimeType("video/x-nosuch") == FileKind::kVideo);
    EXPECT(fileKindForMimeType("text/x-nosuch") == FileKind::kText);
    EXPECT(fileKindForMimeType("application/x-nosuch") == FileKind::kOther);
    EXPECT(fileKindForMimeType("") == FileKind::kOther);
    EXPECT(fileKindForMimeType(std::string(500, 'a')) == FileKind::kOther);
    // Media kinds come only from the top-level type, so decoders don't get Ogg containers,
    // RealMedia or 3D models.
    EXPECT(fileKindForMimeType("application/ogg") != FileKind::kVideo);
    EXPECT(fileKindForMimeType("application/ogg") != FileKind::kAudio);
    EXPECT(fileKindForMimeType("application/vnd.rn-realmedia") == FileKind::kOther);
    // An image icon in shared-mime-info, but text.
    EXPECT(fileKindForMimeType("model/obj") == FileKind::kText);
    // The comment-based Code rule: languages shared-mime-info gives no icon.
    for (const auto type : {"text/x-go", "text/x-java", "text/x-kotlin", "text/x-scala",
                            "text/rust", "text/x-haskell", "text/x-pascal", "text/x-tcl",
                            "text/x-nix"}) {
        if (fileKindForMimeType(type) != FileKind::kCode) {
            std::fprintf(stderr, "%s: not Code\n", type);
            std::exit(1);
        }
    }
}

void testArchives() {
    for (const auto name : {"a.zip", "a.7z", "a.rar", "a.tar", "a.tgz", "a.gz", "a.xz", "a.zst",
                            "a.bz2", "a.iso", "a.jar", "a.tar.gz", "a.TAR.BZ2", "a.tar.xz",
                            "a.tar.zst", "a.tar.lz4", "a.tar.lzma", "a.tbz2", "a.txz", "a.rpm",
                            "a.deb", "a.cab", "a.ustar", "a.cpio"}) {
        EXPECT(isArchiveName(name));
    }
    for (const auto name : {"a.apk", "a.txt", "tar", "a.tar.txt", "gz", "a.epub", "a.docx",
                            "a.cbz", "a.tar.br", "a.tar.lzo", "a.tar.bz3", "a.br",
                            "a.7z.001", "a.zip.001"}) {
        EXPECT(!isArchiveName(name));
    }
}

bool isLower(std::string_view text) {
    for (const char c : text) {
        if (c >= 'A' && c <= 'Z') {
            return false;
        }
    }
    return true;
}

void testTableInvariants() {
    EXPECT(table::kTypes.size() > 1000);
    EXPECT(table::kExtensions.size() > 1000);
    // Only archives: tar.*, cpio.gz, src.rpm.
    EXPECT(table::kCompoundExtensions.size() >= 10);
    for (const auto &entry : table::kTypes) {
        EXPECT(isLower(entry.type));
        EXPECT(entry.type.contains('/'));
        // Media kinds follow the top-level type.
        const auto media = entry.kind == FileKind::kImage   ? "image/"
                           : entry.kind == FileKind::kVideo ? "video/"
                           : entry.kind == FileKind::kAudio ? "audio/"
                                                            : nullptr;
        if (media && !entry.type.starts_with(media)) {
            std::fprintf(stderr, "%.*s: %s\n", static_cast<int>(entry.type.size()),
                         entry.type.data(), kindName(entry.kind));
            std::exit(1);
        }
        // The table's types are what fileKindForMimeType answers for.
        EXPECT(fileKindForMimeType(entry.type) == entry.kind);
    }
    for (const auto extensions : {std::span<const table::ExtEntry>(table::kExtensions),
                                  std::span<const table::ExtEntry>(table::kCompoundExtensions)}) {
        for (const auto &entry : extensions) {
            EXPECT(isLower(entry.extension));
            EXPECT(mimeTypeForExtension(entry.extension) == table::kTypes[entry.type].type);
        }
    }
    for (const auto &entry : table::kCompoundExtensions) {
        EXPECT(entry.extension.contains('.'));
        // Compound extensions are only for archives.
        EXPECT(table::kTypes[entry.type].kind == FileKind::kArchive);
        // The compound extension wins over the last part alone.
        const std::string name = "x." + std::string(entry.extension);
        EXPECT(extensionOf(name) == entry.extension);
        EXPECT(mimeTypeForFileName(name).type == table::kTypes[entry.type].type);
    }
    for (const auto &entry : table::kCaseSensitiveExtensions) {
        EXPECT(!isLower(entry.extension));
        EXPECT(mimeTypeForExtension(entry.extension) == table::kTypes[entry.type].type);
    }
    for (const auto &entry : table::kAliases) {
        EXPECT(isLower(entry.alias));
        EXPECT(fileKindForMimeType(entry.alias) == table::kTypes[entry.type].kind);
    }
}

}  // namespace

int main() {
    testExtensionLookup();
    testExtensionOf();
    testAndroidTypes();
    testKinds();
    testArchives();
    testTableInvariants();
    return 0;
}
