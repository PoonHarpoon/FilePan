// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "filepan/size_format.hpp"
#include "filepan/ui/components/rows.hpp"
#include "filepan/vfs/entry.hpp"
#include "filepan/vfs/location.hpp"

// Text the screens show for files: sizes, dates, meta lines, kinds, and the string form of a
// Location that settings store.
namespace filepan::app {

using filepan::formatSize;
using filepan::formatSizeOf;
// "12 Oct 2026" in local time.
std::string formatDate(vfs::Timestamp time);
// "12 Oct" in the current year, "12 Oct 2025" otherwise: what a row's meta line shows.
std::string formatShortDate(vfs::Timestamp time);
// The local year formatShortDate leaves out. Tests pin it so renders don't change with the
// clock; nullopt goes back to the clock.
int currentYear();
void setCurrentYearForTesting(std::optional<int> year);
// "8 items", "1 item".
std::string formatCount(std::uint64_t count);
// A file's extension: what follows the last dot, upper-cased ("ZST" for "x.tar.zst"); empty for
// none.
std::string displayExtension(std::string_view name);

// A row's meta line: `2.4 MB · 12 Oct` for a file, `8 items · 3 Oct` for a folder (just the date
// until its items are counted); dates outside the current year keep it.
std::string metaLine(const vfs::Entry &entry, std::optional<std::uint64_t> itemCount);
// The utilitarian row's size column: a file's size, a folder's item count once counted.
std::string sizeColumn(const vfs::Entry &entry, std::optional<std::uint64_t> itemCount);
// What a screen reader says for a row: name, type, size and date.
std::string spokenDescription(const vfs::Entry &entry, std::optional<std::uint64_t> itemCount);

// The kit's file kind for an entry, picking its icon and tint.
ui::components::FileKind kitKind(const vfs::Entry &entry);
// "Folder", "PDF document", "Image"...
std::string kindName(const vfs::Entry &entry);

// A Location as one line of printable ASCII with no spaces, and back. Names are raw bytes, so
// everything outside printable ASCII is percent-encoded. Archive locations nest.
std::string encodeLocation(const vfs::Location &location);
std::optional<vfs::Location> decodeLocation(std::string_view text);

// Percent-encoding of arbitrary bytes into a space-free token: everything but [A-Za-z0-9._~/-]
// becomes %XX. decode returns nullopt for a malformed escape.
std::string percentEncode(std::string_view bytes);
std::optional<std::string> percentDecode(std::string_view token);

}  // namespace filepan::app
