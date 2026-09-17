// Copyright (c) 2026 aopenfx contributors.
//
// The SDK headers, compiled the way a plugin author compiles them.
//
// This translation unit links `aofx` and nothing else -- no host library, no
// image module, no logging. That is the test. The headers claim a plugin can be
// built against them without linking the application, and the only way to know
// is to build something that way and watch it link.
//
// It also exercises the entry macro, because the four exported symbols are the
// one part of the interface with no second chance: get them wrong and a bundle
// loads, resolves to nothing, and is absent from the menu with nothing said.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "aofx/Entry.h"
#include "aofx/Shape.h"

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    }
}

/// Two output planes, because that is the case a single-plane design would
/// quietly not support: a picture and a matte produced together, which is what
/// a renderer calls an arbitrary output variable.
class Pretend final : public aofx::Effect {
public:
    void describe(aofx::EffectDesc& into) override {
        into.identifier = "org.aopenfx.pretend";
        into.label = "Pretend";
        into.grouping = "Test";
        into.description = "Does nothing, in order to be described.";
        into.inputs.push_back(aofx::ClipDesc{"Source", "Source", false, false, false});

        aofx::ParamDesc size;
        size.name = "size";
        size.label = "Size";
        size.hint = "How much of nothing to do.";
        size.type = aofx::ParamType::Double;
        size.dimension = 2;
        size.defaults = {0.0, 0.0};
        size.displayMax = {100.0, 100.0};
        into.params.push_back(size);

        aofx::ParamDesc depth;
        depth.name = "depth";
        depth.label = "Depth";
        depth.hint = "Which precision to pretend at.";
        depth.type = aofx::ParamType::Choice;
        depth.choices = {{"half", "Half (16-bit float)"},
                         {"float", "Full float (32-bit)"}};
        into.params.push_back(depth);

        into.outputs.push_back(aofx::PlaneDesc{"Color", "Colour", {"R", "G", "B", "A"}});
        into.outputs.push_back(aofx::PlaneDesc{"Matte", "Matte", {"A"}});
    }

    [[nodiscard]] std::vector<aofx::KernelDesc> kernels() const override {
        return {};
    }

    [[nodiscard]] bool process(const aofx::RenderRequest&) override { return true; }
};

}   // namespace

AOFX_EXPORT_EFFECTS(Pretend)

int main() {
    check(AofxGetAbiVersion() == aofx::kAbiVersion,
          "the bundle reports the ABI it was built against");
    check(std::strcmp(AofxGetBuildTag(), aofx::buildTag()) == 0,
          "and the toolchain it was built with");
    check(AofxGetEffectCount() == 1, "one effect is exported");

    aofx::Effect* effect = AofxGetEffect(0);
    check(effect != nullptr, "and can be fetched");
    check(AofxGetEffect(1) == nullptr, "asking past the end gives null");
    check(AofxGetEffect(-1) == nullptr, "and so does asking before the start");
    if (effect == nullptr) {
        return 1;
    }

    aofx::EffectDesc desc;
    effect->describe(desc);
    check(desc.identifier == "org.aopenfx.pretend", "it describes itself");
    check(desc.inputs.size() == 1, "with its input");
    check(desc.params.size() == 2, "and its parameters");

    // The defect this exists to prevent: a choice stored as an index at one end
    // and read as a string at the other. In this host that silently wrote every
    // EXR at half precision, because the lookup returned nothing and nobody
    // checked. The SDK owns the encoding, so there is one answer to ask.
    check(desc.params[1].indexOf("float") == 1, "a choice maps to its index");
    check(desc.params[1].indexOf("half") == 0, "and so does the first one");
    check(desc.params[1].indexOf("nonsense") == 0,
          "and an unknown value falls back to the first rather than to -1");

    // Several outputs, which is the point of declaring them at all.
    check(desc.outputs.size() == 2, "two planes are declared");
    check(desc.outputs[0].id == "Color", "the picture first");
    check(desc.outputs[1].channels.size() == 1, "and a one-channel matte after it");

    // The default region of definition: the union of the inputs, which is what
    // an effect that does not spread the picture wants and what a blur must
    // override.
    const aofx::Rect united = effect->regionOfDefinition(
        1.0, {aofx::Rect{0, 0, 100, 50}, aofx::Rect{-20, 10, 60, 90}}, {});
    check(united.x1 == -20 && united.y1 == 0 && united.x2 == 100 && united.y2 == 90,
          "the default region of definition is the union of the inputs");

    const aofx::Rect fromNothing = effect->regionOfDefinition(1.0, {}, {});
    check(fromNothing.isEmpty(), "and a generator with no inputs starts empty");

    // A missing parameter reads as its fallback rather than as zero: a script
    // written before a control existed simply does not mention it.
    aofx::RenderRequest request;
    request.params.push_back(aofx::ParamValue{"size", {8.0, 4.0}, {}});
    check(request.number("size", 1.0) == 8.0, "a parameter reads back");
    check(request.number("size", 1.0, 1) == 4.0, "including its second component");
    check(request.number("size", 1.0, 7) == 1.0,
          "a component past the end falls back");
    check(request.number("added-later", 3.0) == 3.0,
          "and so does a parameter the script never heard of");

    // --- shapes -------------------------------------------------------------
    //
    // The encoding is an agreement between a host that writes it and an effect
    // that reads it, and neither is here: this checks the agreement against
    // itself, which is the part that can be checked without either.
    {
        namespace shp = aofx::shape;

        shp::Shape square;
        square.id = 7;
        square.op = shp::Op::Subtract;
        square.opacity = 0.5;
        square.featherFalloff = 2.0;
        square.enabled = false;
        square.points = {
            shp::Point{10.0, 20.0, -1.0, -2.0, 1.0, 2.0, 3.0},
            shp::Point{110.0, 20.0, -3.0, -4.0, 3.0, 4.0, 0.0},
            shp::Point{110.0, 120.0, 0.0, 0.0, 0.0, 0.0, 1.5},
        };
        shp::Shape empty;
        empty.id = 8;

        const std::vector<double> packed = shp::encode({square, empty});
        const std::vector<shp::Shape> back = shp::decode(packed);
        check(back.size() == 2, "two shapes go in and two come out");
        check(back[0].id == 7 && back[0].op == shp::Op::Subtract,
              "a shape keeps its identity and its operation");
        check(back[0].opacity == 0.5 && back[0].featherFalloff == 2.0,
              "and its numbers");
        check(!back[0].enabled && back[0].closed,
              "and its flags, which are two different booleans");
        check(back[0].points.size() == 3, "with every point");
        check(back[0].points[0].feather == 3.0 &&
                  back[0].points[2].feather == 1.5,
              "including the feather each point carries of its own");
        check(back[0].points[1].inX == -3.0 && back[0].points[1].outY == 4.0,
              "and both tangents, which are offsets and not positions");
        check(back[1].points.empty(), "a shape with no points is still a shape");

        // Refusing is the feature. A reader that decoded half of a truncated
        // array would hand back a matte with a hole in it and no error
        // anywhere -- which is exactly the failure nobody traces back to here.
        std::vector<double> cut = packed;
        cut.resize(cut.size() - 1);
        check(shp::decode(cut).empty(), "a truncated array decodes to nothing");

        std::vector<double> lying = packed;
        lying[1] = 99.0;   // says ninety-nine shapes, carries two
        check(shp::decode(lying).empty(),
              "and so does one whose header promises more than it holds");

        std::vector<double> future = packed;
        future[0] = 99.0;
        check(shp::decode(future).empty(),
              "an encoding from the future is refused, not guessed at");

        check(shp::decode({}).empty(), "and an empty value is not a shape");

        // The curve the host draws has to be the curve the effect renders, so
        // there is one evaluator and this is it. At the ends it is the points
        // themselves, whatever the tangents say.
        double x = 0.0;
        double y = 0.0;
        shp::evaluate(square.points[0], square.points[1], 0.0, x, y);
        check(x == 10.0 && y == 20.0, "a segment starts at its first point");
        shp::evaluate(square.points[0], square.points[1], 1.0, x, y);
        check(x == 110.0 && y == 20.0, "and ends at its second");

        check(shp::stepsFor(square.points[0], square.points[1]) > 1,
              "a long segment is flattened into several pieces");
        const shp::Point tiny{0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
        check(shp::stepsFor(tiny, tiny) == 1,
              "and a segment of no length into exactly one");
    }

    if (failures == 0) {
        std::puts("test_sdk_headers: ok");
    }
    return failures == 0 ? 0 : 1;
}
