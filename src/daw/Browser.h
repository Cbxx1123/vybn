#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "DeviceCatalogue.h"
#include "Library.h"
#include "Online.h"
#include "plugin/Protocol.h"
#include "ui/UI.h"

namespace daw {

// What the browser does with what is chosen in it, and what it shows about the project
struct BrowserHooks {
    // Where the instruments come from: the library's and the plugins installed, side by side
    DeviceCatalogue *catalogue = nullptr;
    // Plays an instrument (by its catalogue key: "sampler:<name>", "plugin:<id>") on the selected
    // instrument track (or a new one); what that will be
    std::function<void(const std::string &key)> useInstrument;
    // Starts dragging an instrument, by its catalogue key, to drop on a track
    std::function<void(const std::string &key)> dragInstrument;
    std::function<std::string()> instrumentTarget; // "on Lead", "on a new instrument track"
    std::function<std::string()> currentInstrument; // the selected track's instrument's catalogue key, highlighted
    // Puts a sound file on the selected audio track (or a new one) at the playhead
    std::function<void(const std::string &path)> addSound;
    std::function<std::string()> soundTarget;
    // Listening first: a phrase on an instrument, or a sound file; stop; whether one is playing
    std::function<void(const std::string &name)> previewInstrument;
    std::function<void(const std::string &path)> previewSound;
    // Listening to a built-in synthesizer preset (its setting, with the preset values)
    std::function<void(const audio::DeviceSetting &setting)> previewSynth;
    std::function<void()> stopPreview;
    std::function<bool()> previewing;
    std::function<void()> importInstrument;
    std::function<void(const std::string &category)> importSamples;
    std::function<void()> openSettings;
    // After a download is installed: its kind and where it went
    std::function<void(AssetKind kind, const std::string &path)> installed;
};

// The asset browser: a panel that slides up over the bottom of the window (Tab by default,
// through the view.browser action; Escape or the close button put it away). Two tabs:
// - Library: every instrument, the library's on disk and the plugins installed side by side, and
//   the samples, by category (Guitars, Drums, Synths, ...), with a search. An instrument can be previewed (a short phrase), put on the selected instrument track
//   (double-click or Enter does that too) or moved to another category; a sample can be played,
//   put on the selected audio track at the playhead, or moved.
// - Online: what the sources offer (OnlineCatalogue: GitHub's SFZ instruments, Freesound, and
//   the user's own catalogues), by category, with a search (sent to Freesound as a query).
//   Download fetches, unpacks and installs an item into the library (Downloads) with progress
//   and a Cancel; once installed it can be used like a library item.
// Up and Down move through the list while it is open. It is an overlay that stays open for the
// life of the window, hidden when put away (taking no input), so dialogs and menus open on top.
class AssetBrowser : public ui::Component {
public:
    enum class Tab { Library, Online };
    enum class Kind { All, Instruments, Samples };

    AssetBrowser(ui::Context &context, InstrumentLibrary &library, OnlineCatalogue &online, Downloads &downloads,
                 BrowserHooks hooks);
    ~AssetBrowser() override;
    // Opens it as an overlay of `context`, put away
    static AssetBrowser &open(ui::Context &context, InstrumentLibrary &library, OnlineCatalogue &online,
                              Downloads &downloads, BrowserHooks hooks);

    void show();
    void hide();
    void toggle() { shown_ ? hide() : show(); }
    bool shown() const { return shown_; }
    // Shows the instruments with `key` (a catalogue key) selected
    void chooseInstrument(const std::string &key);
    void setTab(Tab tab);
    Tab tab() const { return tab_; }
    // How far it has slid up, 0-1 (for tests and screenshots)
    float openness() const { return open_; }

    ui::Vec2 measure(const ui::Theme &) const override { return {}; }
    void arrange(const ui::Rect &window, const ui::Theme &theme) override;
    void handleInput(const ui::Input &input) override;
    void draw(ui::DrawList &list, const ui::Theme &theme) const override;
    bool hitTest(ui::Vec2 p) const override { return shown_ && rect_.contains(p); }

    // One row of the list
    struct Entry {
        enum class From { Instrument, Sample, Online, Plugin, Builtin } from = From::Instrument;
        std::string key; // unique: an instrument's catalogue key, "s:path", "o:source/id"
        std::string title, subtitle, category;
        LibraryInstrument instrument;
        LibrarySample sample;
        OnlineItem item;
        CatalogueEntry device; // an instrument plugin's catalogue entry
    };

private:
    void build();
    // The rows the filters let through
    std::vector<Entry> entries() const;
    std::string stamp() const;
    void rebuild();
    void rebuildCategories(const std::vector<Entry> &all);
    void rebuildDetail();
    const Entry *selected() const;
    void select(const std::string &key);
    void moveSelection(int by);
    // Enter or a double-click: use it (or download it, online)
    void activate(const Entry &e);
    void preview(const Entry &e);
    std::string installedPath(const Entry &e) const;
    std::string trailing(const Entry &e) const;
    bool keyPressed(const ui::KeyEvent &e);

    ui::Context &context_;
    InstrumentLibrary &library_;
    OnlineCatalogue &online_;
    Downloads &downloads_;
    BrowserHooks hooks_;
    std::unique_ptr<ui::Panel> panel_;
    ui::Hooks keys_;

    bool shown_ = false;
    float open_ = 0.0f; // slides toward shown_
    double now_ = 0.0;
    Tab tab_ = Tab::Library;
    Kind kind_ = Kind::All;
    std::string category_; // "" for every category
    std::string filter_;
    size_t source_ = 0;
    double filterChanged_ = -1.0; // when the search last changed, to wait before asking a server
    std::string asked_;           // the query last sent to a searchable source
    std::string selectedKey_;
    std::string lastClickKey_;
    double lastClickTime_ = -10.0;
    std::string message_;   // the latest thing done, under the details
    std::vector<Entry> shownEntries_;
    std::string built_;     // stamp of the last rebuild
    std::string detailBuilt_;
    bool pending_ = false;

    // Parts rebuilt as the filters change
    ui::SegmentedControl *tabs_ = nullptr;
    ui::SegmentedControl *kinds_ = nullptr;
    ui::Pages *sourcePages_ = nullptr;
    ui::Dropdown *sources_ = nullptr;
    ui::TextField *search_ = nullptr;
    ui::Container *categories_ = nullptr;
    ui::Container *list_ = nullptr;
    ui::ScrollView *listScroll_ = nullptr;
    ui::Container *detail_ = nullptr;
};

}
