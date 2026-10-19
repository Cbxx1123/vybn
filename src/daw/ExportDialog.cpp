#include "ExportDialog.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <ctime>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>

#include "ExportModel.h"
#include "Format.h"
#include "Project.h"
#include "Widgets.h"

namespace daw {

using namespace ui;

namespace {

// The presets and targets columns, and the form's label and field columns
constexpr float kColumnWidth = 210.0f;
constexpr float kLabelWidth = 92.0f;
constexpr float kFieldWidth = 190.0f;

// The choices of each setting, in the order their controls show them
const std::vector<ExportSource> kSources = {ExportSource::Master, ExportSource::MasterDry, ExportSource::EachTrack,
                                            ExportSource::Tracks, ExportSource::Midi};
const std::vector<std::string> kSourceNames = {"Master", "Master without effects", "Each track", "Chosen tracks", "MIDI"};
const std::vector<std::string> kSourceShort = {"Master", "Dry master", "Each track", "Tracks", "MIDI"};
const std::vector<ExportTap> kTaps = {ExportTap::PostFader, ExportTap::PreFader};
const std::vector<ExportRangeKind> kRanges = {ExportRangeKind::Song, ExportRangeKind::Timeline, ExportRangeKind::Loop,
                                              ExportRangeKind::Bars};
const std::vector<ExportFormat> kFormats = {ExportFormat::Wav, ExportFormat::Aiff, ExportFormat::Flac};
const std::vector<int> kBits = {16, 24, 32};
const std::vector<std::string> kRangeNames = {"Song", "Timeline", "Loop", "Bars"};
const std::vector<NormalizeMode> kNormalize = {NormalizeMode::Off, NormalizeMode::Peak, NormalizeMode::Loudness};
const std::vector<std::string> kNormalizeNames = {"Off", "Peak", "Loudness"};
const std::vector<DitherMode> kDithers = {DitherMode::None, DitherMode::Tpdf, DitherMode::NoiseShaped};
const std::vector<std::string> kDitherNames = {"None", "TPDF", "Noise-shaped"};
const std::vector<uint32_t> kRates = {22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000};
const std::vector<std::string> kRateNames = {"22.05 kHz", "32 kHz", "44.1 kHz", "48 kHz", "88.2 kHz", "96 kHz", "176.4 kHz", "192 kHz"};
const std::vector<double> kLoudnessValues = {-14.0, -16.0, -23.0, -9.0};
// Short, so the choice doesn't widen the target's form (its figure is in the Loudness field beside it)
const std::vector<std::string> kLoudnessNames = {"Custom", "Streaming (-14)", "Apple (-16)", "Broadcast (-23)", "Club (-9)"};

// The index of `value` in `values` (0 when it isn't there)
template <typename T>
int indexOf(const std::vector<T> &values, const T &value)
{
    for (size_t i = 0; i < values.size(); ++i) {
        if (values[i] == value)
            return static_cast<int>(i);
    }
    return 0;
}

// The choice at the preset for a loudness target (index into kLoudnessNames, 0 for a custom one)
int loudnessChoice(double lufs)
{
    for (size_t i = 0; i < kLoudnessValues.size(); ++i) {
        if (std::fabs(kLoudnessValues[i] - lufs) < 0.05)
            return static_cast<int>(i) + 1;
    }
    return 0;
}

// A plain number as it is typed: 1, 2.5, -14
std::string plain(double value)
{
    std::ostringstream out;
    out << value;
    return out.str();
}

// What a target is, in a few words for its row: "Master  WAV 24-bit  -14 LUFS" (the rate is in the form)
std::string summaryOf(const ExportTarget &t)
{
    std::string text = kSourceShort[static_cast<size_t>(indexOf(kSources, t.source))];
    if (t.source == ExportSource::Tracks)
        text += " (" + std::to_string(t.tracks.size()) + ")";
    if (t.source == ExportSource::Midi)
        return text;
    const char *container = t.format == ExportFormat::Wav ? "WAV" : t.format == ExportFormat::Aiff ? "AIFF" : "FLAC";
    text += std::string("  ") + container + " " + (t.bits == 32 ? std::string("float") : std::to_string(t.bits) + "-bit");
    if (t.normalize == NormalizeMode::Peak)
        text += "  peak " + format("%.1f dB", t.peakDb);
    else if (t.normalize == NormalizeMode::Loudness)
        text += "  " + plain(t.loudnessLufs) + " LUFS";
    return text;
}

// The target at (preset, index), or null when either is gone
const ExportTarget *targetAt(const ProjectStore &store, size_t preset, size_t index)
{
    const std::vector<ExportPreset> &presets = store.project().exportPresets;
    if (preset >= presets.size() || index >= presets[preset].targets.size())
        return nullptr;
    return &presets[preset].targets[index];
}

const ExportPreset *presetAt(const ProjectStore &store, size_t preset)
{
    const std::vector<ExportPreset> &presets = store.project().exportPresets;
    return preset < presets.size() ? &presets[preset] : nullptr;
}

// Changes a target as one edit, and only when something about it really differs
template <typename F>
void editTarget(ProjectStore &store, size_t preset, size_t index, F change)
{
    const ExportTarget *current = targetAt(store, preset, index);
    if (!current)
        return;
    ExportTarget copy = *current;
    change(copy);
    if (copy != *current)
        store.editExportPresets()[preset].targets[index] = copy;
}

// Changes a preset as one edit, and only when it really differs
template <typename F>
void editPreset(ProjectStore &store, size_t preset, F change)
{
    const ExportPreset *current = presetAt(store, preset);
    if (!current)
        return;
    ExportPreset copy = *current;
    change(copy);
    if (copy != *current)
        store.editExportPresets()[preset] = copy;
}

// Changes the details as one edit, and only when they really differ
template <typename F>
void editMetadata(ProjectStore &store, F change)
{
    ExportMetadata copy = store.project().exportMetadata;
    change(copy);
    if (copy != store.project().exportMetadata)
        store.editExportMetadata() = copy;
}

// The name a new preset or target gets: the base, then the first number that isn't taken
std::string freeName(const std::vector<std::string> &taken, const std::string &base)
{
    auto used = [&taken](const std::string &name) { return std::find(taken.begin(), taken.end(), name) != taken.end(); };
    if (!used(base))
        return base;
    for (int n = 2;; ++n) {
        const std::string name = base + " " + std::to_string(n);
        if (!used(name))
            return name;
    }
}

// The selection the dialog keeps: shared with its Export button, which runs after the dialog closes
struct Selection {
    size_t preset = 0;
    size_t target = 0;
};

// A control for one target setting, a ScrubField that steps it (`fine` for the finer step)
ScrubField &scrubTarget(Container &parent, ProjectStore &store, size_t preset, size_t index, const char *name,
                        std::function<std::string(const ExportTarget &)> show,
                        std::function<void(ExportTarget &, int, bool)> step,
                        std::function<bool(const ExportTarget &)> enabled = {})
{
    ProjectStore *s = &store;
    ScrubField &field = parent.add<ScrubField>(
        name,
        [s, preset, index, show] {
            const ExportTarget *t = targetAt(*s, preset, index);
            return t ? show(*t) : std::string("-");
        },
        [s, preset, index, step](int steps, bool fine) {
            editTarget(*s, preset, index, [&step, steps, fine](ExportTarget &t) { step(t, steps, fine); });
        },
        [s, preset, index, enabled] {
            const ExportTarget *t = targetAt(*s, preset, index);
            return t && (!enabled || enabled(*t));
        });
    // Its own width, not the column's
    field.setAlignSelf(Align::Start);
    return field;
}

// The editor: presets, targets and the target's settings in three columns, and the details below.
// Structure (the lists, the form) is rebuilt after the frame when what it shows changes; the
// values in it are kept in step each frame by the syncs.
class ExportEditor : public Container {
public:
    ExportEditor(Context &context, ProjectStore &store, std::shared_ptr<Selection> selection);

    void handleInput(const Input &input) override;

private:
    std::string signature() const;
    void clampSelection();
    void keep(std::function<void()> sync) { syncs_.push_back(std::move(sync)); }
    void syncText(TextField &field, std::function<std::string()> read);
    void rebuild();
    void buildPresets();
    void buildTargets();
    void buildForm();
    void buildDetails(Container &parent);
    void selectPreset(size_t index);
    void selectTarget(size_t index);
    void addPreset();
    void copyPreset();
    void renamePreset();
    void deletePreset();
    void resetPresets();
    void addTarget();
    void copyTarget();
    void deleteTarget();
    const ExportPreset *currentPreset() const { return presetAt(store_, sel_->preset); }
    const ExportTarget *currentTarget() const { return targetAt(store_, sel_->preset, sel_->target); }

    Context &context_;
    ProjectStore &store_;
    std::shared_ptr<Selection> sel_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true); // a post made before this goes is ignored
    ListView *presetList_ = nullptr;
    ListView *targetList_ = nullptr;
    Container *form_ = nullptr;
    std::string built_ = "?";
    bool rebuilding_ = false;
    std::vector<std::function<void()>> syncs_;      // rebuilt with the lists and the form
    std::vector<std::function<void()>> fixedSyncs_; // the details, kept for good
};

ExportEditor::ExportEditor(Context &context, ProjectStore &store, std::shared_ptr<Selection> selection)
    : Container(Direction::Column), context_(context), store_(store), sel_(std::move(selection))
{
    setAlign(Align::Stretch);

    // The three columns: presets, targets, and the selected target. They take the editor's height.
    auto &columns = add<Container>(Direction::Row);
    columns.setAlign(Align::Stretch);
    columns.setGrow(1.0f);

    // Each column's list sits in an inset well that fills the column's height and scrolls
    auto well = [](Container &parent, float width) -> Container & {
        auto &panel = parent.add<Panel>(Direction::Column, Elevation::Inset);
        panel.setAlign(Align::Stretch);
        panel.setDepth(Depth::Subtle);
        panel.setGrow(1.0f);
        panel.setMinSize({width, 0.0f});
        auto &scroll = panel.add<ScrollView>();
        scroll.setGrow(1.0f);
        scroll.content().setAlign(Align::Stretch);
        return scroll.content();
    };
    // A small flat button that shares its row's width evenly with its neighbours
    auto tight = [](Container &row, const char *text, std::function<void()> action) {
        Button &button = row.add<Button>(std::move(action));
        button.setLabel(text).setVariant(ButtonVariant::Flat).setSize(Size::Small);
        button.setGrow(1.0f);
    };

    // Presets, with their buttons in one row under the list
    auto &presets = columns.add<Container>(Direction::Column);
    presets.setAlign(Align::Stretch);
    presets.setMinSize({kColumnWidth, 0.0f});
    presets.add<Label>("Presets", TextStyle::Subtitle);
    presetList_ = &well(presets, kColumnWidth).add<ListView>();
    // Two rows of two, so the buttons don't make the column wider than the list
    auto &presetButtons = presets.add<Grid>(2);
    tight(presetButtons, "Add", [this] { addPreset(); });
    tight(presetButtons, "Copy", [this] { copyPreset(); });
    tight(presetButtons, "Rename", [this] { renamePreset(); });
    tight(presetButtons, "Delete", [this] { deletePreset(); });
    presets.add<Button>([this] { resetPresets(); })
        .setLabel("Reset to defaults")
        .setVariant(ButtonVariant::Flat)
        .setSize(Size::Small)
        .setAlignSelf(Align::Start);

    // Targets, with their buttons in one row under the list
    auto &targets = columns.add<Container>(Direction::Column);
    targets.setAlign(Align::Stretch);
    targets.setMinSize({kColumnWidth, 0.0f});
    targets.add<Label>("Targets", TextStyle::Subtitle);
    targetList_ = &well(targets, kColumnWidth).add<ListView>();
    auto &targetButtons = targets.add<Container>(Direction::Row);
    targetButtons.setAlign(Align::Center);
    tight(targetButtons, "Add", [this] { addTarget(); });
    tight(targetButtons, "Copy", [this] { copyTarget(); });
    tight(targetButtons, "Delete", [this] { deleteTarget(); });

    // The selected target's settings: two columns of them in the well, so most show without scrolling
    auto &editor = columns.add<Container>(Direction::Column);
    editor.setAlign(Align::Stretch);
    editor.setGrow(1.0f);
    editor.add<LiveLabel>(
        [this] {
            const ExportTarget *t = currentTarget();
            return t ? "Target: " + t->name : std::string("No target");
        },
        TextStyle::Subtitle);
    form_ = &well(editor, 0.0f).add<Container>(Direction::Row);
    form_->setAlign(Align::Start);

    buildDetails(*this);
}

void ExportEditor::buildDetails(Container &parent)
{
    // The details written into the files' metadata, below the three columns
    auto &details = parent.add<Container>(Direction::Row);
    details.setAlign(Align::Start);
    struct Field {
        const char *label;
        const char *placeholder;
        std::string ExportMetadata::*member;
    };
    const Field fields[] = {{"Title", "Project name", &ExportMetadata::title},
                            {"Artist", "", &ExportMetadata::artist},
                            {"Album", "", &ExportMetadata::album},
                            {"Comment", "", &ExportMetadata::comment}};
    for (const Field &f : fields) {
        auto &column = details.add<Container>(Direction::Column);
        column.setAlign(Align::Stretch);
        column.setGrow(1.0f);
        column.add<Label>(f.label, TextStyle::Caption, TextTone::Muted);
        TextField &field = column.add<TextField>(f.placeholder);
        field.setMinSize({150.0f, 0.0f});
        ProjectStore *s = &store_;
        std::string ExportMetadata::*member = f.member;
        field.onChange([s, member](const std::string &text) {
            editMetadata(*s, [member, &text](ExportMetadata &m) { m.*member = text; });
        });
        ProjectStore *read = &store_;
        TextField *raw = &field;
        fixedSyncs_.push_back([raw, read, member] {
            if (raw->focused())
                return;
            const std::string value = read->project().exportMetadata.*member;
            if (raw->text() != value)
                raw->setText(value);
        });
    }
}

std::string ExportEditor::signature() const
{
    std::string s = std::to_string(store_.project().exportPresets.size()) + "|" + std::to_string(sel_->preset) + "|" +
                    std::to_string(sel_->target);
    const ExportPreset *preset = currentPreset();
    if (!preset)
        return s;
    s += "|" + std::to_string(preset->targets.size());
    const ExportTarget *t = currentTarget();
    if (!t)
        return s;
    // What changes the shape of the form (its rows and their choices)
    s += "|" + std::to_string(static_cast<int>(t->source)) + "," + std::to_string(static_cast<int>(t->range)) + "," +
         std::to_string(static_cast<int>(t->format)) + "," + std::to_string(t->bits) + "," +
         std::to_string(static_cast<int>(t->normalize));
    for (const Track &track : store_.project().tracks)
        s += "," + track.name;
    return s;
}

void ExportEditor::clampSelection()
{
    const size_t presets = store_.project().exportPresets.size();
    if (sel_->preset >= presets)
        sel_->preset = presets ? presets - 1 : 0;
    const ExportPreset *preset = currentPreset();
    const size_t targets = preset ? preset->targets.size() : 0;
    if (sel_->target >= targets)
        sel_->target = targets ? targets - 1 : 0;
}

void ExportEditor::syncText(TextField &field, std::function<std::string()> read)
{
    TextField *raw = &field;
    keep([raw, read] {
        if (raw->focused())
            return;
        const std::string value = read();
        if (raw->text() != value)
            raw->setText(value);
    });
}

void ExportEditor::handleInput(const Input &input)
{
    clampSelection();
    // Structure changes after the frame, never during it
    if (!rebuilding_ && signature() != built_) {
        rebuilding_ = true;
        std::weak_ptr<bool> alive = alive_;
        context_.post([this, alive] {
            if (alive.expired())
                return;
            built_ = signature();
            rebuild();
            rebuilding_ = false;
        });
    }
    for (auto &sync : fixedSyncs_)
        sync();
    for (auto &sync : syncs_)
        sync();
    Container::handleInput(input);
}

void ExportEditor::rebuild()
{
    syncs_.clear();
    presetList_->clear();
    targetList_->clear();
    form_->clear();
    buildPresets();
    buildTargets();
    buildForm();
}

void ExportEditor::buildPresets()
{
    const std::vector<ExportPreset> &presets = store_.project().exportPresets;
    if (presets.empty()) {
        presetList_->add<Label>("No presets yet. Add one to start.", TextStyle::Caption, TextTone::Muted).setWrapWidth(kColumnWidth - 24.0f);
        return;
    }
    for (size_t i = 0; i < presets.size(); ++i) {
        ListItem &item = presetList_->add<ListItem>(presets[i].name, [this, i] { selectPreset(i); });
        ListItem *raw = &item;
        ProjectStore *s = &store_;
        std::shared_ptr<Selection> sel = sel_;
        keep([raw, s, sel, i] {
            const ExportPreset *p = presetAt(*s, i);
            if (!p)
                return;
            raw->setTitle(p->name);
            raw->setSubtitle(std::to_string(p->targets.size()) + (p->targets.size() == 1 ? " target" : " targets"));
            raw->setSelected(sel->preset == i);
        });
    }
}

void ExportEditor::buildTargets()
{
    const ExportPreset *preset = currentPreset();
    if (!preset) {
        targetList_->add<Label>("Choose or add a preset.", TextStyle::Caption, TextTone::Muted);
        return;
    }
    const size_t presetIndex = sel_->preset;
    for (size_t i = 0; i < preset->targets.size(); ++i) {
        Container &row = targetList_->add<Container>(Direction::Row);
        row.setAlign(Align::Center);
        Checkbox &box = row.add<Checkbox>(std::string(), preset->targets[i].enabled, [this, presetIndex, i](bool on) {
            editTarget(store_, presetIndex, i, [on](ExportTarget &t) { t.enabled = on; });
        });
        ListItem &item = row.add<ListItem>(std::string(), [this, i] { selectTarget(i); });
        item.setGrow(1.0f);
        Checkbox *boxRaw = &box;
        ListItem *raw = &item;
        ProjectStore *s = &store_;
        std::shared_ptr<Selection> sel = sel_;
        keep([boxRaw, raw, s, sel, presetIndex, i] {
            const ExportTarget *t = targetAt(*s, presetIndex, i);
            if (!t)
                return;
            raw->setTitle(t->name);
            raw->setSubtitle(summaryOf(*t));
            raw->setSelected(sel->target == i && sel->preset == presetIndex);
            if (boxRaw->isChecked() != t->enabled)
                boxRaw->setChecked(t->enabled);
        });
    }
}

void ExportEditor::buildForm()
{
    const ExportPreset *preset = currentPreset();
    const ExportTarget *current = currentTarget();
    if (!preset || !current) {
        form_->add<Label>("Add a target to edit it.", TextStyle::Body, TextTone::Muted);
        return;
    }
    // The form is for this target; its controls keep to it, whatever is selected by the time they're used
    const size_t pi = sel_->preset;
    const size_t ti = sel_->target;
    const ExportTarget t = *current;
    const bool midi = t.source == ExportSource::Midi;
    const bool wav = t.format == ExportFormat::Wav;
    ProjectStore *s = &store_;

    // Two grids side by side: the first holds the target and its audio, the second what is done to it
    Grid &left = form_->add<Grid>(2);
    Grid &right = form_->add<Grid>(2);
    Grid *grid = &left;
    auto labelled = [&grid](const char *text) {
        grid->add<Label>(text, TextStyle::Body, TextTone::Secondary).setMinSize({kLabelWidth, 0.0f});
    };
    // A control that is clicked changes the target it was built for, and is kept in step with it
    auto segmented = [&grid](const std::vector<std::string> &options, int selected, std::function<void(int)> onChange) -> SegmentedControl & {
        return grid->add<SegmentedControl>(options, selected, std::move(onChange)).setSize(Size::Small);
    };

    // Name, and the source
    labelled("Name");
    TextField &name = grid->add<TextField>("Target name");
    name.setMinSize({kFieldWidth, 0.0f});
    name.onChange([s, pi, ti](const std::string &text) { editTarget(*s, pi, ti, [&text](ExportTarget &x) { x.name = text; }); });
    syncText(name, [s, pi, ti] {
        const ExportTarget *x = targetAt(*s, pi, ti);
        return x ? x->name : std::string();
    });

    labelled("Source");
    Dropdown &source = grid->add<Dropdown>(context_, kSourceNames, indexOf(kSources, t.source), [s, pi, ti](int i) {
        editTarget(*s, pi, ti, [i](ExportTarget &x) { x.source = kSources[static_cast<size_t>(i)]; });
    });
    source.setAlignSelf(Align::Start);
    keep([&source, s, pi, ti] {
        const ExportTarget *x = targetAt(*s, pi, ti);
        if (x && source.selected() != indexOf(kSources, x->source))
            source.setSelected(indexOf(kSources, x->source));
    });

    // The tracks a chosen-tracks or MIDI target takes, by name
    if (t.source == ExportSource::Tracks || midi) {
        labelled("Tracks");
        Container &list = grid->add<Container>(Direction::Column);
        list.setAlign(Align::Start);
        const std::vector<Track> &tracks = store_.project().tracks;
        if (tracks.empty())
            list.add<Label>("The project has no tracks", TextStyle::Caption, TextTone::Muted);
        for (const Track &track : tracks) {
            const std::string trackName = track.name;
            Checkbox &box = list.add<Checkbox>(trackName, std::find(t.tracks.begin(), t.tracks.end(), trackName) != t.tracks.end(),
                                               [s, pi, ti, trackName](bool on) {
                                                   editTarget(*s, pi, ti, [&trackName, on](ExportTarget &x) {
                                                       auto at = std::find(x.tracks.begin(), x.tracks.end(), trackName);
                                                       if (on && at == x.tracks.end())
                                                           x.tracks.push_back(trackName);
                                                       if (!on && at != x.tracks.end())
                                                           x.tracks.erase(at);
                                                   });
                                               });
            Checkbox *raw = &box;
            keep([raw, s, pi, ti, trackName] {
                const ExportTarget *x = targetAt(*s, pi, ti);
                if (!x)
                    return;
                const bool on = std::find(x->tracks.begin(), x->tracks.end(), trackName) != x->tracks.end();
                if (raw->isChecked() != on)
                    raw->setChecked(on);
            });
        }
    }

    if (!midi) {
        labelled("Tap");
        segmented({"Post-fader", "Pre-fader"}, indexOf(kTaps, t.tap), [s, pi, ti](int i) {
            editTarget(*s, pi, ti, [i](ExportTarget &x) { x.tap = kTaps[static_cast<size_t>(i)]; });
        })
            .setAlignSelf(Align::Start);
        labelled("Mute and solo");
        Checkbox &mute = grid->add<Checkbox>("Respect mute and solo", t.respectMuteSolo, [s, pi, ti](bool on) {
            editTarget(*s, pi, ti, [on](ExportTarget &x) { x.respectMuteSolo = on; });
        });
        mute.setAlignSelf(Align::Start);
        Checkbox *muteRaw = &mute;
        keep([muteRaw, s, pi, ti] {
            const ExportTarget *x = targetAt(*s, pi, ti);
            if (x && muteRaw->isChecked() != x->respectMuteSolo)
                muteRaw->setChecked(x->respectMuteSolo);
        });
    }

    // The part of the song
    labelled("Range");
    Dropdown &range = grid->add<Dropdown>(context_, kRangeNames, indexOf(kRanges, t.range), [s, pi, ti](int i) {
        editTarget(*s, pi, ti, [i](ExportTarget &x) { x.range = kRanges[static_cast<size_t>(i)]; });
    });
    range.setAlignSelf(Align::Start);
    keep([&range, s, pi, ti] {
        const ExportTarget *x = targetAt(*s, pi, ti);
        if (x && range.selected() != indexOf(kRanges, x->range))
            range.setSelected(indexOf(kRanges, x->range));
    });
    if (t.range == ExportRangeKind::Bars) {
        labelled("From bar");
        scrubTarget(*grid, *s, pi, ti, "", [](const ExportTarget &x) { return plain(x.fromBar); },
                    [](ExportTarget &x, int steps, bool fine) {
                        const double step = (fine ? 0.25 : 1.0) * steps;
                        x.fromBar = std::clamp(x.fromBar + step, 1.0, std::max(1.0, x.toBar - 0.25));
                    });
        labelled("To bar");
        scrubTarget(*grid, *s, pi, ti, "", [](const ExportTarget &x) { return plain(x.toBar); },
                    [](ExportTarget &x, int steps, bool fine) {
                        const double step = (fine ? 0.25 : 1.0) * steps;
                        x.toBar = std::max(x.toBar + step, x.fromBar + 0.25);
                    });
    }
    // The tail goes in the right-hand grid, with the mix's finishing
    auto tail = [&] {
        labelled("Tail");
        scrubTarget(*grid, *s, pi, ti, "", [](const ExportTarget &x) { return format("%.1f s", x.tailSeconds); },
                    [](ExportTarget &x, int steps, bool fine) {
                        x.tailSeconds = std::clamp(x.tailSeconds + (fine ? 0.1 : 0.5) * steps, 0.0, 30.0);
                    })
            .setValueWidth(90.0f);
    };

    // The file: format, bits, rate, channels, normalising, limiter, fades, dither (audio only)
    if (!midi) {
        labelled("Format");
        segmented({"WAV", "AIFF", "FLAC"}, indexOf(kFormats, t.format), [s, pi, ti](int i) {
            editTarget(*s, pi, ti, [i](ExportTarget &x) {
                x.format = kFormats[static_cast<size_t>(i)];
                if (x.format != ExportFormat::Wav && x.bits == 32)
                    x.bits = 24;
            });
        }).setAlignSelf(Align::Start);

        labelled("Bits");
        // 32-bit float is for WAV only
        const std::vector<int> bitChoices = wav ? kBits : std::vector<int>{16, 24};
        const std::vector<std::string> bitNames = wav ? std::vector<std::string>{"16", "24", "32 float"}
                                                      : std::vector<std::string>{"16", "24"};
        segmented(bitNames, indexOf(bitChoices, t.bits), [s, pi, ti, bitChoices](int i) {
            const int bits = bitChoices[static_cast<size_t>(i)];
            editTarget(*s, pi, ti, [bits](ExportTarget &x) { x.bits = bits; });
        }).setAlignSelf(Align::Start);

        labelled("Sample rate");
        Dropdown &rate = grid->add<Dropdown>(context_, kRateNames, indexOf(kRates, t.sampleRate), [s, pi, ti](int i) {
            editTarget(*s, pi, ti, [i](ExportTarget &x) { x.sampleRate = kRates[static_cast<size_t>(i)]; });
        });
        rate.setAlignSelf(Align::Start);
        keep([&rate, s, pi, ti] {
            const ExportTarget *x = targetAt(*s, pi, ti);
            if (x && rate.selected() != indexOf(kRates, x->sampleRate))
                rate.setSelected(indexOf(kRates, x->sampleRate));
        });

        labelled("Channels");
        grid->add<Checkbox>("Mix down to mono", t.mono, [s, pi, ti](bool on) {
            editTarget(*s, pi, ti, [on](ExportTarget &x) { x.mono = on; });
        }).setAlignSelf(Align::Start);

        grid = &right;
        tail();
        labelled("Normalize");
        Dropdown &normalize = grid->add<Dropdown>(context_, kNormalizeNames, indexOf(kNormalize, t.normalize), [s, pi, ti](int i) {
            editTarget(*s, pi, ti, [i](ExportTarget &x) { x.normalize = kNormalize[static_cast<size_t>(i)]; });
        });
        normalize.setAlignSelf(Align::Start);
        keep([&normalize, s, pi, ti] {
            const ExportTarget *x = targetAt(*s, pi, ti);
            if (x && normalize.selected() != indexOf(kNormalize, x->normalize))
                normalize.setSelected(indexOf(kNormalize, x->normalize));
        });
        if (t.normalize == NormalizeMode::Peak) {
            labelled("Peak level");
            scrubTarget(*grid, *s, pi, ti, "", [](const ExportTarget &x) { return format("%.1f dB", x.peakDb); },
                        [](ExportTarget &x, int steps, bool fine) {
                            x.peakDb = std::clamp(x.peakDb + (fine ? 0.1 : 0.5) * steps, -30.0, 0.0);
                        })
                .setValueWidth(90.0f);
        } else if (t.normalize == NormalizeMode::Loudness) {
            labelled("Loudness");
            scrubTarget(*grid, *s, pi, ti, "", [](const ExportTarget &x) { return format("%.1f LUFS", x.loudnessLufs); },
                        [](ExportTarget &x, int steps, bool fine) {
                            x.loudnessLufs = std::clamp(x.loudnessLufs + (fine ? 0.1 : 0.5) * steps, -40.0, 0.0);
                        })
                .setValueWidth(90.0f);
            labelled("Preset");
            Dropdown &choice = grid->add<Dropdown>(context_, kLoudnessNames, loudnessChoice(t.loudnessLufs), [s, pi, ti](int i) {
                if (i > 0)
                    editTarget(*s, pi, ti, [i](ExportTarget &x) { x.loudnessLufs = kLoudnessValues[static_cast<size_t>(i - 1)]; });
            });
            choice.setAlignSelf(Align::Start);
            keep([&choice, s, pi, ti] {
                const ExportTarget *x = targetAt(*s, pi, ti);
                if (x && choice.selected() != loudnessChoice(x->loudnessLufs))
                    choice.setSelected(loudnessChoice(x->loudnessLufs));
            });
        }

        labelled("Limiter");
        grid->add<Checkbox>("True-peak limiter", t.limiter, [s, pi, ti](bool on) {
            editTarget(*s, pi, ti, [on](ExportTarget &x) { x.limiter = on; });
        }).setAlignSelf(Align::Start);
        labelled("Ceiling");
        scrubTarget(*grid, *s, pi, ti, "", [](const ExportTarget &x) { return format("%.1f dBTP", x.ceilingDbtp); },
                    [](ExportTarget &x, int steps, bool fine) {
                        x.ceilingDbtp = std::clamp(x.ceilingDbtp + (fine ? 0.1 : 0.5) * steps, -12.0, 0.0);
                    },
                    [](const ExportTarget &x) { return x.limiter; })
            .setValueWidth(90.0f);

        auto fade = [](double ExportTarget::*member) {
            return [member](ExportTarget &x, int steps, bool fine) {
                x.*member = std::clamp(x.*member + (fine ? 1.0 : 10.0) * steps, 0.0, 10000.0);
            };
        };
        auto fadeText = [](double ExportTarget::*member) {
            return [member](const ExportTarget &x) { return format("%.0f ms", x.*member); };
        };
        labelled("Fade in");
        scrubTarget(*grid, *s, pi, ti, "", fadeText(&ExportTarget::fadeInMs), fade(&ExportTarget::fadeInMs)).setValueWidth(90.0f);
        labelled("Fade out");
        scrubTarget(*grid, *s, pi, ti, "", fadeText(&ExportTarget::fadeOutMs), fade(&ExportTarget::fadeOutMs)).setValueWidth(90.0f);

        if (t.bits != 32) {
            labelled("Dither");
            Dropdown &dither = grid->add<Dropdown>(context_, kDitherNames, indexOf(kDithers, t.dither), [s, pi, ti](int i) {
                editTarget(*s, pi, ti, [i](ExportTarget &x) { x.dither = kDithers[static_cast<size_t>(i)]; });
            });
            dither.setAlignSelf(Align::Start);
            keep([&dither, s, pi, ti] {
                const ExportTarget *x = targetAt(*s, pi, ti);
                if (x && dither.selected() != indexOf(kDithers, x->dither))
                    dither.setSelected(indexOf(kDithers, x->dither));
            });
        }
    }

    // The names of the files it writes, and where
    grid = &right;
    if (midi)
        tail();
    labelled("File name");
    Container &nameColumn = grid->add<Container>(Direction::Column);
    nameColumn.setAlign(Align::Stretch);
    TextField &fileName = nameColumn.add<TextField>("{project}");
    fileName.setMinSize({kFieldWidth, 0.0f});
    fileName.onChange([s, pi, ti](const std::string &text) { editTarget(*s, pi, ti, [&text](ExportTarget &x) { x.fileName = text; }); });
    syncText(fileName, [s, pi, ti] {
        const ExportTarget *x = targetAt(*s, pi, ti);
        return x ? x->fileName : std::string();
    });
    // The resolved names: the first two tracks for an output per track
    LiveLabel &preview = nameColumn.add<LiveLabel>(
        [s, pi, ti] {
            const ExportPreset *p = presetAt(*s, pi);
            const ExportTarget *x = targetAt(*s, pi, ti);
            if (!p || !x)
                return std::string();
            const Project &project = s->project();
            const std::time_t now = std::time(nullptr);
            if (x->source == ExportSource::EachTrack) {
                std::string text;
                const size_t count = project.tracks.size();
                for (size_t k = 0; k < std::min<size_t>(2, count); ++k)
                    text += (k ? "    " : "") + resolveFileName(*x, project, p->name, project.tracks[k].name, now);
                if (count > 2)
                    text += "    +" + std::to_string(count - 2) + " more";
                return count ? text : resolveFileName(*x, project, p->name, std::string(), now);
            }
            return resolveFileName(*x, project, p->name, std::string(), now);
        },
        TextStyle::Caption, TextTone::Muted);
    preview.setWrapWidth(kFieldWidth);

    labelled("Folder");
    TextField &folder = grid->add<TextField>("Beside the project");
    folder.setMinSize({kFieldWidth, 0.0f});
    folder.onChange([s, pi, ti](const std::string &text) { editTarget(*s, pi, ti, [&text](ExportTarget &x) { x.folder = text; }); });
    syncText(folder, [s, pi, ti] {
        const ExportTarget *x = targetAt(*s, pi, ti);
        return x ? x->folder : std::string();
    });
}

void ExportEditor::selectPreset(size_t index)
{
    sel_->preset = index;
    sel_->target = 0;
}

void ExportEditor::selectTarget(size_t index)
{
    sel_->target = index;
}

void ExportEditor::addPreset()
{
    std::vector<std::string> names;
    for (const ExportPreset &p : store_.project().exportPresets)
        names.push_back(p.name);
    ExportPreset preset;
    preset.name = freeName(names, "Preset");
    preset.targets.push_back(ExportTarget{});
    store_.editExportPresets().push_back(preset);
    sel_->preset = store_.project().exportPresets.size() - 1;
    sel_->target = 0;
}

void ExportEditor::copyPreset()
{
    const ExportPreset *current = currentPreset();
    if (!current)
        return;
    std::vector<std::string> names;
    for (const ExportPreset &p : store_.project().exportPresets)
        names.push_back(p.name);
    ExportPreset copy = *current;
    copy.name = freeName(names, current->name + " copy");
    store_.editExportPresets().push_back(copy);
    sel_->preset = store_.project().exportPresets.size() - 1;
    sel_->target = 0;
}

void ExportEditor::renamePreset()
{
    const ExportPreset *current = currentPreset();
    if (!current)
        return;
    // The dialog opens after this frame; the name is what its field holds when Rename is pressed
    ProjectStore *s = &store_;
    std::shared_ptr<Selection> sel = sel_;
    auto name = std::make_shared<std::string>(current->name);
    ui::Context *context = &context_;
    context_.post([s, sel, name, context] {
        Dialog &dialog = Dialog::open(*context, "Rename preset", "");
        dialog.setWidth(360.0f);
        TextField &field = dialog.body().add<TextField>("Preset name");
        field.setText(*name);
        field.setMinSize({300.0f, 0.0f});
        field.onChange([name](const std::string &text) { *name = text; });
        TextField *raw = &field;
        auto focused = std::make_shared<bool>(false);
        dialog.body().add<FrameHook>([raw, focused](const Input &) {
            if (!*focused) {
                raw->focus();
                *focused = true;
            }
        });
        dialog.addButton("Cancel");
        dialog.addButton("Rename", [s, sel, name] {
            const std::string text = *name;
            if (text.find_first_not_of(" \t") == std::string::npos)
                return;
            editPreset(*s, sel->preset, [&text](ExportPreset &p) { p.name = text; });
        }, true);
    });
}

void ExportEditor::deletePreset()
{
    const ExportPreset *current = currentPreset();
    if (!current)
        return;
    ProjectStore *s = &store_;
    std::shared_ptr<Selection> sel = sel_;
    const size_t index = sel_->preset;
    const std::string name = current->name;
    ui::Context *context = &context_;
    context_.post([s, sel, index, name, context] {
        Dialog::open(*context, "Delete preset", "Delete \"" + name + "\" and its targets? Undo brings it back.")
            .addButton("Cancel")
            .addButton("Delete", [s, sel, index] {
                std::vector<ExportPreset> &presets = s->editExportPresets();
                if (index >= presets.size())
                    return;
                presets.erase(presets.begin() + static_cast<std::ptrdiff_t>(index));
                sel->preset = presets.empty() ? 0 : std::min(index, presets.size() - 1);
                sel->target = 0;
            }, true);
    });
}

void ExportEditor::resetPresets()
{
    ProjectStore *s = &store_;
    std::shared_ptr<Selection> sel = sel_;
    ui::Context *context = &context_;
    context_.post([s, sel, context] {
        Dialog::open(*context, "Reset export presets",
                     "Put back the presets a new project starts with (Mix, Release, Stems, MIDI)? Undo brings yours back.")
            .addButton("Cancel")
            .addButton("Reset", [s, sel] {
                s->editExportPresets() = defaultExportPresets();
                sel->preset = 0;
                sel->target = 0;
            }, true);
    });
}

void ExportEditor::addTarget()
{
    const ExportPreset *current = currentPreset();
    if (!current)
        return;
    std::vector<std::string> names;
    for (const ExportTarget &t : current->targets)
        names.push_back(t.name);
    ExportTarget target;
    target.name = freeName(names, "Mix");
    editPreset(store_, sel_->preset, [&target](ExportPreset &p) { p.targets.push_back(target); });
    sel_->target = store_.project().exportPresets[sel_->preset].targets.size() - 1;
}

void ExportEditor::copyTarget()
{
    const ExportPreset *current = currentPreset();
    if (!current || current->targets.empty())
        return;
    std::vector<std::string> names;
    for (const ExportTarget &t : current->targets)
        names.push_back(t.name);
    ExportTarget copy = current->targets[std::min(sel_->target, current->targets.size() - 1)];
    copy.name = freeName(names, copy.name + " copy");
    editPreset(store_, sel_->preset, [&copy](ExportPreset &p) { p.targets.push_back(copy); });
    sel_->target = store_.project().exportPresets[sel_->preset].targets.size() - 1;
}

void ExportEditor::deleteTarget()
{
    const ExportPreset *current = currentPreset();
    if (!current || current->targets.empty())
        return;
    const size_t index = std::min(sel_->target, current->targets.size() - 1);
    editPreset(store_, sel_->preset, [index](ExportPreset &p) { p.targets.erase(p.targets.begin() + static_cast<std::ptrdiff_t>(index)); });
    const size_t left = store_.project().exportPresets[sel_->preset].targets.size();
    sel_->target = left ? std::min(index, left - 1) : 0;
}

}

void openExportDialog(Context &context, ProjectStore &store, ExportDialogHooks hooks)
{
    auto selection = std::make_shared<Selection>();
    Dialog &dialog = Dialog::open(context, "Export", "A preset is a list of files, rendered together when you press Export.");
    // About 1180 by 820 on a screen this size, smaller in a smaller window
    const Rect window = context.bounds();
    dialog.setWidth(std::min(1180.0f, window.w - 64.0f));
    dialog.body().add<ExportEditor>(context, store, selection).setMinSize({0.0f, std::clamp(window.h - 200.0f, 360.0f, 680.0f)});
    dialog.addButton("Close");
    dialog.addButton("Export", [run = std::move(hooks.run), selection] {
        if (run)
            run(selection->preset);
    }, true);
}

}
