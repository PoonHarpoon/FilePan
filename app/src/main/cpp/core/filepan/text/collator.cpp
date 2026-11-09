// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/text/collator.hpp"

#include <limits>

#include "filepan/text/icu.hpp"

namespace filepan::text {

struct Collator::Impl {
    UCollator *collator = nullptr;

    ~Impl() { ucol_close(collator); }
};

std::unique_ptr<Collator> Collator::create(const std::string &locale, Options options) {
    UErrorCode status = U_ZERO_ERROR;
    UCollator *collator = ucol_open(locale.c_str(), &status);
    if (U_FAILURE(status)) {
        ucol_close(collator);
        status = U_ZERO_ERROR;
        collator = ucol_open("", &status);
        if (U_FAILURE(status)) {
            ucol_close(collator);
            return nullptr;
        }
    }
    status = U_ZERO_ERROR;
    ucol_setAttribute(collator, UCOL_NUMERIC_COLLATION, options.numeric ? UCOL_ON : UCOL_OFF, &status);
    auto impl = std::make_unique<Impl>();
    impl->collator = collator;
    return std::unique_ptr<Collator>(new Collator(std::move(impl)));
}

Collator::Collator(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Collator::~Collator() = default;

int Collator::compare(std::string_view a, std::string_view b) const {
    constexpr std::size_t kMax = std::numeric_limits<std::int32_t>::max();
    if (a.size() <= kMax && b.size() <= kMax) {
        UErrorCode status = U_ZERO_ERROR;
        const UCollationResult result =
                ucol_strcollUTF8(impl_->collator, a.data(), static_cast<std::int32_t>(a.size()), b.data(),
                                 static_cast<std::int32_t>(b.size()), &status);
        if (U_SUCCESS(status) && result != UCOL_EQUAL) {
            return result == UCOL_LESS ? -1 : 1;
        }
    }
    const int order = a.compare(b);
    return (order > 0) - (order < 0);
}

}  // namespace filepan::text
