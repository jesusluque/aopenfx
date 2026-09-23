// Copyright (c) 2026 aopenfx contributors.
//
// What an effect says about itself, before anybody asks it to render.
//
// Plain data, filled in once. The host turns it into the same description it
// builds for an OpenFX plugin, so the properties panel, the node menu, the port
// labels, the MCP server and the command line all work on an AOFX effect
// without knowing there is a second kind of plugin. That translation is the
// only place the two vocabularies meet, and it is the host's job.
#pragma once

#include <cmath>
#include <string>
#include <vector>

#include "aofx/Gizmo.h"
#include "aofx/Types.h"

namespace aofx {

/// The parameter kinds. Fewer than OpenFX has, on purpose: these are the ones
/// an effect actually needs, and every one of them has a control that can edit
/// it and a well-defined thing to put in a script.
enum class ParamType {
    Double,    ///< one to four components; `dimension` says how many
    Integer,
    Boolean,
    Choice,    ///< `choices` holds the options, the value is the index
    String,
    /// N components that are the control points of a curve, evenly spaced
    /// along whatever the curve is indexed by.
    ///
    /// The values travel exactly as a Double of the same dimension does -- this
    /// is again a statement about meaning, not about transport. What it buys is
    /// an editor you drag instead of a row of spin boxes, which for a curve is
    /// the difference between a control and a table of numbers.
    ///
    /// Fixed positions, unlike Nuke's LookupCurves where points can be added
    /// anywhere. That covers what the effects here need -- an amount per hue, a
    /// lift per input level -- and it keeps the value a plain list of numbers,
    /// which is what makes it fit the SDK at all.
    Curve,
    /// A button. Not a value: pressing it is an event, and the host counts
    /// the presses per node. A request then carries a ParamValue of this name
    /// holding the count so far, and an effect that remembers the last count
    /// it saw knows a press happened -- which is what a live trigger is.
    /// Nothing is written to the document.
    Button,
    /// Three or four components that mean a colour rather than three or four
    /// numbers that happen to sit together.
    ///
    /// The values are identical -- this changes nothing about what reaches the
    /// kernel. What it changes is the panel: a colour gets a swatch, a picker
    /// and a wheel, and four numbers get four spin boxes. Nuke makes the same
    /// distinction and makes it everywhere it matters: every one of Grade's
    /// seven controls is an AColor_Knob, not a four-element array.
    ///
    /// Worth a type rather than a flag because it is a statement about meaning,
    /// and the day something else needs to know -- a colour-managed picker, a
    /// swatch in a node's tooltip -- it will ask the type.
    Colour,
    /// A list of closed shapes: control points, tangents, per-point feather,
    /// and how each shape combines with the ones under it.
    ///
    /// The value travels as numbers like every other, in the layout
    /// `aofx/Shape.h` defines -- so this adds a meaning and not a transport.
    /// What it buys is everything a list of numbers cannot have: the host
    /// draws the outline over the picture, the pen edits it, and the points
    /// animate.
    ///
    /// It is the same bargain `ParamRole::Position` makes and the reason that
    /// role exists: an effect cannot draw a handle and should not be able to,
    /// so it says what its numbers *are* and the host draws what it always
    /// knew how to draw. A roto is that sentence with a list of points instead
    /// of one.
    ///
    /// **Not animated in the value.** What reaches `render` is the shape at
    /// that time, evaluated and transformed. Where the keys live is the
    /// document's business; an effect that interpolated them would be guessing
    /// at what a keyframe means here.
    Shape,
    /// A heading with a collapsible section under it. Holds no value: every
    /// parameter naming it as `parent` is drawn inside.
    ///
    /// Thirteen flat rows is a list somebody reads top to bottom every time
    /// they open a node. Four headings is a place to look.
    Group,
};

/// What a number *means*, beyond being a number.
///
/// The host draws every gizmo -- a plugin here cannot, and should not be able
/// to: it would have to link the toolkit the application is written in, share
/// its event loop and its coordinate systems, and one crash in a handle would
/// take the window down with it. What a plugin can do is *say what its numbers
/// are*, and let the host draw the handle it always knew how to draw.
///
/// That is how the OpenFX path already works: a plugin marks a pair of numbers
/// as a position and gets a handle in the viewer without writing a line of
/// drawing code. AOFX effects had no way to say it, so they got no handles
/// except where the host recognised the parameter's *name* -- which covers
/// Transform and nothing anybody writes next.
enum class ParamRole {
    None,
    /// Two numbers that are a place in the picture, in pixels. Gets a handle.
    Position,
    /// Degrees. Gets a rotation ring, and a sensible slider range -- without
    /// this a rotation with no declared range lands on a 0..1 slider that
    /// cannot reach a single degree.
    Angle,
    /// A multiplier around one, rather than a value between zero and one.
    Scale,
    /// How many items of a pool are in use, and which one is being edited.
    ///
    /// A pool is how an effect offers a list when OFX declares its parameters
    /// once: N slots, a count, and an index that decides whose rows are shown.
    /// Declaring the pair is what lets the panel draw a *list* -- add, remove,
    /// reorder, pick -- instead of two spin boxes somebody has to drive by
    /// hand.
    ///
    /// A statement about meaning, like every other role. The alternative is a
    /// host that recognises a pool by the names its parameters happen to use,
    /// which is guessing, and which would work for the effects somebody
    /// thought of and no others.
    ItemCount,
    ItemIndex,
};

struct ChoiceOption {
    std::string value;   ///< what a script records
    std::string label;   ///< what a person reads
};

struct ParamDesc {
    std::string name;    ///< unique within the effect; what a script records
    std::string label;   ///< what the panel shows
    std::string hint;    ///< what the tooltip says. Not optional: an unexplained
                         ///< control is a control nobody touches.
    ParamType   type = ParamType::Double;
    int         dimension = 1;

    std::vector<double> defaults;       ///< numeric default, one per dimension
    std::string         textDefault;    ///< for a String parameter
    std::vector<ChoiceOption> choices;  ///< for a Choice

    /// What the numbers mean. See ParamRole: this is how an effect asks for a
    /// gizmo without drawing one.
    ParamRole role = ParamRole::None;

    /// Whether `defaults` is a fraction of the project rather than pixels.
    ///
    /// A spatial default very often wants to be "the middle of the frame" or
    /// "the corners of it", and a plugin has no way to know the project size
    /// when it describes itself. OpenFX solved this long ago with
    /// `kOfxParamPropDefaultCoordinateSystem`, and this host already expands
    /// those for the plugins that use it; AOFX had no equivalent, so an effect
    /// wanting a centred default had to write a number and hope.
    ///
    /// Our own Transform wrote `(0, 0)`, which is the bottom-left corner --
    /// and the gizmo, having nothing to read, drew its ring in the middle of
    /// the frame. So the ring said one thing, the render did another, and the
    /// picture turned about a point with no handle on it.
    ///
    /// `{0.5, 0.5}` with this set means the centre of the frame at any format.
    bool defaultsNormalised = false;
    /// When this parameter is worth showing. Empty `param` means always.
    ShownWhen shownWhen;
    /// A button that means "run this node now", not "set a flag".
    ///
    /// A delivery node does its work across a delivery: CameraTrack feeds the
    /// solver frame by frame over the range and launches it on the last one.
    /// Its "Solve again" button therefore cannot start anything by itself, and
    /// pressing it in the viewer did nothing an operator could see -- the hint
    /// said "on the next delivery" and the next delivery was something they
    /// had to know to go and run.
    ///
    /// A button with this set asks the host to deliver this node's range right
    /// after the press. The plugin still decides what the press means; this
    /// only says that a press is useless without a delivery behind it.
    bool startsDelivery = false;
    /// The Integer parameter a press writes the current frame into, or empty.
    ///
    /// A tracker's "Start again" forgets its reference; the operator pressing
    /// it on frame 20 means "start from here", and having to type 20 into the
    /// reference by hand is the kind of second step that gets forgotten. A
    /// button with this set has the host write the frame it was pressed on
    /// into the named parameter, as a document edit with an undo step, and
    /// then count the press as usual. Appended, so a plugin built without it
    /// reads as empty, which is what it was.
    std::string stampsFrameInto;
    /// A second condition, and both must hold.
    ///
    /// One was enough while a rule meant "this checkbox is off". It stops
    /// being enough the moment a control belongs to an *item*: a title card's
    /// components are a pool of slots, and a font size is worth a row when
    /// slot 3 is the selected one **and** slot 3 is a piece of text. With one
    /// condition the only way to say that is to show typography rows on a
    /// selected rectangle, which is exactly the clutter `shownWhen` exists to
    /// remove.
    ///
    /// Empty `param` means "no second condition", so nothing that already
    /// declared one condition changes.
    ShownWhen shownAlso;
    /// The `Group` parameter this one belongs under, by name. Empty is top
    /// level, which is where everything was before groups existed.
    std::string parent;

    /// For a Curve: what to write under each control point. Empty for none.
    std::vector<std::string> axisLabels;

    /// For a Curve: whether the last point joins the first. True for anything
    /// indexed by hue, false for anything indexed by level.
    ///
    /// It has to match what the kernel does. A widget that closes the loop over
    /// a curve the kernel clamps draws a shape the render will not produce, and
    /// the user believes the drawing.
    bool curveWraps = true;

    /// What a String parameter holds: free text, a file, or a folder.
    ///
    /// A path parameter gets a chooser button beside it -- which the panel has
    /// always drawn for the built-in Read and Write, because those declare
    /// themselves through OpenFX's own property and an effect had no way to
    /// say it. So the delivery node asked people to type a path from memory
    /// while the node it replaces had a button, and that is a regression
    /// dressed as a new feature.
    ///
    /// `Multiline` is for a value that is written rather than typed -- the text
    /// of a title card, a list of overrides, a caption with expressions in it.
    /// It gets a real editor rather than a one-line field, and that is not
    /// cosmetic: a caption is two lines and a `{hole}` is a thing somebody
    /// needs to see the whole of while they write it.
    /// `Components` is a `Multiline` that happens to be a *list* -- one item
    /// per line, drawn in the order written. It gets the editor plus a list
    /// beside it that can add, remove and reorder rows, because "put the third
    /// one behind the second" is a thing somebody does with two clicks and not
    /// by cutting a line out of a text box and pasting it one line up.
    ///
    /// The text stays the truth. The list reads it and writes it back, and
    /// holds nothing of its own -- the alternative is two copies of the card
    /// that disagree the first time somebody types into the box.
    /// `LocalDirectoryPath` is a folder on the machine running the *window*,
    /// not on the one running the render.
    ///
    /// Every other path in a document belongs to whoever will open it, so in a
    /// remote session the chooser browses the render host -- which is right for
    /// a Read's file and a Write's output and wrong for exactly one thing: the
    /// folder a finished render is copied *back* to. That one is this machine's
    /// by definition, and browsing the host for it offered a list of folders
    /// the copy could never land in.
    ///
    /// Appended, never reordered: the value travels as an int across the
    /// plugin boundary, so an older host reads an unknown one as Plain and
    /// shows a text field, which is what it did before this existed.
    enum class Text {
        Plain,
        FilePath,
        DirectoryPath,
        Multiline,
        Components,
        LocalDirectoryPath,
    };
    Text text = Text::Plain;

    /// Bounds. `display*` is where a slider ends; `hard*` is where the value
    /// stops. Leaving them empty means unbounded, which is right for a
    /// translation and wrong for a radius.
    std::vector<double> displayMin;
    std::vector<double> displayMax;
    std::vector<double> hardMin;
    std::vector<double> hardMax;

    /// The choice index a value maps to.
    ///
    /// Here rather than at each call site, because leaving that to callers is
    /// how this host ended up writing every EXR at half precision: the panel
    /// stored a choice as an index, the engine read it back as a string, the
    /// lookup silently returned nothing and picking "Full float" did nothing at
    /// all. The SDK owns the encoding so that cannot happen again.
    [[nodiscard]] int indexOf(const std::string& value) const noexcept {
        for (size_t i = 0; i < choices.size(); ++i) {
            if (choices[i].value == value) {
                return static_cast<int>(i);
            }
        }
        return 0;
    }
};

struct ClipDesc {
    std::string name;    ///< "Source", "Mask", ...
    std::string label;   ///< what the port is called in the graph
    bool        optional = false;
    bool        isMask = false;
    /// The host fills this one; it is not a port.
    ///
    /// A clip the effect reads by name and nobody wires: the 3D renderer's
    /// four texture registers, which the engine binds from its walk of the
    /// scene. Declared so the effect can find them by name and so the host
    /// knows how many there are -- and marked, so that the graph, the panel
    /// and the node reference all leave them out of the list of ports.
    ///
    /// The rule everywhere else: a port is something a person can connect
    /// something to. If nobody can, it does not belong on the node.
    bool        hostWired = false;
    /// Hand this effect every plane its input declares, not only the picture.
    ///
    /// A plane is allocated only when something wants it -- the viewer's
    /// Layer selector, for the whole session -- because one nobody reads costs
    /// a full frame on every render. This is the other way to want one: a node
    /// downstream that reads planes, which makes the node above it produce
    /// them. `RenderRequest::inputs` then carries one `InputPlane` per plane,
    /// found by clip name and plane id.
    bool        wantsPlanes = false;

    /// Frames other than the one being rendered. An effect that leaves this
    /// false is promised the current frame and nothing else, which is what
    /// almost every effect wants and what lets the host cache aggressively.
    bool temporal = false;

    /// *Which* other frames, as offsets from the one being rendered. `{-1}` is
    /// "and the frame before this one". Setting it sets `temporal`.
    ///
    /// WHY AN EFFECT SHOULD ASK FOR THE PREVIOUS FRAME RATHER THAN KEEP IT
    ///
    /// `keep` makes it tempting to hold last frame's work and call that the
    /// previous frame. It is the same temptation every optical flow and every
    /// tracker has, and it is wrong here for a reason that has nothing to do
    /// with the effect: **this host does not render frames in order.** It
    /// prefetches, so a second of playback arrives as 4, 1, 4, 2, 2, 2, 3; and
    /// it drops frames on purpose to hold the frame rate, so gaps are normal
    /// rather than exceptional. An effect whose answer depends on what it was
    /// asked last is an effect whose answer depends on the machine's load.
    ///
    /// Asking instead makes the answer a function of the frame number, which
    /// is what the cache already assumes about every node it stores -- and the
    /// previous frame is nearly always a cache hit, so asking is not a second
    /// decode.
    ///
    /// Keeping is still right as a **cache**: hold the pyramid you built, note
    /// which frame it was for, and use it when it happens to be the frame you
    /// were going to build anyway. That is fast in order and correct out of
    /// it. An optical-flow node is the usual case.
    ///
    /// Clamped at the ends of the clip: an offset that would run off the front
    /// gives the first frame, which is the same answer OFX gives and better
    /// than a hole.
    std::vector<int> alsoFrames;

    /// The same clip at the frame an Integer parameter of this effect names,
    /// whatever frame is being rendered.
    ///
    /// For a tracker with a reference frame: the picture it tracks against
    /// is the one at `reference`, and until this existed the only way to
    /// see it was for the playhead to pass through that frame -- so a
    /// tracker that lost its memory, on a worker that was restarted, on a
    /// document opened at frame nine hundred, waited for somebody to scrub
    /// back. Named here, the host renders that frame beside the current one
    /// (a cache hit after the first) and `RenderRequest::inputAt(clip,
    /// frame)` answers it on every render. Clamped to the project's range;
    /// empty, or a parameter that is not there, asks for nothing.
    std::string alsoAtParam;

    /// The input a picture comes *through* this effect from.
    ///
    /// With one input the question does not arise, and the host used "the first
    /// connected one" without saying so. With two it stops being obvious, and
    /// getting it wrong is silent: a merge whose channel switches restored from
    /// the foreground would put the wrong picture back into the channels
    /// nobody asked to change, and it would look like a compositing mistake
    /// rather than a host one.
    ///
    /// It decides three things, all of which have to agree: which input the
    /// output takes its pixel format from, which one an identity hands back,
    /// and which one the unselected channels are restored from. For a merge
    /// that is the background. Where no clip claims it, the first connected
    /// input is used, which is what every single-input effect wants and what
    /// the host did before this existed.
    bool passThrough = false;
};

/// A plane the effect writes in addition to the picture.
///
/// The idea a renderer calls an arbitrary output variable: a matte, a depth, a
/// motion vector field, an object id -- things that are nearly free to produce
/// while the kernel is already reading the pixels, and expensive to get by
/// running the effect again.
///
/// The host allocates one buffer per declared plane and hands them all to
/// `process`. Downstream, a plane is addressed by id, exactly as a multi-layer
/// EXR addresses one.
struct PlaneDesc {
    std::string id;      ///< "Color", "Depth", "Backward", "MyLayer"
    std::string label;   ///< what a person reads
    /// Channel names, in order. Four for RGBA, one for a matte, two for a
    /// motion vector. The buffer is float4 regardless -- this says how many of
    /// those four carry meaning.
    std::vector<std::string> channels{"R", "G", "B", "A"};
};

/// One kernel the effect brings with it, already compiled.
///
/// `name` is what `Gpu::load` takes. `entry` is the function inside the blob,
/// which is a different thing and the first plugin written against this SDK
/// found out the hard way: a kernel wants a globally unique name, and a Slang
/// entry point wants to be called `blurMain`. Conflating them means the backend
/// looks for a function called `org.aopenfx.blur.blurMain` and reports,
/// correctly and unhelpfully, that there is no such function in the library.
///
/// The blob is the compiled form for the backend this was built for -- PTX or a
/// metallib -- produced from a `.slang` source by the plugin's own build.
struct KernelDesc {
    std::string          name;
    std::string          entry;
    const unsigned char* blob = nullptr;
    size_t               blobBytes = 0;
};

struct EffectDesc {
    /// Reverse DNS, in the same space OpenFX plugins and this host's own
    /// built-in nodes use. Sharing it is deliberate: a script then needs no
    /// separate "is this one of ours?" flag, and a node keeps working if an
    /// effect is ever reimplemented as some other kind of plugin.
    std::string identifier;
    std::string label;
    std::string grouping;      ///< "Filter", "Colour", "Image/Readers"
    std::string description;
    int         versionMajor = 1;
    int         versionMinor = 0;

    std::vector<ClipDesc>  inputs;
    std::vector<ParamDesc> params;

    /// The picture first, then any extra planes. Left empty, the host supplies
    /// a single RGBA colour plane, which is what a normal effect wants.
    std::vector<PlaneDesc> outputs;

    /// True where this effect runs a compiled network rather than only
    /// kernels.
    ///
    /// Not a technicality and not decoration. A kernel node costs a
    /// millisecond or two and a model node costs tens; one is a colour
    /// correction and the other is a decision about whether the graph plays.
    /// Somebody reading a graph should be able to see which is which without
    /// opening every node, so the host draws these differently -- and the flag
    /// has to come from the effect, because nothing else knows.
    bool usesModel = false;

    /// True where this effect's real work happens only in a delivery: a
    /// range walked in order, once per frame, however long each one takes.
    ///
    /// The other side of the same coin as `usesModel`. A model node is slow
    /// but plays; an offline node never plays -- it writes a set of pictures,
    /// rectifies a dataset, starts a training run -- and in the viewer it
    /// shows a preview of what it would do and nothing more. The host draws
    /// these darker still, treats them as delivery targets like a Write
    /// (never served from cache, walked by a batch, handed a resolved
    /// `first`/`last`), and waits on the job they report through the
    /// `job.<instance>` attachment. See `aofx/Delivery.h`.
    bool offline = false;

    /// What the host does about sound when `process` leaves
    /// `RenderRequest::audio` unset.
    enum class AudioRule : uint8_t {
        /// The sum of every non-mask input's sound at this frame. The default,
        /// and the rule that never loses a sound quietly: a Merge whose
        /// foreground has music is a Merge somebody wants to hear.
        Mix,
        /// The sound of the input the effect passes through (the first, or
        /// the one whose `ClipDesc::passThrough` is set).
        PassThrough,
        /// No sound.
        Silent,
    };
    AudioRule audio = AudioRule::Mix;

    /// True where this effect writes no pixels at all -- a gain, a file of
    /// sound laid under a picture. The host runs `process` with no output
    /// buffers and hands on the input's picture, unchanged and uncopied, with
    /// whatever sound the effect set. An effect that sets this and then
    /// writes to an output has nothing to write to.
    bool audioOnly = false;

    /// While this holds, the node is a live source: its picture arrives on
    /// its own rather than with the timeline.
    ///
    /// A graphic a browser is animating, a feed, a clock -- anything whose
    /// frame is not a function of the frame number. The host treats such a
    /// node the way it treats a Movie set to Live: never served from the
    /// cache for a time it rendered before, never prefetched ahead of the
    /// playhead, and rendered on the live clock while the transport is
    /// parked. Without it an effect like that is rendered once per frame
    /// number and frozen there.
    ///
    /// The same form as `ParamDesc::shownWhen`: a parameter and the value it
    /// must have (a Choice's option `value`, anything else written out).
    /// Empty `param` means never. A parameter the node does not set counts as
    /// its declared default.
    ShownWhen flowsWhen;

    /// The handles this effect wants in the viewer, beyond what `ParamRole`
    /// gives. See `aofx/Gizmo.h` and `docs/gizmos.md`.
    ///
    /// Any gizmo is a composition of the primitives there, bound to this
    /// effect's parameters; the host draws them and writes a drag back as a
    /// parameter edit. A parameter bound to a gizmo the host draws gets no
    /// handle from its role, so declaring one never draws a handle twice; a
    /// gizmo the host skips suppresses nothing. Left empty, an effect gets
    /// exactly what its roles gave it before this existed.
    ///
    /// `checkGizmos` says what is wrong with a list before a host has to.
    std::vector<GizmoDesc> gizmos;
};

/// What is wrong with an effect's gizmos, one sentence each. Empty means a
/// host can draw every one of them.
///
/// Worth calling from a test: a gizmo bound to a parameter that was renamed
/// is not an error anywhere else -- it is a handle that silently stops
/// appearing, and nobody notices a handle that is not there. A host calls it
/// too, and skips the gizmos it names rather than refusing the bundle.
[[nodiscard]] inline std::vector<std::string> checkGizmos(const EffectDesc& effect) {
    std::vector<std::string> problems;
    const auto paramNamed = [&effect](const std::string& name) -> const ParamDesc* {
        for (const ParamDesc& param : effect.params) {
            if (param.name == name) {
                return &param;
            }
        }
        return nullptr;
    };
    const auto gizmoNamed = [&effect](const std::string& id) -> const GizmoDesc* {
        for (const GizmoDesc& gizmo : effect.gizmos) {
            if (gizmo.id == id) {
                return &gizmo;
            }
        }
        return nullptr;
    };
    const auto clipNamed = [&effect](const std::string& name) {
        for (const ClipDesc& clip : effect.inputs) {
            if (clip.name == name) {
                return true;
            }
        }
        return false;
    };
    // What `{i}` becomes for the purpose of checking: the first item, which
    // every pool has.
    const auto firstItem = [](std::string name) {
        for (size_t at = name.find("{i}"); at != std::string::npos;
             at = name.find("{i}", at)) {
            name.replace(at, 3, "1");
        }
        return name;
    };

    for (size_t index = 0; index < effect.gizmos.size(); ++index) {
        const GizmoDesc& gizmo = effect.gizmos[index];
        const std::string who =
            "gizmo '" + (gizmo.id.empty() ? "#" + std::to_string(index) : gizmo.id) + "'";
        if (gizmo.id.empty()) {
            problems.push_back(who + " has no id");
        }
        for (size_t other = 0; other < index; ++other) {
            if (!gizmo.id.empty() && effect.gizmos[other].id == gizmo.id) {
                problems.push_back(who + " has the same id as an earlier one");
                break;
            }
        }
        const int kind = static_cast<int>(gizmo.kind);
        if (kind < 0 || kind >= kGizmoKindCount) {
            problems.push_back(who + " is of a kind this SDK does not know");
            continue;
        }

        // The space, and so how many numbers a place has.
        if (gizmo.space == GizmoSpace::Parent) {
            const GizmoDesc* up = gizmoNamed(gizmo.parent);
            if (up == nullptr ||
                (up->kind != GizmoKind::Frame && up->kind != GizmoKind::Frame3D)) {
                problems.push_back(who + " is placed in '" + gizmo.parent +
                                   "', which is not a Frame or Frame3D of this effect");
                continue;
            }
        } else if (!gizmo.parent.empty()) {
            problems.push_back(who + " names a parent but is not in GizmoSpace::Parent");
        }
        const int dimensions = gizmoDimensions(effect.gizmos, gizmo);
        if (dimensions == 0) {
            problems.push_back(who + " is in a chain of frames that loops or breaks");
            continue;
        }
        if (!gizmoWorksIn(gizmo.kind, dimensions)) {
            problems.push_back(who + (dimensions == 3 ? " is a 2D kind in a 3D space"
                                                      : " is a 3D kind in a 2D space"));
            continue;
        }

        // Not `slots`: a Qt host defines that word as a macro, and this
        // header is included by hosts as well as plugins.
        const std::vector<GizmoSlot> kindSlots = gizmoSlots(gizmo.kind);
        const auto boundCount = [&gizmo](const char* name) {
            int bound = 0;
            for (const GizmoBinding& binding : gizmo.bindings) {
                bound += binding.slot == name ? 1 : 0;
            }
            return bound;
        };
        for (const GizmoSlot& slot : kindSlots) {
            const int bound = boundCount(slot.name);
            if (bound == 0 && !slot.optional) {
                problems.push_back(who + " leaves slot '" + slot.name + "' unbound");
            }
            if (bound > 1 && !slot.list) {
                problems.push_back(who + " binds slot '" + slot.name + "' more than once");
            }
        }
        if (gizmo.kind == GizmoKind::Frame3D || gizmo.kind == GizmoKind::Camera) {
            const bool matrix = boundCount("matrix") > 0;
            const bool parts = boundCount("translate") + boundCount("rotate") +
                                   boundCount("scale") + boundCount("pivot") > 0;
            if (matrix && parts) {
                problems.push_back(who + " binds 'matrix' and the slots it replaces");
            }
            if (gizmo.kind == GizmoKind::Camera && !matrix &&
                boundCount("translate") == 0) {
                problems.push_back(who + " is a Camera with neither 'translate' nor 'matrix'");
            }
            const int order = static_cast<int>(gizmo.rotationOrder);
            if (order < 0 || order > static_cast<int>(xform::RotationOrder::ZYX)) {
                problems.push_back(who + " has a rotation order this SDK does not know");
            }
        }

        const bool repeated = !gizmo.repeat.empty();
        for (const GizmoBinding& binding : gizmo.bindings) {
            const GizmoSlot* slot = nullptr;
            for (const GizmoSlot& candidate : kindSlots) {
                if (binding.slot == candidate.name) {
                    slot = &candidate;
                }
            }
            const std::string where = who + " slot '" + binding.slot + "'";
            if (slot == nullptr) {
                problems.push_back(where + " is not a slot of its kind");
                continue;
            }
            const int sources = (binding.param.empty() ? 0 : 1) +
                                (binding.attachment.empty() ? 0 : 1);
            if (sources > 1) {
                problems.push_back(where + " names both a parameter and an attachment");
                continue;
            }
            if (sources == 0 && binding.constant.empty()) {
                problems.push_back(where + " has no source: no parameter, attachment or constant");
                continue;
            }
            if (gizmo.kind == GizmoKind::Drawing && binding.attachment.empty()) {
                problems.push_back(where + " must be an attachment: a drawing is computed");
                continue;
            }
            if (!binding.clip.empty()) {
                if (binding.attachment.empty()) {
                    problems.push_back(where + " names a clip but no attachment to read on it");
                } else if (!clipNamed(binding.clip)) {
                    problems.push_back(where + " reads clip '" + binding.clip +
                                       "', which is not an input of this effect");
                }
            }
            if (!std::isfinite(binding.scale) || binding.scale == 0.0 ||
                !std::isfinite(binding.offset)) {
                problems.push_back(where + " has a scale that cannot be undone");
            }
            if (!repeated && (binding.param.find("{i}") != std::string::npos ||
                              binding.attachment.find("{i}") != std::string::npos)) {
                problems.push_back(where + " uses {i} on a gizmo that does not repeat");
            }
            const int wanted = gizmoSlotWidth(*slot, dimensions);
            const bool whole = !slot->list && wanted == 0;   // a drawing's strokes
            if (binding.param.empty()) {
                const size_t size = binding.constant.size();
                if (binding.attachment.empty() && !whole &&
                    (slot->list ? size % static_cast<size_t>(dimensions) != 0
                                : size != static_cast<size_t>(wanted))) {
                    problems.push_back(where + " has a constant of the wrong size");
                }
                continue;
            }
            const ParamDesc* param = paramNamed(firstItem(binding.param));
            if (param == nullptr) {
                problems.push_back(where + " names parameter '" + binding.param +
                                   "', which this effect does not have");
                continue;
            }
            if (param->type != ParamType::Double && param->type != ParamType::Integer) {
                problems.push_back(where + " names parameter '" + binding.param +
                                   "', which is not a number");
                continue;
            }
            const int left = param->dimension - binding.component;
            if (binding.component < 0 || left <= 0 || left < wanted ||
                (slot->list && left % dimensions != 0)) {
                problems.push_back(where + " wants more components of '" +
                                   binding.param + "' than it has");
            }
        }

        const int constraint = static_cast<int>(gizmo.constraint);
        if (dimensions == 2 && constraint > static_cast<int>(GizmoConstraint::Locked)) {
            problems.push_back(who + " has a 3D constraint in a 2D space");
        }
        if (!gizmo.camera.empty()) {
            const GizmoDesc* camera = gizmoNamed(gizmo.camera);
            if (dimensions != 3) {
                problems.push_back(who + " names a camera but is not 3D");
            } else if (camera == nullptr || camera->kind != GizmoKind::Camera) {
                problems.push_back(who + " is projected through '" + gizmo.camera +
                                   "', which is not a Camera of this effect");
            } else if (camera == &gizmo) {
                problems.push_back(who + " is projected through itself");
            }
        }
        if (gizmo.kind == GizmoKind::Label && gizmo.text.empty()) {
            problems.push_back(who + " is a Label with no text");
        }
        if (repeated) {
            const ParamDesc* count = paramNamed(gizmo.repeat);
            if (count == nullptr || count->role != ParamRole::ItemCount) {
                problems.push_back(who + " repeats over '" + gizmo.repeat +
                                   "', which is not an ItemCount parameter");
            }
        }
        for (const ShownWhen* rule : {&gizmo.shownWhen, &gizmo.shownAlso}) {
            if (!rule->param.empty() && paramNamed(rule->param) == nullptr) {
                problems.push_back(who + " is shown by '" + rule->param +
                                   "', which this effect does not have");
            }
        }
    }
    return problems;
}

}   // namespace aofx
