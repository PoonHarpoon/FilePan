// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <span>

// Transfers on a Linux usbfs file descriptor (on Android, UsbDeviceConnection's).
namespace filepan::securitykey {

// One bulk or interrupt transfer on endpoint (its address, with the direction bit), waiting up
// to timeoutMillis. Returns the bytes moved, 0 on timeout, or -1 on failure.
int usbfsTransfer(int fd, std::uint8_t endpoint, std::span<std::uint8_t> data, int timeoutMillis);

}  // namespace filepan::securitykey
