// Copyright (c) 2026 aopenfx contributors.
//
// The gizmo standard, checked on a CPU: what `checkGizmos` accepts, what it
// names, and that a drawing survives the trip through an attachment.
//
// The declarations here are the ones the documentation shows -- a crop's box,
// a corner pin's quad, a transform whose handles turn with it, a pool of
// points, a tracker's path -- so a change to the standard that breaks an
// example in `docs/gizmos.md` breaks this first.
#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "aofx/Descriptor.h"
#include "aofx/Gizmo.h"

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    }
}

aofx::ParamDesc number(const std::string& name, int dimension,
                       aofx::ParamRole role = aofx::ParamRole::None) {
    aofx::ParamDesc param;
    param.name = name;
    param.label = name;
    param.hint = name;
    param.type = aofx::ParamType::Double;
    param.dimension = dimension;
    param.defaults.assign(static_cast<size_t>(dimension), 0.0);
    param.role = role;
    return param;
}

aofx::GizmoBinding bind(const std::string& slot, const std::string& param,
                        int component = 0) {
    return aofx::bindParam(slot, param, component);
}

aofx::GizmoDesc gizmo(const std::string& id, aofx::GizmoKind kind,
                      std::vector<aofx::GizmoBinding> bindings) {
    aofx::GizmoDesc desc;
    desc.id = id;
    desc.kind = kind;
    desc.bindings = std::move(bindings);
    return desc;
}

bool mentions(const std::vector<std::string>& problems, const std::string& text) {
    for (const std::string& problem : problems) {
        if (problem.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

/// Everything the documentation shows, in one effect, which must be clean.
aofx::EffectDesc everything() {
    aofx::EffectDesc effect;
    effect.identifier = "org.aopenfx.gizmos";
    for (int corner = 1; corner <= 4; ++corner) {
        effect.params.push_back(number("corner" + std::to_string(corner), 2));
    }
    effect.params.push_back(number("translate", 2, aofx::ParamRole::Position));
    effect.params.push_back(number("rotate", 1, aofx::ParamRole::Angle));
    effect.params.push_back(number("scale", 2, aofx::ParamRole::Scale));
    effect.params.push_back(number("center", 2, aofx::ParamRole::Position));
    effect.params.push_back(number("size", 1));
    effect.params.push_back(number("box", 4));

    aofx::ParamDesc count = number("count", 1, aofx::ParamRole::ItemCount);
    count.type = aofx::ParamType::Integer;
    effect.params.push_back(count);
    for (int item = 1; item <= 3; ++item) {
        effect.params.push_back(number("point" + std::to_string(item), 2));
    }
    aofx::ParamDesc show;
    show.name = "show";
    show.type = aofx::ParamType::Boolean;
    effect.params.push_back(show);

    // A crop: a box of two corners.
    effect.gizmos.push_back(gizmo("crop", aofx::GizmoKind::Box,
                                  {bind("corner1", "corner1"), bind("corner2", "corner2")}));
    // The same box from one four-component parameter.
    effect.gizmos.push_back(gizmo("box", aofx::GizmoKind::Box,
                                  {bind("corner1", "box", 0), bind("corner2", "box", 2)}));
    // A corner pin.
    effect.gizmos.push_back(gizmo("pin", aofx::GizmoKind::Quad,
                                  {bind("corner1", "corner1"), bind("corner2", "corner2"),
                                   bind("corner3", "corner3"), bind("corner4", "corner4")}));
    // A transform, and a blur size that turns with it.
    effect.gizmos.push_back(gizmo("xf", aofx::GizmoKind::Frame,
                                  {bind("translate", "translate"), bind("angle", "rotate"),
                                   bind("scale", "scale"), bind("centre", "center")}));
    aofx::GizmoDesc radius = gizmo("radius", aofx::GizmoKind::Distance,
                                   {bind("origin", "center"), bind("length", "size")});
    radius.bindings[1].scale = 0.5;   // a diameter, drawn as a radius
    radius.space = aofx::GizmoSpace::Parent;
    radius.parent = "xf";
    radius.visibility = aofx::GizmoVisibility::WhileEditing;
    radius.shownWhen = {"show", "1"};
    effect.gizmos.push_back(radius);
    // A polygon through every corner, bound point by point.
    effect.gizmos.push_back(gizmo("outline", aofx::GizmoKind::Polygon,
                                  {bind("points", "corner1"), bind("points", "corner2"),
                                   bind("points", "corner3")}));
    // A pool: one handle per item in use.
    aofx::GizmoDesc points = gizmo("points", aofx::GizmoKind::Point,
                                   {bind("at", "point{i}")});
    points.repeat = "count";
    effect.gizmos.push_back(points);
    // A label that says what a handle is set to.
    aofx::GizmoDesc label = gizmo("label", aofx::GizmoKind::Label, {bind("at", "center")});
    label.text = "size {size}";
    effect.gizmos.push_back(label);
    // A constant guide: the middle of a UHD frame.
    effect.gizmos.push_back(gizmo("middle", aofx::GizmoKind::Crosshair,
                                  {aofx::bindConstant("at", {1920.0, 1080.0})}));
    // What a tracker found, and the path it followed.
    effect.gizmos.push_back(
        gizmo("found", aofx::GizmoKind::Crosshair,
              {aofx::bindAttachment("at", "track.position", {0.0, 0.0})}));
    aofx::GizmoDesc drawing = gizmo("path", aofx::GizmoKind::Drawing,
                                    {aofx::bindAttachment("strokes", "track.path")});
    drawing.visibility = aofx::GizmoVisibility::Always;
    effect.gizmos.push_back(drawing);
    return effect;
}

void testClean() {
    const std::vector<std::string> problems = aofx::checkGizmos(everything());
    for (const std::string& problem : problems) {
        std::fprintf(stderr, "  unexpected: %s\n", problem.c_str());
    }
    check(problems.empty(), "every gizmo the documentation shows is accepted");
    check(aofx::checkGizmos(aofx::EffectDesc{}).empty(), "no gizmos, no problems");
}

void testProblems() {
    const auto broken = [](auto change) {
        aofx::EffectDesc effect = everything();
        change(effect);
        return aofx::checkGizmos(effect);
    };

    check(mentions(broken([](aofx::EffectDesc& e) { e.gizmos[0].bindings[0].param = "gone"; }),
                   "does not have"),
          "a renamed parameter is named");
    check(mentions(broken([](aofx::EffectDesc& e) { e.gizmos[0].bindings.pop_back(); }),
                   "leaves slot 'corner2' unbound"),
          "a missing slot is named");
    check(mentions(broken([](aofx::EffectDesc& e) { e.gizmos[0].bindings[1].slot = "corner1"; }),
                   "more than once"),
          "a slot bound twice is named");
    check(mentions(broken([](aofx::EffectDesc& e) { e.gizmos[0].bindings[0].slot = "centre"; }),
                   "not a slot of its kind"),
          "a slot the kind does not have is named");
    check(mentions(broken([](aofx::EffectDesc& e) { e.gizmos[1].bindings[1].component = 3; }),
                   "more components"),
          "a binding past the end of a parameter is named");
    check(mentions(broken([](aofx::EffectDesc& e) { e.gizmos[1].id = "crop"; }), "same id"),
          "a duplicate id is named");
    check(mentions(broken([](aofx::EffectDesc& e) { e.gizmos[4].parent = "pin"; }),
                   "not a Frame"),
          "a parent that is not a Frame is named");
    check(mentions(broken([](aofx::EffectDesc& e) {
                       e.gizmos[3].space = aofx::GizmoSpace::Parent;
                       e.gizmos[3].parent = "xf";
                   }),
                   "loops"),
          "a frame placed in itself is named");
    check(mentions(broken([](aofx::EffectDesc& e) { e.gizmos[4].bindings[1].scale = 0.0; }),
                   "cannot be undone"),
          "a scale of zero is named");
    check(mentions(broken([](aofx::EffectDesc& e) { e.gizmos[6].repeat = "size"; }),
                   "not an ItemCount"),
          "a repeat over a parameter that is not a count is named");
    check(mentions(broken([](aofx::EffectDesc& e) { e.gizmos[6].repeat.clear(); }),
                   "does not repeat"),
          "{i} without a repeat is named");
    check(mentions(broken([](aofx::EffectDesc& e) { e.gizmos[7].text.clear(); }), "no text"),
          "a Label with no text is named");
    check(mentions(broken([](aofx::EffectDesc& e) { e.gizmos[8].bindings[0].constant = {1.0}; }),
                   "wrong size"),
          "a constant of the wrong size is named");
    check(mentions(broken([](aofx::EffectDesc& e) { e.gizmos[9].bindings[0].param = "center"; }),
                   "both"),
          "two sources on one binding are named");
    check(mentions(broken([](aofx::EffectDesc& e) {
                       e.gizmos[10].bindings[0].attachment.clear();
                       e.gizmos[10].bindings[0].constant = {0.0, 0.0};
                   }),
                   "must be an attachment"),
          "a drawing that is not computed is named");
    check(mentions(broken([](aofx::EffectDesc& e) { e.gizmos[4].shownWhen.param = "gone"; }),
                   "is shown by 'gone'"),
          "a visibility rule on a missing parameter is named");
    check(mentions(broken([](aofx::EffectDesc& e) {
                       e.gizmos[0].kind = static_cast<aofx::GizmoKind>(aofx::kGizmoKindCount);
                   }),
                   "does not know"),
          "a kind from a newer SDK is named");
    check(mentions(broken([](aofx::EffectDesc& e) {
                       e.gizmos[0].bindings[0].param = "show";
                   }),
                   "not a number"),
          "a binding to a Boolean is named");
}

void testSlots() {
    for (int kind = 0; kind < aofx::kGizmoKindCount; ++kind) {
        check(!aofx::gizmoSlots(static_cast<aofx::GizmoKind>(kind)).empty(),
              "kind " + std::to_string(kind) + " has slots");
    }
    check(aofx::gizmoSlots(static_cast<aofx::GizmoKind>(aofx::kGizmoKindCount)).empty(),
          "a kind past the end has none");
    check(aofx::gizmoIsEditable(aofx::GizmoKind::Quad), "a quad drags");
    check(!aofx::gizmoIsEditable(aofx::GizmoKind::Drawing), "a drawing does not");
}

void testDrawing() {
    using aofx::gizmo::Stroke;
    std::vector<Stroke> strokes(2);
    strokes[0].flags = aofx::gizmo::kStrokeClosed | aofx::gizmo::kStrokeColoured;
    strokes[0].red = 0.25F;
    strokes[0].points = {0.0F, 0.0F, 10.0F, 0.0F, 10.0F, 10.0F};
    strokes[1].flags = aofx::gizmo::kStrokePoints;
    strokes[1].points = {5.0F, 5.0F, 7.0F};   // the odd one out is dropped

    const std::vector<float> data = aofx::gizmo::encodeDrawing(strokes);
    std::vector<Stroke> back;
    check(aofx::gizmo::decodeDrawing(data, back), "a drawing decodes");
    check(back.size() == 2, "two strokes back");
    if (back.size() == 2) {
        check(back[0].flags == strokes[0].flags && back[0].red == 0.25F &&
                  back[0].points == strokes[0].points,
              "the first stroke is what went in");
        check(back[1].points.size() == 2, "a half point is not a point");
    }

    std::vector<float> truncated = data;
    truncated.pop_back();
    check(!aofx::gizmo::decodeDrawing(truncated, back) && back.empty(),
          "a truncated drawing is refused, and leaves nothing");
    std::vector<float> longer = data;
    longer.push_back(0.0F);
    check(!aofx::gizmo::decodeDrawing(longer, back), "trailing numbers are refused");
    std::vector<float> newer = data;
    newer[0] = aofx::gizmo::kDrawingVersion + 1.0F;
    check(!aofx::gizmo::decodeDrawing(newer, back), "a newer layout is refused");
    std::vector<float> huge = {aofx::gizmo::kDrawingVersion, 1e9F};
    check(!aofx::gizmo::decodeDrawing(huge, back), "an absurd count is refused");
    std::vector<float> bad = data;
    bad.back() = std::numeric_limits<float>::infinity();
    check(!aofx::gizmo::decodeDrawing(bad, back), "an infinite coordinate is refused");
    check(aofx::gizmo::decodeDrawing(aofx::gizmo::encodeDrawing({}), back) && back.empty(),
          "no strokes is a drawing");
}

}   // namespace

int main() {
    testClean();
    testProblems();
    testSlots();
    testDrawing();
    if (failures == 0) {
        std::printf("gizmos: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
