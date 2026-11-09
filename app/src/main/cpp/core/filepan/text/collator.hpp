// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <memory>
#include <string>
#include <string_view>

// Locale-aware string comparison on ICU's collator, for sorting file names the way people
// expect: "file2" before "file10" (numeric ordering), accents and case as the locale orders them.
namespace filepan::text {

class Collator {
public:
    struct Options {
        // Digit runs compare by numeric value.
        bool numeric = true;
    };

    // locale is BCP 47 ("en-US", "de"); an unknown one falls back to ICU's root collation.
    // Returns null only if ICU can't open any collator.
    static std::unique_ptr<Collator> create(const std::string &locale, Options options);
    static std::unique_ptr<Collator> create(const std::string &locale) { return create(locale, {}); }
    ~Collator();
    Collator(const Collator &) = delete;
    Collator &operator=(const Collator &) = delete;

    // Negative, zero or positive. Strings ICU considers equal (differing only in ignorable
    // characters, or invalid UTF-8 replaced alike) compare by their bytes, so only identical
    // strings return zero. Thread-safe: ICU collators may be shared for comparison.
    [[nodiscard]] int compare(std::string_view a, std::string_view b) const;

    struct Impl;

private:
    explicit Collator(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace filepan::text
