// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <atomic>
#include <memory>

namespace filepan {

// A shared cancellation flag. Copies observe the same flag, so a job can hand its token to the
// operations it runs while the UI keeps a copy to cancel it. Cancelling is permanent.
class CancellationToken {
public:
    CancellationToken() : flag_(std::make_shared<std::atomic<bool>>(false)) {}

    void cancel() const { flag_->store(true, std::memory_order_relaxed); }
    [[nodiscard]] bool isCancelled() const { return flag_->load(std::memory_order_relaxed); }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

}  // namespace filepan
