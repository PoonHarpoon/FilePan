// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <memory>
#include <vector>

#include "filepan/app/app.hpp"
#include "filepan/ui/scroll.hpp"

// The storage home (glass-signal-mobile.md, "Other screens"): the all-files-access gate when
// it's needed, a glass card per mounted volume with a meter ring for the space used (corner ticks
// on the primary one), the favorites (favorites_sheet.hpp), and the saved SFTP servers with the way to manage them. Its background is the only one
// with the 48 dp grid; the App draws that.
namespace filepan::app {

class HomeScreen final : public ui::Node {
public:
    static std::shared_ptr<HomeScreen> create(AppContext &context);
    explicit HomeScreen(AppContext &context) : context_(context) {}

    [[nodiscard]] ui::ScrollNode &scroller() const { return *scroll_; }
    // Null when access is granted.
    [[nodiscard]] ui::Node *permissionCard() const { return permission_; }
    [[nodiscard]] const std::vector<ui::Node *> &volumeCards() const { return volumes_; }
    [[nodiscard]] const std::vector<ui::Node *> &favoriteRows() const { return favorites_; }
    [[nodiscard]] ui::Node *serversRow() const { return servers_; }
    // One row per saved server.
    [[nodiscard]] const std::vector<ui::Node *> &serverRows() const { return serverRows_; }

    void openServers();

private:
    void construct();
    void rebuild();

    AppContext &context_;
    std::shared_ptr<ui::ScrollNode> scroll_;
    std::shared_ptr<ui::Node> column_;
    ui::Node *permission_ = nullptr;
    std::vector<ui::Node *> volumes_;
    std::vector<ui::Node *> favorites_;
    ui::Node *servers_ = nullptr;
    std::vector<ui::Node *> serverRows_;
};

}  // namespace filepan::app
