// DawApp: the asset browser's side of things: putting instruments and sounds on tracks,
// importing samples, and what happens after a download
#include "daw/app/DawApp.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "daw/views/Arrangement.h"
#include "daw/views/Browser.h"
#include "daw/widgets/Format.h"
#include "daw/views/Mixer.h"
#include "daw/views/TransportBar.h"
#include "engine/FileDialog.h"

namespace daw {

using namespace ui;
namespace fs = std::filesystem;

namespace {

// An instrument being dragged out of the browser: a raised pill with its name under the mouse,
// following it until the button is let go. It reads the mouse from the desktop, so it follows it
// over every part of the window, and it never takes the mouse itself.
class DragGhost : public Component {
public:
    DragGhost(Context &context, std::string name, std::function<void(Vec2)> moved, std::function<void(Vec2)> dropped)
        : context_(context), name_(std::move(name)), moved_(std::move(moved)), dropped_(std::move(dropped))
    {
    }

    Vec2 measure(const Theme &) const override { return {}; }
    void arrange(const Rect &window, const Theme &) override { rect_ = window; }
    bool hitTest(Vec2) const override { return false; }

    void handleInput(const Input &input) override
    {
        int x = 0, y = 0;
        mouse_ = engine::Window::desktopCursor(x, y) ? context_.fromScreen({static_cast<float>(x), static_cast<float>(y)})
                                                     : input.mouse;
        if (done_)
            return;
        if (!engine::Window::primaryButtonHeld()) {
            done_ = true;
            dropped_(mouse_);
            context_.closeOverlay(*this);
            return;
        }
        moved_(mouse_);
        shown_ += (1.0f - shown_) * input.ease(18.0f);
    }

    void draw(DrawList &list, const Theme &theme) const override
    {
        if (done_)
            return;
        const float h = theme.controlHeight(Size::Small);
        const float w = theme.measureText(name_, Size::Small).x + h + 22.0f;
        const Rect r{mouse_.x + 14.0f, mouse_.y + 12.0f, w, h};
        list.add(theme.surface(r, Shape::Pill, Elevation::Raised, shown_, Finish::Convex));
        drawIcon(list, theme, Icon::Keys, {r.x + h * 0.25f, r.y + h * 0.2f, h * 0.6f, h * 0.6f}, TextTone::Accent);
        list.add(theme.text(name_, {r.x + h, r.y, w - h - 10.0f, h}, theme.fontSize(Size::Small), TextAlign::Start,
                            theme.textColor(TextTone::Primary)));
    }

private:
    Context &context_;
    std::string name_;
    std::function<void(Vec2)> moved_, dropped_;
    Vec2 mouse_;
    float shown_ = 0.0f;
    bool done_ = false;
};

}

void DawApp::buildBrowser()
{
    BrowserHooks hooks;
    hooks.catalogue = catalogue_.get();
    hooks.useInstrument = [this](const std::string &key) { useDevice(key); };
    hooks.dragInstrument = [this](const std::string &key) {
        // A library instrument or a plugin, by its catalogue key; the ghost shows its name
        const CatalogueEntry *entry = catalogue_->find(key);
        if (!entry)
            return;
        const std::string name = entry->device.name;
        OverlayOptions options;
        options.closeOnOutsideClick = false;
        gui_.openOverlay<DragGhost>(
            std::move(options), gui_, name, [this, name](Vec2 at) {
                const int row = instrumentDropRow(at);
                arrangement_->setDropTarget(row, row == static_cast<int>(project().tracks.size()) ? "New track playing " + name
                                                                                                    : std::string());
            },
            [this, key](Vec2 at) {
                arrangement_->setDropTarget(-1);
                dropInstrument(key, instrumentDropRow(at));
            });
    };
    hooks.instrumentTarget = [this] {
        const Track *t = project().track(project().selectedTrack);
        return t && t->kind == TrackKind::Instrument ? "on " + t->name : std::string("on a new instrument track");
    };
    hooks.currentInstrument = [this] {
        const Track *t = project().track(project().selectedTrack);
        return t && t->kind == TrackKind::Instrument ? DeviceCatalogue::keyOf(t->instrument) : std::string();
    };
    hooks.addSound = [this](const std::string &path) { addSound(path); };
    hooks.soundTarget = [this] {
        const Track *t = project().track(project().selectedTrack);
        const double bar = std::floor(transportBeats(audio_) / std::max(1, project().beatsPerBar)) + 1.0;
        const std::string at = format("at bar %.0f", bar);
        return (t && t->kind == TrackKind::Audio ? "to " + t->name : std::string("to a new audio track")) + ", " + at;
    };
    hooks.previewInstrument = [this](const std::string &name) { previewInstrument(name); };
    hooks.previewSound = [this](const std::string &path) { previewSound(path); };
    hooks.previewSynth = [this](const audio::DeviceSetting &setting) { previewSynth(setting); };
    hooks.stopPreview = [this] { stopPreview(); };
    hooks.previewing = [this] { return previewing(); };
    hooks.importInstrument = [this] { importInstrument(); };
    hooks.importSamples = [this](const std::string &category) { importSamples(category); };
    hooks.openSettings = [this] { openSettings(); };
    hooks.installed = [this](AssetKind kind, const std::string &path) {
        if (kind != AssetKind::Instrument)
            return;
        // Tracks that asked for it before it was installed load it now
        bridge_->reloadInstrument(fs::u8path(path).filename().string());
    };
    browser_ = &AssetBrowser::open(gui_, *library_, *online_, *downloads_, std::move(hooks));
}

void DawApp::toggleBrowser()
{
    browser_->toggle();
}

void DawApp::chooseInstrument() { pickDevice(true, false, project().selectedTrack); }

int DawApp::instrumentDropRow(Vec2 at) const
{
    // Only on the arrangement in this window, not where the browser covers it
    if (dock_->isFloating(*arrangementPanel_) || !dock_->isShowing(*arrangementPanel_) || (browser_ && browser_->hitTest(at)))
        return -1;
    const int row = arrangement_->trackAt(at);
    if (row < 0 || row == static_cast<int>(project().tracks.size()))
        return row;
    // Audio tracks don't play instruments
    return project().tracks[static_cast<size_t>(row)].kind == TrackKind::Instrument ? row : -1;
}

void DawApp::dropInstrument(const std::string &key, int row)
{
    if (row < 0)
        return;
    if (row < static_cast<int>(project().tracks.size()))
        session_.store().select(row);
    if (const CatalogueEntry *e = catalogue_->find(key); e && e->instrument)
        setInstrument(row, e->device); // the empty space under the tracks: a new track
}

void DawApp::addSound(const std::string &path)
{
    ui_.post([this, path] { importAudioFiles({path}); });
}

void DawApp::importSamples(const std::string &category)
{
    ui::FileDialogOptions options;
    options.title = "Add samples to the library";
    options.filters = {{"Sound files (WAV, FLAC, MP3)", "*.wav;*.flac;*.mp3"}, {"All files", "*.*"}};
    options.multiple = true;
    options.action = "Add";
    options.remember = "samples";
    options.folder = engine::knownFolder(engine::KnownFolder::Downloads);
    chooseFiles(std::move(options), [this, category](std::vector<std::string> files) {
        std::string error;
        const int copied = library_->importSamples(files, category, error);
        if (copied == 0)
            Dialog::open(gui_, "No samples added", error).addButton("OK", {}, true);
    });
}

// Listening in the browser: the previews are the PreviewPlayer's

void DawApp::previewInstrument(const std::string &name) { preview_->previewInstrument(name); }

void DawApp::previewSound(const std::string &path) { preview_->previewSound(path); }

void DawApp::previewSynth(const audio::DeviceSetting &setting) { preview_->previewSynth(setting); }

void DawApp::stopPreview() { preview_->stopPreview(); }

bool DawApp::previewing() const { return preview_->previewing(); }

}
