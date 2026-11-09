// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

// The whole UI framework, for the component kit and screens.
//
// Where to start reading:
//   reactive.hpp      Signal, Memo, Effect, Scope, batch: fine-grained reactivity.
//   node.hpp          Node, Box, Text, Image: the retained tree, dirtiness, recording.
//   style.hpp         Style: the flexbox subset; layout.hpp applies it.
//   for_each.hpp      Keyed children for short dynamic lists.
//   scroll.hpp        ScrollNode, fling and overscroll.
//   virtual_list.hpp  VirtualList: lists and grids of any length.
//   gesture.hpp       The gesture arena: tap, long press, drag; VelocityTracker.
//   animation.hpp     FrameClock, AnimatedValue, springs and easing.
//   semantics.hpp     Accessibility semantics and their wire format.
//   focus.hpp         Keyboard focus; text_input.hpp: the IME contract.
//   environment.hpp   Density, insets, font scale, reduced motion, theme as signals.
//   theme.hpp         Glass Signal tokens and contrast helpers.
//   ui_loop.hpp       The UI thread's task queue and the I/O pool.
//   ui_root.hpp       UiRoot: events in, frames and semantics out.

#include "filepan/ui/animation.hpp"
#include "filepan/ui/environment.hpp"
#include "filepan/ui/focus.hpp"
#include "filepan/ui/for_each.hpp"
#include "filepan/ui/gesture.hpp"
#include "filepan/ui/input.hpp"
#include "filepan/ui/layout.hpp"
#include "filepan/ui/node.hpp"
#include "filepan/ui/reactive.hpp"
#include "filepan/ui/scroll.hpp"
#include "filepan/ui/semantics.hpp"
#include "filepan/ui/style.hpp"
#include "filepan/ui/text_input.hpp"
#include "filepan/ui/theme.hpp"
#include "filepan/ui/ui_loop.hpp"
#include "filepan/ui/ui_root.hpp"
#include "filepan/ui/virtual_list.hpp"
