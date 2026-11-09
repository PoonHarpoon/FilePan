// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/securitykey/usbfs.hpp"

#include <algorithm>
#include <cerrno>

#include <linux/usbdevice_fs.h>
#include <sys/ioctl.h>

namespace filepan::securitykey {

int usbfsTransfer(int fd, std::uint8_t endpoint, std::span<std::uint8_t> data, int timeoutMillis) {
    usbdevfs_bulktransfer transfer{};
    transfer.ep = endpoint;
    transfer.len = static_cast<unsigned>(data.size());
    // 0 would wait forever.
    transfer.timeout = static_cast<unsigned>(std::max(timeoutMillis, 1));
    transfer.data = data.data();
    int moved;
    do {
        moved = ::ioctl(fd, USBDEVFS_BULK, &transfer);
    } while (moved < 0 && errno == EINTR);
    if (moved < 0) {
        return errno == ETIMEDOUT ? 0 : -1;
    }
    return moved;
}

}  // namespace filepan::securitykey
