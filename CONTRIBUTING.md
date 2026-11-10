# Contributing to FilePan

FilePan welcomes contributions: bug reports, fixes, features and testing on real devices. The UI is
English-only while it's being rebuilt, so translations will have to wait.

## Licensing of contributions

FilePan's own code is licensed under the [GNU Affero General Public License, version 3 or
later](LICENSE). Third-party components keep their own licenses; see [NOTICE](NOTICE).

By submitting a contribution, you agree that it is licensed under AGPL-3.0-or-later, together with
the additional permission for app stores set out in [NOTICE](NOTICE). You also
confirm that you have the right to submit it under those terms. There is no contributor license
agreement. You keep the copyright to your work.

## How a pull request lands

Pull requests are reviewed and merged here as usual, and your commits keep your name.

Day-to-day development happens in a separate repository, and each release arrives here as a single
commit. Merged pull requests are carried over to that repository with you as their author, so the
next release includes your work. When your pull request is merged, you're added to
[CONTRIBUTORS.md](CONTRIBUTORS.md) and [AUTHORS](AUTHORS).

## License headers

Every source file starts with a license header. Check it with
`utils/check-license-headers.sh` before committing.

A new file:

```kotlin
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors
```

Files you edit already carry this header. XML uses the same lines with ` ~ ` prefixes, and CMake,
Nix and shell use `#`. See [NOTICE](NOTICE).

## Building and testing

    git submodule update --init --recursive
    nix develop
    ./gradlew testDebugUnitTest assembleDebug lintVitalRelease

Native core tests run on the host:

    cmake -S app/src/main/cpp/host -B build/host -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build/host
    ctest --test-dir build/host --output-on-failure

The golden images those tests compare against are in Git LFS: run `git lfs install` before you
clone, or `git lfs pull` in a clone made without it.

## Commits

Keep each commit to one change, with a short imperative subject line, such as "Fix a leak when
archive creation is aborted". Start build-only changes with `Build:`. Use the body to explain
why the change is needed when that isn't obvious.

## Security issues

Don't open public issues for vulnerabilities; see [SECURITY.md](SECURITY.md).
