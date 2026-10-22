#include "daw/views/Browser.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <utility>

#include "daw/widgets/Format.h"
#include "daw/widgets/Widgets.h"
#include "engine/Shell.h"

namespace daw {

using namespace ui;
namespace fs = std::filesystem;

namespace {

constexpr size_t kMaxRows = 300; // more than this asks for a search
constexpr double kSearchDelay = 0.6; // seconds of no typing before a server is asked

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string sizeText(uint64_t bytes)
{
    if (bytes == 0)
        return {};
    if (bytes < 1024 * 1024)
        return format("%.0f KB", std::max(1.0, bytes / 1024.0));
    if (bytes < 1024ull * 1024 * 1024)
        return format("%.0f MB", bytes / (1024.0 * 1024.0));
    return format("%.1f GB", bytes / (1024.0 * 1024.0 * 1024.0));
}

// Every word of the search somewhere in the text
bool matches(const std::string &text, const std::string &filter)
{
    if (filter.empty())
        return true;
    const std::string t = lower(text);
    size_t start = 0;
    const std::string f = lower(filter);
    while (start < f.size()) {
        size_t end = f.find(' ', start);
        if (end == std::string::npos)
            end = f.size();
        if (end > start && t.find(f.substr(start, end - start)) == std::string::npos)
            return false;
        start = end + 1;
    }
    return true;
}

// A ListItem whose trailing text follows a callback (download progress, "On track")
class AssetRow : public ListItem {
public:
    AssetRow(std::string title, std::function<void()> onClick, std::function<std::string()> trailing)
        : ListItem(std::move(title), std::move(onClick)), trailing_(std::move(trailing))
    {
    }

    // Dragging the row (past a few pixels) calls this once, instead of only clicking it
    void setDrag(std::function<void()> onDrag) { onDrag_ = std::move(onDrag); }

    void handleInput(const Input &input) override
    {
        if (onDrag_) {
            if (input.pressed && rect_.contains(input.mouse)) {
                pressAt_ = input.mouse;
                armed_ = true;
            }
            if (!input.down)
                armed_ = false;
            if (armed_ && std::abs(input.mouse.x - pressAt_.x) + std::abs(input.mouse.y - pressAt_.y) > 8.0f) {
                armed_ = false;
                onDrag_();
            }
        }
        if (trailing_) {
            std::string text = trailing_();
            if (text != shown_) {
                shown_ = text;
                setTrailing(std::move(text));
            }
        }
        ListItem::handleInput(input);
    }

private:
    std::function<std::string()> trailing_;
    std::string shown_;
    std::function<void()> onDrag_;
    Vec2 pressAt_;
    bool armed_ = false;
};

Container &row(Container &parent)
{
    auto &r = parent.add<Container>(Direction::Row);
    r.setAlign(Align::Center);
    return r;
}

}

AssetBrowser::AssetBrowser(Context &context, InstrumentLibrary &library, OnlineCatalogue &online, Downloads &downloads,
                           BrowserHooks hooks)
    : context_(context), library_(library), online_(online), downloads_(downloads), hooks_(std::move(hooks))
{
    build();
}

AssetBrowser::~AssetBrowser() = default;

AssetBrowser &AssetBrowser::open(Context &context, InstrumentLibrary &library, OnlineCatalogue &online,
                                 Downloads &downloads, BrowserHooks hooks)
{
    OverlayOptions options;
    options.closeOnOutsideClick = false;
    return context.openOverlay<AssetBrowser>(std::move(options), context, library, online, downloads, std::move(hooks));
}

void AssetBrowser::build()
{
    panel_ = std::make_unique<Panel>(Direction::Column);
    panel_->setDepth(Depth::Deep);
    panel_->setAlign(Align::Stretch);

    // Header: the tabs, the online source, the search, close
    auto &header = row(*panel_);
    header.add<Label>("Browser", TextStyle::Subtitle);
    tabs_ = &header.add<SegmentedControl>(std::vector<std::string>{"Library", "Online"}, 0,
                                          [this](int i) { setTab(i == 1 ? Tab::Online : Tab::Library); });
    tabs_->setSize(Size::Small);
    sourcePages_ = &header.add<Pages>();
    sourcePages_->add<Spacer>();
    std::vector<std::string> names;
    for (const SourceConfig &s : online_.sources())
        names.push_back(s.name);
    if (names.empty())
        names.push_back("No sources");
    sources_ = &sourcePages_->add<Dropdown>(context_, names, 0, [this](int i) {
        source_ = static_cast<size_t>(std::max(0, i));
        category_.clear();
        selectedKey_.clear();
        asked_.clear();
        filterChanged_ = now_;
    });
    sources_->setSize(Size::Small);
    sourcePages_->setMinSize({240.0f, 0.0f});
    search_ = &header.add<TextField>("Search");
    search_->setSize(Size::Small).setGrow(1.0f);
    search_->onChange([this](const std::string &text) {
        filter_ = text;
        filterChanged_ = now_;
    });
    search_->onSubmit([this](const std::string &) {
        filterChanged_ = now_ - kSearchDelay; // ask now
        if (const Entry *e = selected())
            activate(*e);
    });
    header.add<Button>([this] {
              if (tab_ == Tab::Online)
                  online_.fetch(source_, asked_, true);
              else
                  library_.rescan();
          })
        .setLabel("Refresh")
        .setVariant(ButtonVariant::Flat)
        .setSize(Size::Small);
    header.add<Button>([this] { hide(); }).setIcon(Icon::Close).setShape(Shape::Circle).setSize(Size::Small);

    // Body: categories, the list, the details
    auto &body = panel_->add<Container>(Direction::Row);
    body.setAlign(Align::Stretch).setGrow(1.0f);

    auto &categoryScroll = body.add<ScrollView>();
    categoryScroll.setMinSize({200.0f, 0.0f});
    categoryScroll.content().setAlign(Align::Stretch);
    categories_ = &categoryScroll.content().add<ListView>();
    categories_->setAlign(Align::Stretch);

    auto &middle = body.add<Container>(Direction::Column);
    middle.setAlign(Align::Stretch).setGrow(1.0f);
    auto &filters = row(middle);
    kinds_ = &filters.add<SegmentedControl>(std::vector<std::string>{"All", "Instruments", "Samples"}, 0, [this](int i) {
        kind_ = static_cast<Kind>(i);
    });
    kinds_->setSize(Size::Small);
    filters.add<LiveLabel>([this] {
        if (tab_ == Tab::Online) {
            const auto &l = online_.listing(source_);
            if (l.loading)
                return std::string("Loading...");
        }
        const size_t n = shownEntries_.size();
        if (n >= kMaxRows)
            return format("The first %.0f: search to narrow them", static_cast<double>(n));
        return format("%.0f item", static_cast<double>(n)) + (n == 1 ? "" : "s");
    }, TextStyle::Caption, TextTone::Secondary).setGrow(1.0f);
    filters.add<Button>([this] {
              if (hooks_.importInstrument)
                  hooks_.importInstrument();
          })
        .setLabel("Import instrument")
        .setIcon(Icon::Plus)
        .setVariant(ButtonVariant::Flat)
        .setSize(Size::Small);
    filters.add<Button>([this] {
              if (hooks_.importSamples)
                  hooks_.importSamples(category_);
          })
        .setLabel("Add samples")
        .setIcon(Icon::Plus)
        .setVariant(ButtonVariant::Flat)
        .setSize(Size::Small);
    listScroll_ = &middle.add<ScrollView>();
    listScroll_->setGrow(1.0f);
    listScroll_->content().setAlign(Align::Stretch);
    list_ = &listScroll_->content().add<ListView>();
    list_->setAlign(Align::Stretch);

    auto &side = body.add<Panel>(Direction::Column, Elevation::Inset);
    side.setDepth(Depth::Subtle);
    side.setMinSize({330.0f, 0.0f});
    side.setAlign(Align::Stretch);
    auto &detailScroll = side.add<ScrollView>();
    detailScroll.setGrow(1.0f);
    detailScroll.content().setAlign(Align::Stretch);
    detail_ = &detailScroll.content().add<Container>(Direction::Column);
    detail_->setAlign(Align::Stretch);
    side.add<LiveLabel>([this] { return message_; }, TextStyle::Caption, TextTone::Secondary).setWrapWidth(300.0f);
}

void AssetBrowser::show()
{
    if (!shown_) {
        library_.rescan();
        context_.bringToFront(*this);
    }
    shown_ = true;
}

void AssetBrowser::hide()
{
    shown_ = false;
    if (search_ && search_->focused())
        context_.signals().setFocus(nullptr);
    if (hooks_.stopPreview)
        hooks_.stopPreview();
}

void AssetBrowser::setTab(Tab tab)
{
    if (tab == tab_)
        return;
    tab_ = tab;
    tabs_->setSelected(tab == Tab::Online ? 1 : 0);
    category_.clear();
    selectedKey_.clear();
    filterChanged_ = now_;
}

void AssetBrowser::chooseInstrument(const std::string &key)
{
    setTab(Tab::Library);
    kind_ = Kind::Instruments;
    kinds_->setSelected(1);
    category_.clear();
    show();
    if (!key.empty())
        selectedKey_ = key;
}

std::vector<AssetBrowser::Entry> AssetBrowser::entries() const
{
    std::vector<Entry> out;
    if (tab_ == Tab::Library) {
        if (kind_ != Kind::Samples) {
            // The library's instruments and the instrument plugins, side by side in their categories
            for (const CatalogueEntry &c : hooks_.catalogue ? hooks_.catalogue->entries() : std::vector<CatalogueEntry>{}) {
                if (!c.instrument)
                    continue;
                Entry e;
                e.key = c.key();
                e.title = c.device.name;
                e.category = c.category;
                if (c.plugin()) {
                    e.from = Entry::From::Plugin;
                    e.subtitle = "Plugin  /  " + c.category + "  /  " + c.source;
                    e.device = c;
                } else if (c.device.kind == audio::DeviceKind::Synth) {
                    e.from = Entry::From::Builtin;
                    e.subtitle = "Built-in  /  " + c.category;
                    e.device = c;
                } else {
                    const LibraryInstrument *i = library_.find(c.device.id);
                    if (!i)
                        continue;
                    e.from = Entry::From::Instrument;
                    e.subtitle = "Instrument  /  " + i->category;
                    e.instrument = *i;
                }
                out.push_back(std::move(e));
            }
            {
                std::stable_sort(out.begin(), out.end(), [](const Entry &a, const Entry &b) {
                    std::string x = a.title, y = b.title;
                    for (char &ch : x)
                        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    for (char &ch : y)
                        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    return x < y;
                });
            }
        }
        if (kind_ != Kind::Instruments) {
            for (const LibrarySample &s : library_.samples()) {
                Entry e;
                e.from = Entry::From::Sample;
                e.key = "s:" + s.path;
                e.title = s.name;
                e.subtitle = (s.pack.empty() ? s.category : s.category + "  /  " + s.pack);
                e.category = s.category;
                e.sample = s;
                out.push_back(std::move(e));
            }
        }
    } else {
        for (const OnlineItem &item : online_.listing(source_).items) {
            const bool instrument = item.kind == AssetKind::Instrument;
            if ((kind_ == Kind::Instruments && !instrument) || (kind_ == Kind::Samples && instrument))
                continue;
            Entry e;
            e.from = Entry::From::Online;
            e.key = "o:" + item.source + "/" + item.id;
            e.title = item.name;
            std::string by = item.author.empty() ? std::string() : item.author;
            if (!item.license.empty())
                by += (by.empty() ? "" : "  /  ") + item.license;
            e.subtitle = by.empty() ? std::string(assetKindName(item.kind)) : by;
            e.category = item.category;
            e.item = item;
            out.push_back(std::move(e));
        }
    }
    return out;
}

std::string AssetBrowser::stamp() const
{
    return std::to_string(static_cast<int>(tab_)) + "|" + std::to_string(static_cast<int>(kind_)) + "|" + category_ +
           "|" + filter_ + "|" + std::to_string(source_) + "|" + std::to_string(library_.version()) + "|" +
           std::to_string(online_.version()) + "|" + std::to_string(hooks_.catalogue ? hooks_.catalogue->version() : 0) + "|" +
           selectedKey_;
}

void AssetBrowser::rebuild()
{
    const std::vector<Entry> all = entries();
    rebuildCategories(all);

    // The list: the category and search applied (a searchable source has done the search itself)
    const bool serverSearch = tab_ == Tab::Online && source_ < online_.sources().size() &&
                              online_.sources()[source_].searchable();
    shownEntries_.clear();
    for (const Entry &e : all) {
        if (!category_.empty() && e.category != category_)
            continue;
        if (!serverSearch && !matches(e.title + " " + e.subtitle + " " + e.item.description, filter_))
            continue;
        shownEntries_.push_back(e);
        if (shownEntries_.size() >= kMaxRows)
            break;
    }

    list_->clear();
    for (const Entry &e : shownEntries_) {
        const std::string key = e.key;
        auto &item = list_->add<AssetRow>(e.title, [this, key] {
            // A second click on the same row soon after is a double-click
            const bool twice = lastClickKey_ == key && now_ - lastClickTime_ < 0.4;
            lastClickKey_ = key;
            lastClickTime_ = now_;
            select(key);
            if (twice) {
                if (const Entry *s = selected())
                    activate(*s);
            }
        }, [this, e] { return trailing(e); });
        item.setSubtitle(e.subtitle).setSize(Size::Small).setSelected(key == selectedKey_);
        if (e.from == Entry::From::Instrument || e.from == Entry::From::Plugin || e.from == Entry::From::Builtin) {
            // Library instruments, plugins and built-in instruments drag onto tracks alike
            item.setDrag([this, key] {
                if (hooks_.dragInstrument)
                    hooks_.dragInstrument(key);
            });
        }
        item.setIcon(e.from == Entry::From::Plugin || e.from == Entry::From::Builtin     ? Icon::Sliders
                     : e.from == Entry::From::Sample || e.item.kind == AssetKind::Sample ? Icon::Wave
                     : e.item.kind == AssetKind::Pack && e.from == Entry::From::Online ? Icon::Folder
                                                                                         : Icon::Keys);
    }
    if (shownEntries_.empty()) {
        std::string why;
        if (tab_ == Tab::Online) {
            const auto &l = online_.listing(source_);
            why = l.loading ? "Loading..." : !l.error.empty() ? l.error : "Nothing here matches.";
        } else if (library_.entries().empty() && library_.samples().empty()) {
            why = "The library is empty. Download instruments and samples from the Online tab, or import your own.";
        } else {
            why = "Nothing in the library matches.";
        }
        list_->add<Label>(why, TextStyle::Body, TextTone::Muted).setWrapWidth(520.0f);
        const auto &l = online_.listing(source_);
        if (tab_ == Tab::Online && !l.loading && !l.error.empty() && l.error.find("Settings") != std::string::npos &&
            hooks_.openSettings)
            list_->add<Button>([this] { hooks_.openSettings(); }).setLabel("Open Settings").setSize(Size::Small);
    }
    rebuildDetail();
}

void AssetBrowser::rebuildCategories(const std::vector<Entry> &all)
{
    categories_->clear();
    std::vector<std::string> names = tab_ == Tab::Library ? library_.categories() : defaultCategories();
    std::vector<size_t> counts(names.size(), 0);
    for (const Entry &e : all) {
        auto it = std::find(names.begin(), names.end(), e.category);
        if (it == names.end()) {
            names.push_back(e.category);
            counts.push_back(0);
            it = names.end() - 1;
        }
        ++counts[static_cast<size_t>(it - names.begin())];
    }
    auto add = [this](const std::string &title, const std::string &value, size_t count) {
        auto &item = categories_->add<ListItem>(title, [this, value] {
            category_ = value;
            selectedKey_.clear();
            filterChanged_ = now_;
        });
        item.setTrailing(count ? std::to_string(count) : std::string()).setSize(Size::Small);
        item.setSelected(category_ == value);
        item.setIcon(Icon::Folder);
    };
    add("All", "", all.size());
    for (size_t i = 0; i < names.size(); ++i) {
        // The library shows every category, so there is somewhere to put things; online only
        // the ones it has
        if (counts[i] > 0 || (tab_ == Tab::Library && names[i] != "Other"))
            add(names[i], names[i], counts[i]);
    }
}

const AssetBrowser::Entry *AssetBrowser::selected() const
{
    for (const Entry &e : shownEntries_) {
        if (e.key == selectedKey_)
            return &e;
    }
    return nullptr;
}

void AssetBrowser::select(const std::string &key)
{
    if (key == selectedKey_)
        return;
    selectedKey_ = key;
    message_.clear();
}

void AssetBrowser::moveSelection(int by)
{
    if (shownEntries_.empty())
        return;
    int index = -1;
    for (size_t i = 0; i < shownEntries_.size(); ++i) {
        if (shownEntries_[i].key == selectedKey_)
            index = static_cast<int>(i);
    }
    index = index < 0 ? (by > 0 ? 0 : static_cast<int>(shownEntries_.size()) - 1)
                      : std::clamp(index + by, 0, static_cast<int>(shownEntries_.size()) - 1);
    select(shownEntries_[static_cast<size_t>(index)].key);
    // Keep it in view: rows are about the same height
    if (list_->childCount() > static_cast<size_t>(index)) {
        const Rect r = list_->child(static_cast<size_t>(index)).rect();
        const Rect view = listScroll_->rect();
        if (r.y < view.y)
            listScroll_->scrollTo(listScroll_->offsetY() - (view.y - r.y) - 8.0f);
        else if (r.y + r.h > view.y + view.h)
            listScroll_->scrollTo(listScroll_->offsetY() + (r.y + r.h - view.y - view.h) + 8.0f);
    }
}

std::string AssetBrowser::installedPath(const Entry &e) const
{
    if (e.from != Entry::From::Online)
        return {};
    const Downloads::Job *job = downloads_.find(e.item.source, e.item.id);
    return job && job->state == Downloads::State::Done ? job->installed : std::string();
}

std::string AssetBrowser::trailing(const Entry &e) const
{
    switch (e.from) {
    case Entry::From::Instrument:
    case Entry::From::Plugin:
    case Entry::From::Builtin:
        return hooks_.currentInstrument && hooks_.currentInstrument() == e.key
                   ? "On track"
                   : (e.from == Entry::From::Plugin ? "Plugin" : e.from == Entry::From::Builtin ? "Built-in" : "");
    case Entry::From::Sample:
        return sizeText(e.sample.bytes);
    case Entry::From::Online:
        if (const Downloads::Job *job = downloads_.find(e.item.source, e.item.id)) {
            if (job->active())
                return format("%.0f%%", job->fraction() * 100.0);
            if (job->state == Downloads::State::Done)
                return "Installed";
            if (job->state == Downloads::State::Failed)
                return "Failed";
        }
        return e.item.kind == AssetKind::Sample && e.item.seconds > 0 ? format("%.1f s", e.item.seconds)
                                                                        : sizeText(e.item.bytes);
    }
    return {};
}

void AssetBrowser::activate(const Entry &e)
{
    switch (e.from) {
    case Entry::From::Plugin:
    case Entry::From::Builtin:
    case Entry::From::Instrument:
        if (hooks_.useInstrument) {
            const std::string target = hooks_.instrumentTarget ? hooks_.instrumentTarget() : std::string();
            hooks_.useInstrument(e.key);
            message_ = "\"" + e.title + "\" " + (target.empty() ? "is in use" : "plays " + target) +
                       (e.from == Entry::From::Plugin ? ", in its sandbox." : ".");
        }
        break;
    case Entry::From::Sample:
        if (hooks_.addSound) {
            const std::string target = hooks_.soundTarget ? hooks_.soundTarget() : std::string();
            hooks_.addSound(e.sample.path);
            message_ = "\"" + e.sample.name + "\" added" + (target.empty() ? "" : " " + target) + ".";
        }
        break;
    case Entry::From::Online: {
        const std::string installed = installedPath(e);
        if (installed.empty()) {
            const Downloads::Job *job = downloads_.find(e.item.source, e.item.id);
            if (!job || !job->active()) {
                const OnlineItem item = e.item;
                downloads_.start(item, [this, item](const Downloads::Job &done) {
                    if (done.state == Downloads::State::Done) {
                        message_ = "\"" + item.name + "\" is in your library, under " + item.category + ".";
                        if (hooks_.installed)
                            hooks_.installed(item.kind, done.installed);
                    } else if (done.state == Downloads::State::Failed) {
                        message_ = "\"" + item.name + "\" didn't download: " + done.error;
                    }
                });
                message_ = "Downloading \"" + item.name + "\"...";
            }
        } else if (e.item.kind == AssetKind::Instrument && hooks_.useInstrument) {
            hooks_.useInstrument("sampler:" + fs::u8path(installed).filename().string());
        } else if (e.item.kind == AssetKind::Sample && hooks_.addSound) {
            hooks_.addSound(installed);
        }
        break;
    }
    }
}

void AssetBrowser::preview(const Entry &e)
{
    if (hooks_.previewing && hooks_.previewing()) {
        if (hooks_.stopPreview)
            hooks_.stopPreview();
        return;
    }
    switch (e.from) {
    case Entry::From::Plugin:
        break; // plugins are heard on a track
    case Entry::From::Builtin:
        if (hooks_.previewSynth)
            hooks_.previewSynth(e.device.device);
        break;
    case Entry::From::Instrument:
        if (hooks_.previewInstrument)
            hooks_.previewInstrument(e.instrument.name);
        break;
    case Entry::From::Sample:
        if (hooks_.previewSound)
            hooks_.previewSound(e.sample.path);
        break;
    case Entry::From::Online: {
        const std::string installed = installedPath(e);
        if (!installed.empty() && e.item.kind == AssetKind::Instrument) {
            if (hooks_.previewInstrument)
                hooks_.previewInstrument(fs::u8path(installed).filename().string());
            break;
        }
        message_ = "Fetching the preview...";
        const std::string name = e.item.name;
        online_.preview(e.item, [this, name](const std::string &path, const std::string &error) {
            if (path.empty()) {
                message_ = "No preview: " + error;
                return;
            }
            message_.clear();
            if (hooks_.previewSound)
                hooks_.previewSound(path);
        });
        break;
    }
    }
}

void AssetBrowser::rebuildDetail()
{
    detail_->clear();
    const Entry *found = selected();
    if (!found) {
        detail_->add<Label>(tab_ == Tab::Library ? "Choose an instrument or a sample" : "Choose something to download",
                            TextStyle::Body, TextTone::Muted)
            .setWrapWidth(290.0f);
        detail_->add<Label>(tab_ == Tab::Library
                                ? "Double-click an instrument to play it on the selected track, or a sample to put "
                                  "it on the selected audio track at the playhead."
                                : "Downloads go into your library, sorted by category, with a note of their licence.",
                            TextStyle::Caption, TextTone::Secondary)
            .setWrapWidth(290.0f);
        return;
    }
    const Entry e = *found;
    detail_->add<Label>(e.title, TextStyle::Title).setWrapWidth(290.0f);
    const char *kind = e.from == Entry::From::Instrument ? "Instrument"
                       : e.from == Entry::From::Plugin   ? "Plugin instrument"
                       : e.from == Entry::From::Builtin  ? "Built-in instrument"
                       : e.from == Entry::From::Sample   ? "Sample"
                                                         : assetKindName(e.item.kind);
    detail_->add<Label>(std::string(kind) + "  /  " + e.category, TextStyle::Caption, TextTone::Secondary);

    auto buttons = [this]() -> Container & {
        auto &r = row(*detail_);
        return r;
    };
    // The preview button follows whether something is playing
    auto addPreview = [this, e](Container &parent) {
        auto &button = parent.add<Button>([this, e] { preview(e); });
        button.setLabel("Preview").setIcon(Icon::Play).setSize(Size::Small);
        Button *b = &button;
        parent.add<FrameHook>([this, b](const Input &) {
            const bool playing = hooks_.previewing && hooks_.previewing();
            b->setLabel(playing ? "Stop" : "Preview").setIcon(playing ? Icon::Stop : Icon::Play);
        });
    };
    // Moving between categories, on disk
    auto addCategory = [this, e](const std::string &current) {
        auto &r = row(*detail_);
        r.add<Label>("Category", TextStyle::Body, TextTone::Secondary).setMinSize({76.0f, 0.0f});
        std::vector<std::string> names = library_.categories();
        int index = 0;
        for (size_t i = 0; i < names.size(); ++i) {
            if (names[i] == current)
                index = static_cast<int>(i);
        }
        r.add<Dropdown>(context_, names, index, [this, e, names](int i) {
             const std::string to = names[static_cast<size_t>(i)];
             std::string error;
             const bool ok = e.from == Entry::From::Instrument ? library_.moveInstrument(e.instrument.name, to, error)
                                                               : library_.moveSample(e.sample.path, to, error);
             message_ = ok ? "Moved to " + to + "." : error;
             if (ok && e.from == Entry::From::Sample)
                 selectedKey_.clear(); // its path changed
         })
            .setSize(Size::Small)
            .setGrow(1.0f);
    };

    switch (e.from) {
    case Entry::From::Plugin: {
        const CatalogueEntry &d = e.device;
        detail_->add<Label>("By " + d.source, TextStyle::Caption, TextTone::Secondary);
        if (!d.description.empty())
            detail_->add<Label>(d.description, TextStyle::Caption, TextTone::Muted).setWrapWidth(290.0f);
        auto &actions = buttons();
        actions.add<Button>([this, e] { activate(e); }).setLabel("Use").setVariant(ButtonVariant::Accent).setSize(Size::Small);
        detail_->add<LiveLabel>([this] {
            return hooks_.instrumentTarget ? "Use plays it " + hooks_.instrumentTarget() + "." : std::string();
        }, TextStyle::Caption, TextTone::Secondary).setWrapWidth(290.0f);
        detail_->add<Label>("A CLAP plugin. It runs in a sandbox of its own: no network, and none of your files but its own. "
                            "Its controls and its interface are on the track's Devices tab. Drag it onto a track, or Use.",
                            TextStyle::Caption, TextTone::Muted)
            .setWrapWidth(290.0f);
        const std::string path = d.path;
        detail_->add<Button>([path] { engine::showInFolder(path); })
            .setLabel("Show in folder")
            .setIcon(Icon::Folder)
            .setVariant(ButtonVariant::Flat)
            .setSize(Size::Small)
            .setAlignSelf(Align::Start);
        break;
    }
    case Entry::From::Builtin: {
        const CatalogueEntry &d = e.device;
        detail_->add<Label>("By " + d.source, TextStyle::Caption, TextTone::Secondary);
        if (!d.description.empty())
            detail_->add<Label>(d.description, TextStyle::Caption, TextTone::Muted).setWrapWidth(290.0f);
        auto &actions = buttons();
        addPreview(actions);
        actions.add<Button>([this, e] { activate(e); }).setLabel("Use").setVariant(ButtonVariant::Accent).setSize(Size::Small);
        detail_->add<LiveLabel>([this] {
            return hooks_.instrumentTarget ? "Use plays it " + hooks_.instrumentTarget() + "." : std::string();
        }, TextStyle::Caption, TextTone::Secondary).setWrapWidth(290.0f);
        detail_->add<Label>("Built into Vibin. Its controls are on the track's Devices tab, where its full view opens.",
                            TextStyle::Caption, TextTone::Muted)
            .setWrapWidth(290.0f);
        break;
    }
    case Entry::From::Instrument: {
        detail_->add<LiveLabel>([this, e] {
            if (library_.loading(e.instrument.name))
                return std::string("Loading...");
            if (auto set = library_.cached(e.instrument.name))
                return format("%.0f samples", static_cast<double>(set->zones.size())) +
                       format(",  %.0f MB in memory", set->bytes() / 1e6);
            return std::string("Not loaded yet");
        }, TextStyle::Caption, TextTone::Muted);
        auto &actions = buttons();
        addPreview(actions);
        auto &use = actions.add<Button>([this, e] { activate(e); });
        use.setLabel("Use").setVariant(ButtonVariant::Accent).setSize(Size::Small);
        detail_->add<LiveLabel>([this] {
            return hooks_.instrumentTarget ? "Use plays it " + hooks_.instrumentTarget() + "." : std::string();
        }, TextStyle::Caption, TextTone::Secondary).setWrapWidth(290.0f);
        addCategory(e.instrument.category);
        const std::string folder = e.instrument.folder;
        detail_->add<Button>([folder] { engine::showInFolder(folder); })
            .setLabel("Show in folder")
            .setIcon(Icon::Folder)
            .setVariant(ButtonVariant::Flat)
            .setSize(Size::Small)
            .setAlignSelf(Align::Start);
        break;
    }
    case Entry::From::Sample: {
        if (!e.sample.pack.empty())
            detail_->add<Label>("In " + e.sample.pack, TextStyle::Caption, TextTone::Muted);
        detail_->add<Label>(sizeText(e.sample.bytes) + ",  " + fs::u8path(e.sample.path).extension().string().substr(1),
                            TextStyle::Caption, TextTone::Muted);
        auto &actions = buttons();
        addPreview(actions);
        actions.add<Button>([this, e] { activate(e); })
            .setLabel("Add to track")
            .setVariant(ButtonVariant::Accent)
            .setSize(Size::Small);
        detail_->add<LiveLabel>([this] {
            return hooks_.soundTarget ? "Adds it " + hooks_.soundTarget() + "." : std::string();
        }, TextStyle::Caption, TextTone::Secondary).setWrapWidth(290.0f);
        addCategory(e.sample.category);
        const std::string path = e.sample.path;
        detail_->add<Button>([path] { engine::showInFolder(path); })
            .setLabel("Show in folder")
            .setIcon(Icon::Folder)
            .setVariant(ButtonVariant::Flat)
            .setSize(Size::Small)
            .setAlignSelf(Align::Start);
        break;
    }
    case Entry::From::Online: {
        const OnlineItem &item = e.item;
        if (!item.author.empty())
            detail_->add<Label>("By " + item.author, TextStyle::Caption, TextTone::Secondary);
        if (!item.license.empty())
            detail_->add<Label>("Licence: " + item.license, TextStyle::Caption, TextTone::Secondary).setWrapWidth(290.0f);
        std::string size = item.kind == AssetKind::Sample && item.seconds > 0 ? format("%.1f seconds", item.seconds)
                                                                                : sizeText(item.bytes);
        if (!size.empty())
            detail_->add<Label>((item.kind == AssetKind::Instrument ? "About " : "") + size, TextStyle::Caption,
                                TextTone::Muted);
        if (!item.description.empty())
            detail_->add<Label>(item.description, TextStyle::Caption, TextTone::Secondary).setWrapWidth(290.0f);

        // Download, its progress, then what it can do once installed
        auto &progress = detail_->add<ProgressBar>(0.0f);
        progress.setSize(Size::Small);
        auto &state = detail_->add<Label>("", TextStyle::Caption, TextTone::Secondary);
        state.setWrapWidth(290.0f);
        auto &actions = buttons();
        if (!item.preview.empty() || item.kind == AssetKind::Instrument)
            addPreview(actions);
        auto &main = actions.add<Button>([this, e] { activate(e); });
        main.setVariant(ButtonVariant::Accent).setSize(Size::Small);
        auto &cancel = actions.add<Button>([this, item] { downloads_.cancel(item.source, item.id); });
        cancel.setLabel("Cancel").setVariant(ButtonVariant::Flat).setSize(Size::Small);
        Button *mainButton = &main;
        Button *cancelButton = &cancel;
        ProgressBar *bar = &progress;
        Label *stateLabel = &state;
        Button *previewButton = actions.childCount() > 0 ? dynamic_cast<Button *>(&actions.child(0)) : nullptr;
        if (previewButton == mainButton)
            previewButton = nullptr;
        detail_->add<FrameHook>([this, item, mainButton, cancelButton, bar, stateLabel, previewButton](const Input &) {
            const Downloads::Job *job = downloads_.find(item.source, item.id);
            const bool active = job && job->active();
            const bool done = job && job->state == Downloads::State::Done;
            bar->setValue(job ? job->fraction() : 0.0f);
            std::string text;
            if (active) {
                text = std::string(downloadStateName(job->state)) + format(",  %.0f%%", job->fraction() * 100.0);
                if (job->state == Downloads::State::Downloading && job->progress->done > 0)
                    text += "  (" + sizeText(job->progress->done) + ")";
            } else if (done) {
                text = "In your library: " + job->installed;
            } else if (job && job->state == Downloads::State::Failed) {
                text = "Failed: " + job->error;
            } else if (job && job->state == Downloads::State::Cancelled) {
                text = "Cancelled";
            }
            stateLabel->setText(text);
            mainButton->setEnabled(!active);
            mainButton->setLabel(done ? (item.kind == AssetKind::Instrument ? "Use" : "Add to track")
                                      : item.kind == AssetKind::Instrument ? "Download" : "Download");
            mainButton->setIcon(done ? Icon::Check : Icon::Download);
            cancelButton->setEnabled(active);
            // An instrument is heard only once it is installed
            if (previewButton && item.preview.empty())
                previewButton->setEnabled(done);
        });
        if (!item.page.empty()) {
            const std::string page = item.page;
            detail_->add<Button>([page] { engine::openUrl(page); })
                .setLabel("Open its page")
                .setVariant(ButtonVariant::Flat)
                .setSize(Size::Small)
                .setAlignSelf(Align::Start);
        }
        break;
    }
    }
}

bool AssetBrowser::keyPressed(const KeyEvent &e)
{
    if (!e.down || !shown_)
        return false;
    if (e.mods != 0)
        return false;
    switch (e.key) {
    case Key::Up:
        moveSelection(-1);
        return true;
    case Key::Down:
        moveSelection(1);
        return true;
    case Key::Enter:
        if (const Entry *s = selected())
            activate(*s);
        return true;
    case Key::Escape:
        hide();
        return true;
    default:
        return false;
    }
}

void AssetBrowser::arrange(const Rect &window, const Theme &theme)
{
    // Slid up from the bottom edge, nearly the window's width
    const float margin = 14.0f;
    const float height = std::min(std::max(360.0f, window.h * 0.5f), window.h - 120.0f);
    const float eased = 1.0f - (1.0f - open_) * (1.0f - open_);
    const float top = window.y + window.h - margin - height;
    rect_ = {window.x + margin, top + (1.0f - eased) * (height + margin + 20.0f), window.w - 2.0f * margin, height};
    panel_->arrange(rect_, theme);
}

void AssetBrowser::handleInput(const Input &input)
{
    now_ = input.time;
    open_ += ((shown_ ? 1.0f : 0.0f) - open_) * input.ease(16.0f);
    if (std::abs(open_ - (shown_ ? 1.0f : 0.0f)) < 0.002f)
        open_ = shown_ ? 1.0f : 0.0f;
    if (!shown_)
        return; // put away: no input, no hooks

    if (keys_.attach(input.signals))
        keys_.key(Priority::Component, [this](const KeyEvent &e) { return keyPressed(e); });

    sourcePages_->setCurrent(tab_ == Tab::Online ? 1 : 0);
    // Online: the listing, fetched when first shown; a searchable source asks again once the
    // search has settled
    if (tab_ == Tab::Online && source_ < online_.sources().size()) {
        const SourceConfig &source = online_.sources()[source_];
        if (source.searchable()) {
            const std::string query = freesoundQuery(filter_, category_);
            if (query != asked_ && (asked_.empty() || now_ - filterChanged_ >= kSearchDelay)) {
                asked_ = query;
                online_.fetch(source_, query);
            }
        } else {
            online_.fetch(source_);
        }
    }

    // The lists and the details follow the filters and the data, after the frame
    const std::string s = stamp();
    if (s != built_ && !pending_) {
        pending_ = true;
        context_.post([this, s] {
            rebuild();
            built_ = s;
            pending_ = false;
        });
    }
    panel_->handleInput(input);
}

void AssetBrowser::draw(DrawList &list, const Theme &theme) const
{
    if (open_ <= 0.001f)
        return;
    panel_->draw(list, theme);
}

}
