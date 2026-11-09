// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include "filepan/app/app.hpp"
#include "filepan/app/backup.hpp"

// Backing up and restoring the settings (backup.hpp) from the settings screen.
//
// - Back up: a sheet with an optional password (twice); saving writes the backup off the UI
//   thread and hands it to the system's picker for where to put it.
// - Restore: the system's picker for the file; an encrypted backup asks for its password (or
//   goes on without the secrets); then a sheet says what it holds and confirms replacing the
//   current settings.
namespace filepan::app {

void showBackupSheet(AppContext &context);
void startRestore(AppContext &context);

// Replaces the settings with a restored backup's, keeping what belongs to this device: the open
// tabs, and whether the intro was seen. Servers point at this device's known_hosts, a server
// whose backup had no secrets keeps the ones saved here for it, and the backup's known hosts are
// added to this device's. Then a toast says so.
void applyRestore(AppContext &context, RestoredBackup backup);

}  // namespace filepan::app
