// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "filepan/app/app.hpp"
#include "filepan/app/file_actions.hpp"
#include "filepan/ui/components/components.hpp"
#include "filepan/ui/components/text_editor.hpp"
#include "filepan/vfs/save.hpp"

// The full-screen text viewer and editor, over everything like the media viewer.
//
// - The file is read on the I/O pool and decoded (text_document.hpp); a binary file says so and
//   offers "Open with".
// - The top bar: back, the name over `UTF-8 · LF · 120 lines`, save (while there are changes),
//   wrap, and more (select all, copy, cut, paste, open with, properties).
// - Files in archives, on read-only storage, or over 8 MiB open read-only.
// - Saving writes the file in one piece (vfs/save.hpp). If something else changed the file since
//   it was read, it asks before overwriting; back with unsaved changes asks to save them.
namespace filepan::app {

// What the editor opens from the file list: text and source files, by MIME type.
bool isTextFile(const vfs::Entry &entry);

// Larger files open read-only; past kMaxTextOpen they don't open at all.
inline constexpr std::uint64_t kMaxTextEdit = 8ull << 20;
inline constexpr std::uint64_t kMaxTextOpen = 64ull << 20;

class TextScreen final : public ui::Node {
public:
    enum class Status : std::uint8_t { loading, ready, failed };

    // file: the file's folder and its one entry.
    static std::shared_ptr<TextScreen> create(AppContext &context, FileTargets file, std::function<void()> onClose);
    TextScreen(AppContext &context, FileTargets file, std::function<void()> onClose);

    // System back: asks about unsaved changes first.
    void back();
    // Writes the file; then runs once it's saved.
    void save(std::function<void()> then = {});

    [[nodiscard]] Status status() const { return status_.peek(); }
    [[nodiscard]] const std::string &errorMessage() const { return error_; }
    // Tracked.
    [[nodiscard]] bool modified() const { return modified_(); }
    [[nodiscard]] bool saving() const { return saving_.peek(); }
    [[nodiscard]] ui::components::TextEditor *editor() const { return editor_.get(); }
    [[nodiscard]] ui::Node &backButton() const { return *back_; }
    [[nodiscard]] ui::Node &saveButton() const { return *save_; }
    [[nodiscard]] ui::Node &wrapButton() const { return *wrap_; }
    [[nodiscard]] ui::Node &moreButton() const { return *more_; }
    [[nodiscard]] ui::Text &subtitle() const { return *subtitle_; }

private:
    struct Loaded;

    void construct();
    void load();
    void loaded(Loaded loaded);
    void showMore();
    void write(std::vector<std::uint8_t> bytes, std::optional<vfs::FileStamp> expected, std::uint64_t revision,
               std::function<void()> then);
    void close();
    [[nodiscard]] vfs::Location location() const;

    AppContext &context_;
    FileTargets file_;
    std::function<void()> onClose_;
    ui::Signal<Status> status_{Status::loading};
    ui::Signal<bool> modified_{false};
    ui::Signal<bool> saving_{false};
    ui::Signal<bool> wrapping_{true};
    std::string error_;
    bool readOnly_ = true;
    std::optional<vfs::FileStamp> stamp_;
    std::uint64_t savedRevision_ = 0;

    std::shared_ptr<ui::Node> top_;
    std::shared_ptr<ui::Node> back_;
    std::shared_ptr<ui::Text> subtitle_;
    std::shared_ptr<ui::Node> save_;
    std::shared_ptr<ui::Node> wrap_;
    std::shared_ptr<ui::Node> more_;
    std::shared_ptr<ui::Node> body_;
    std::shared_ptr<ui::components::TextEditor> editor_;
};

}  // namespace filepan::app
