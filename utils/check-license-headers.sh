#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 FilePan contributors

# Checks license headers on source files changed since a base revision:
# every changed source file must carry the AGPL-3.0-or-later header, and a file
# carrying a Material Files header is an error: FilePan files are
# AGPL-3.0-or-later. Only changed files are checked.
#
# Usage: utils/check-license-headers.sh [base]   (default: HEAD, i.e. uncommitted changes,
#                                                 including untracked files)

set -e

base="${1:-HEAD}"
if ! git rev-parse -q --verify "$base^{commit}" >/dev/null; then
    # A new branch push has no "before" revision; check the last commit only.
    base="HEAD~1"
fi

changed_files() {
    git diff --name-only --no-renames --diff-filter=AM "$base"
    if [[ "$base" == HEAD ]]; then
        git ls-files --others --exclude-standard
    fi
}

failed=0
while IFS= read -r file; do
    case "$file" in
        third_party/*|gradle/wrapper/*) continue ;;
        *.kt|*.java|*.aidl|*.pro|*.c|*.cc|*.cpp|*.h|*.hpp|*.xml|*.gradle|*.cmake|*.nix|*.sh|*.py|*.properties) ;;
        CMakeLists.txt|*/CMakeLists.txt|*/mime_policy.txt) ;;
        *) continue ;;
    esac
    header="$(head -n 12 "$file")"
    if grep -q 'Hai Zhang' <<<"$header"; then
        echo "$file: Material Files header; FilePan files are AGPL-3.0-or-later"
        failed=1
    elif ! grep -q 'SPDX-License-Identifier: AGPL-3.0-or-later' <<<"$header"; then
        echo "$file: missing SPDX-License-Identifier: AGPL-3.0-or-later"
        failed=1
    fi
done < <(changed_files | sort -u)

if [[ "$failed" -ne 0 ]]; then
    echo
    echo 'See CONTRIBUTING.md, "License headers".'
    exit 1
fi
