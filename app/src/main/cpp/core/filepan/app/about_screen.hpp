// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "filepan/app/app.hpp"
#include "filepan/app/licenses.hpp"
#include "filepan/app/screen_parts.hpp"

// About, the open source notices, and one license's full text. The one place the marks are
// generous (glass-signal-mobile.md): the wordmark, build coordinates, a barcode, a hazard band.
//
//   "about"     The wordmark and logo mark, version and build in mono coordinates, the links
//               (source repository, FilePan's license) and the way to the licenses.
//   "licenses"  Every notice from assets/licenses/licenses.xml: name, copyright and license.
//   "license"   A notice's license in full, read from its text file.
namespace filepan::app {

inline constexpr const char *kAboutScreen = "about";
inline constexpr const char *kLicensesScreen = "licenses";
inline constexpr const char *kLicenseScreen = "license";
inline constexpr const char *kSourceUrl = "https://github.com/PoonHarpoon/FilePan";

std::vector<ScreenSpec> aboutScreenSpecs();

// Something read on the I/O pool: not yet, the value, or why not.
template <typename T>
struct Loaded {
    bool done = false;
    T value{};
    std::string error;
};

// What the license screen shows.
struct LicenseChoice {
    std::string title;    // The component: "FilePan", "libssh".
    std::string license;  // Its license's name, as licenses.xml spells it.
    std::string url;
    std::string copyright;
};

class LicensesScreen final : public ui::Node {
public:
    explicit LicensesScreen(AppContext &context) : context_(context) {}
    void construct(std::shared_ptr<std::optional<LicenseChoice>> choice);

    // Tracked: the parsed file once loaded, or the error.
    [[nodiscard]] const ui::Signal<Loaded<Licenses>> &licenses() const { return licenses_; }
    [[nodiscard]] const std::vector<ui::Node *> &rows() const { return rows_; }
    void open(const Notice &notice);

private:
    void rebuild();

    AppContext &context_;
    ScrollPage page_;
    std::shared_ptr<std::optional<LicenseChoice>> choice_;
    ui::Signal<Loaded<Licenses>> licenses_;
    std::vector<ui::Node *> rows_;
};

class LicenseScreen final : public ui::Node {
public:
    explicit LicenseScreen(AppContext &context) : context_(context) {}
    void construct(std::optional<LicenseChoice> choice);

    // Tracked: the license text once loaded, or the error.
    [[nodiscard]] const ui::Signal<Loaded<std::string>> &text() const { return text_; }

private:
    void rebuild();

    AppContext &context_;
    ScrollPage page_;
    std::optional<LicenseChoice> choice_;
    ui::Signal<Loaded<std::string>> text_;
};

// Reads and parses licenses.xml through the services' assets (on an I/O thread).
Result<Licenses> loadLicenses(const runtime::AssetSource *assets);

}  // namespace filepan::app
