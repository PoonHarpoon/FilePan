<p align="center">
  <img src="art/icon/filepan.svg" width="128" alt="The FilePan logo: a black pan holding a white page, on an acid green squircle">
</p>

<h1 align="center">FilePan</h1>

A fast file manager for Android, with a native C++ core and its own Vulkan interface.

Written to be fast and usable for both local and remote file browsing, including using latest SSH/SFTP
features and supporting jump hosts natively.

<p align="center">
  <img src="art/screenshots/home.png" width="38%" alt="The main screen: storage use, favorites and SFTP servers">
  <img src="art/screenshots/home-clean.png" width="38%" alt="The same main screen in the simple Clean theme">
</p>

<p>
  <img src="art/screenshots/browse.png" width="32%" alt="Browsing a folder with thumbnails">
  <img src="art/screenshots/disk-usage.png" width="32%" alt="The disk usage map of a folder">
  <img src="art/screenshots/sftp.png" width="32%" alt="A list of saved SFTP servers">
</p>

## What it does

| Area | Features |
|---|---|
| Browsing | Lists and grids, tabs, filtering, multi-selection, favorites and fast search |
| File jobs | Copy, move and delete in the background, with size, rate and time left |
| Thumbnails | Images, video and APKs, on servers too |
| Checksums | Computed in parallel with the CPU's hardware instructions |
| Archives | Browse, extract, create and verify through libarchive, with multithreaded xz |
| Viewer | Full-resolution images, GIF, animated WebP and AVIF, and hardware-decoded video |
| Text editor | Large files, keeps encodings and line endings, asks before overwriting a file changed elsewhere |
| SFTP | Trust-on-first-use host keys, FIDO security keys, pipelined transfers and a mobile data mode |
| Themes | Acid and Ultraviolet dark themes, and Clean, which follows the system's light or dark mode, with any accent colour |
| Settings | Password-encrypted backup and restore |
| Accessibility | TalkBack support |

Implements up to date SSH post-quantum and Jump Hosts, including support for smart cards and YubiKey.

## Experimental

Under Settings → Experimental:

- **Disk usage** (on by default): a map of what takes the space in a folder, by folder or file
  type, and a way to clean it up.
- **System storage** (off by default): folders picked through the system, such as USB drives and
  other apps' storage.

Root and Shizuku access are gone for now and may return. SMB, FTP, WebDAV, the FTP server and
terminal integration have been removed.

## Install

Download the latest APK from the [releases page](https://github.com/PoonHarpoon/FilePan/releases).
FilePan needs an arm64 device running Android 16 or later.

### Verify the APK

Every release is signed with the same key. To check a download, run:

    apksigner verify --print-certs FilePan-0.1.0-arm64-v8a.apk

It should report this SHA-256 digest:

    258cccf7c7385b23c788a63514de34c0ca45c243ae4c13e2eaf0184ff81c1b32

## Build

Native dependencies are git submodules under `third_party/`, each pinned to a release tag, and are
built from source as part of the native build.

### With Nix (supported)

This builds the debug APK from a checkout and prints its path. It fetches the git submodules if
needed:

    nix run

Arguments replace the Gradle task, for example `nix run . -- assembleRelease`.

For development, the flake also provides a shell:

    git submodule update --init --recursive
    nix develop   # or `direnv allow` with nix-direnv
    ./gradlew assembleDebug

### Native compilation (not tested)

Without Nix, you'll need JDK 21, CMake, Ninja and the Android SDK with platform 37.2, build tools
37.0.0 and NDK 30.0.16248370. With `ANDROID_HOME` set:

    git submodule update --init --recursive
    ./gradlew assembleDebug

This route isn't tested, so expect to adjust it.

## Contribute

Contributions are welcome: see [CONTRIBUTING.md](CONTRIBUTING.md). The people behind FilePan are
listed in [CONTRIBUTORS.md](CONTRIBUTORS.md). To report a vulnerability, see
[SECURITY.md](SECURITY.md).

## Credits

FilePan is based on the work done on [Material Files](https://github.com/zhanghai/MaterialFiles) by
Hai Zhang, unassociated with this project. The disk usage view is inspired by
[QDirStat](https://github.com/shundhammer/qdirstat) and KDirStat by Stefan Hundhammer, and shares no
code with them.

## License

FilePan is licensed under the [GNU Affero General Public License, version 3 or later](LICENSE).
Third-party components keep their own licenses; they are listed in
[licenses.xml](app/src/main/assets/licenses/licenses.xml) and shown in the app under
About > Licenses. See [NOTICE](NOTICE) for details.

FilePan's own code also carries an app-store exception under section 7 of the AGPL, set out in
[NOTICE](NOTICE): you may distribute it through third-party stores, as long as its source stays
freely available and the store listing or the app says where to get it.

    Copyright (C) 2026 FilePan contributors (see AUTHORS)
