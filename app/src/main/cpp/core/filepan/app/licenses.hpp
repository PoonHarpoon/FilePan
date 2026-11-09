// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "filepan/error.hpp"

// The open source notices: assets/licenses/licenses.xml lists each component FilePan ships
// (<notice>: name, url, copyright, license) and maps each license name to its full text, a file
// beside it (<text license="..." file="..."/>).
namespace filepan::app {

inline constexpr const char *kLicensesDir = "licenses";
inline constexpr const char *kLicensesFile = "licenses/licenses.xml";

struct Notice {
    std::string name;
    std::string url;
    std::string copyright;
    std::string license;

    bool operator==(const Notice &) const = default;
};

struct LicenseText {
    std::string license;
    std::string file;  // Relative to the licenses directory.

    bool operator==(const LicenseText &) const = default;
};

struct Licenses {
    std::vector<Notice> notices;
    std::vector<LicenseText> texts;

    // The text file for a license name; empty if there's none.
    [[nodiscard]] std::string fileFor(std::string_view license) const;
    // Notices whose license has no text, as "name: license".
    [[nodiscard]] std::vector<std::string> missingTexts() const;
};

// Parses licenses.xml: the small XML subset it uses (elements, attributes in double quotes,
// comments, the XML declaration and the five predefined entities plus numeric references).
// EINVAL on malformed input.
Result<Licenses> parseLicenses(std::string_view xml);

}  // namespace filepan::app
