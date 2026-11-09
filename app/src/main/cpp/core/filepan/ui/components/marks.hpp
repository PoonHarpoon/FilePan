// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <functional>
#include <memory>
#include <string>

#include "filepan/ui/components/common.hpp"
#include "filepan/ui/node.hpp"

// Tags (§7.7) and the graphic marks (§6): the hard, data-carrying layer. Marks are seasoning:
// a list screen uses quiet section labels, stamps and at most one data band; corner ticks,
// barcodes and hazard stripes are for sheets, the about screen and identity tags.
namespace filepan::ui::components {

enum class StampTone : std::uint8_t { accent, secondary, ink, destructive };

// A solid, sharp-cornered (4 dp) mono caps tag: `ONLINE`, `12 SELECTED`, `SIG-0x7F3A`.
NodePtr Stamp(Prop<std::string> text, StampTone tone = StampTone::accent, std::string icon = {});
// A hairline-bordered mono tag.
NodePtr OutlineTag(Prop<std::string> text);

struct GlassChipProps {
    Prop<std::string> text;
    std::string icon;
    // An accent dot before the text: "live".
    bool dot = false;
    Prop<bool> selected = false;
    std::function<void()> onTap;  // Makes it a button with a 48 dp target.
};
// A glass pill with a mono label.
NodePtr GlassChip(GlassChipProps props);

// A barcode and a mono ID in an outline tag: archive and SFTP identity.
NodePtr BarcodeTag(std::string id);

// Corner ticks over the parent: an absolutely positioned, hit-transparent overlay.
NodePtr CornerTicks(float inset = 12, float length = 12);
// A block of diagonal hazard stripes.
NodePtr HazardStripes(float width = 40, float height = 20);
// A barcode drawn from seed.
NodePtr Barcode(std::string seed, float width = 64, float height = 20);

// `FOLDERS`: a quiet mono caps label (11 sp, +0.08em, 52% ink) sitting on the bottom of a 36 dp
// band. A heading in semantics.
NodePtr SectionLabel(std::string label);

struct DataBandProps {
    Prop<std::string> text;       // Body face, sentence case: `Copying 12 of 40`.
    Prop<std::string> rate = std::string();  // Mono, at the end: `38 MB/s`; empty for none.
    Prop<std::string> icon = std::string("sync");  // Material Symbols ligature, in the accent.
    Prop<float> progress = -1;    // 0-1; negative shows no fill.
    std::string label;            // Spoken, e.g. "File jobs".
    std::function<void()> onTap;
};

// The data band: a 40 dp glass pill (radius 20) with a small accent icon, the text, and a mono
// rate at the end. Progress fills it from the start with a 20% accent wash ending in a 2 dp
// accent edge. One per screen.
class DataBand final : public Node {
public:
    static std::shared_ptr<DataBand> create(DataBandProps props);

protected:
    void paint(PaintContext &context) override;
    void paintOverlay(PaintContext &context) override;

private:
    Prop<float> progress_;
    std::shared_ptr<Pressable> pressable_;
};

}  // namespace filepan::ui::components
