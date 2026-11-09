// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

// ICU's C API, the same on both platforms. Android's NDK exposes the system ICU4C from API 31 as
// libicu.so with unversioned symbols; the host links the dev shell's ICU, whose headers rename
// every symbol with its version. FilePan only uses the C API that the NDK has (CMake defines
// U_SHOW_CPLUSPLUS_API=0 on the host to match the NDK), and only through this header.

#include <memory>
#include <string>

#include <unicode/ubidi.h>
#include <unicode/ubrk.h>
#include <unicode/uchar.h>
#include <unicode/ucol.h>
#include <unicode/ustring.h>
#include <unicode/utf16.h>
#include <unicode/utypes.h>

namespace filepan::text::icu {

struct BiDiDeleter {
    void operator()(UBiDi *bidi) const { ubidi_close(bidi); }
};
using BiDiPtr = std::unique_ptr<UBiDi, BiDiDeleter>;

struct BreakIteratorDeleter {
    void operator()(UBreakIterator *iterator) const { ubrk_close(iterator); }
};
using BreakIteratorPtr = std::unique_ptr<UBreakIterator, BreakIteratorDeleter>;

// ICU takes POSIX-style locale IDs; it accepts BCP 47's hyphens as well.
inline BreakIteratorPtr openBreakIterator(UBreakIteratorType type, const std::string &locale) {
    UErrorCode status = U_ZERO_ERROR;
    UBreakIterator *iterator = ubrk_open(type, locale.c_str(), nullptr, 0, &status);
    if (U_FAILURE(status)) {
        ubrk_close(iterator);
        return nullptr;
    }
    return BreakIteratorPtr(iterator);
}

}  // namespace filepan::text::icu
