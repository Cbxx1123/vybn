// The DawApp's plugin settings: Settings > Plugins (the folders scanned, the sandbox, importing a
// plugin, CLAP or VST3, and each plugin: turned on or off, reloaded, the folders it may read), and keeping the
// PluginManager in step with them without a restart.

#include "daw/app/DawApp.h"

#include <algorithm>
#include <filesystem>
#include <system_error>

#include "daw/audio/AutomationWriter.h"
#include "daw/views/DeviceControls.h"
#include "daw/views/DeviceView.h"
#include "daw/widgets/Format.h"
#include "daw/views/TransportBar.h"
#include "daw/views/PluginEditor.h"
#include "engine/Shell.h"

namespace daw {

using namespace ui;
namespace fs = std::filesystem;

namespace {

ui::Container &row(ui::Container &parent)
{
    auto &r = parent.add<Container>(Direction::Row);
    r.setAlign(Align::Center);
    return r;
}

bool contains(const std::vector<std::string> &list, const std::string &item)
{
    return std::find(list.begin(), list.end(), item) != list.end();
}

}

void DawApp::applyPluginSettings()
{
    std::string error;
    settings_.save(error);
    if (!plugins_)
        return;
    // The folders given on the command line for this run stay
    plugin::PluginConfig config = settings_.pluginConfig();
    for (const std::string &extra : extraPluginFolders_)
        if (!contains(config.folders, extra))
            config.folders.push_back(extra);
    plugins_->setConfig(config);
}

// The key of a view's place: the master by its index, a track by its id (ids are never reused)
static std::string viewKey(const DevicePlace &place, ObjectId track)
{
    return place.master ? "M:" + std::to_string(place.index) : "T" + std::to_string(track) + ":" + std::to_string(place.index);
}

DevicePlace DawApp::placeAt(const DevicePlace &place, ObjectId track) const
{
    DevicePlace found = place;
    if (!place.master)
        found.track = project().trackIndex(track); // -1 once the track is gone
    return found;
}

void DawApp::openDevice(const DevicePlace &place, bool interface)
{
    const audio::DeviceSetting *setting = project().device(place);
    if (!setting)
        return;
    const std::string key = setting->key();
    const Track *track = project().track(place.track);
    const ObjectId trackId = track ? track->id : kNoId;
    const std::string where = viewKey(place, trackId);
    if (auto it = deviceViews_.find(where); it != deviceViews_.end() && it->second.key == key && !it->second.closing) {
        dock_->show(*it->second.panel);
        if (interface)
            it->second.view->showInterface(true);
        return;
    }
    const std::string title = setting->name.empty() ? setting->id : setting->name;
    std::string caption = deviceKindText(*setting, place.instrument());
    if (const CatalogueEntry *e = catalogue_->find(DeviceCatalogue::keyOf(*setting)); e && setting->kind == audio::DeviceKind::Plugin)
        caption += "  /  " + e->source;
    caption += "  /  " + (place.master ? std::string("Master") : track ? track->name : std::string());
    // A synth's view is bigger: its panel lays out every section
    const bool synth = setting->kind == audio::DeviceKind::Synth;
    gui_.post([this, place, trackId, key, where, title, caption, interface, synth] {
        // Found again each time: the same place (its track by id), still holding the same device
        DeviceAccess access;
        access.setting = [this, place, trackId, key]() -> const audio::DeviceSetting * {
            const audio::DeviceSetting *d = session_.store().project().device(placeAt(place, trackId));
            return d && d->key() == key ? d : nullptr;
        };
        access.edit = [this, place, trackId, key]() -> audio::DeviceSetting * {
            const DevicePlace at = placeAt(place, trackId);
            const audio::DeviceSetting *d = session_.store().project().device(at);
            return d && d->key() == key ? session_.store().editDevice(at) : nullptr;
        };
        access.device = [this, place, trackId, key]() -> audio::Device * {
            const DevicePlace at = placeAt(place, trackId);
            const audio::DeviceSetting *d = project().device(at);
            return d && d->key() == key ? bridge_->deviceAt(project(), at) : nullptr;
        };
        access.playhead = [this] { return transportBeats(audio_); };
        access.revision = [this] { return session_.store().revision(); };
        access.sidechain = [this, place, trackId, key]() -> SidechainOffer {
            const DevicePlace at = placeAt(place, trackId);
            const audio::DeviceSetting *d = project().device(at);
            return d && d->key() == key ? sidechainOffer(project(), at) : SidechainOffer{};
        };
        access.write = [this, place, trackId, key](const audio::DeviceParam &param, double value) {
            const DevicePlace at = placeAt(place, trackId);
            const audio::DeviceSetting *d = project().device(at);
            return d && d->key() == key &&
                   automationWriter_->touch(at.track, AutomationPlace{false, at}, param.id, automationCurve(param), value);
        };
        // A panel of the dock grid of its own, floating in a native window to begin with (it can be
        // docked anywhere in the grid, and closed from its tab or its window)
        DockPanel &panel = dock_->addPanel(title);
        DockPanel *p = &panel;
        panel.setClosable([this, where, access] {
            deviceViews_.erase(where);
            if (audio::Device *d = access.device())
                if (plugin::Slot *s = d->slot()) {
                    s->placeEditor(0, 0, false);
                    s->closeEditor();
                }
        });
        auto &view = panel.add<DeviceView>(gui_, access, title, caption, !place.instrument(), synth);
        view.setGrow(1.0f);
        DeviceView *raw = &view;
        deviceViews_[where] = {p, raw, key, place, trackId, false};
        if (interface)
            view.showInterface(true);
        // While it floats on the plugin's interface, its window takes the interface's size (as far
        // as the screen allows)
        auto fitted = std::make_shared<std::pair<uint32_t, uint32_t>>(0, 0);
        panel.add<FrameHook>([this, p, raw, fitted](const Input &) {
            PluginEditorView *editor = raw->editor();
            if (!editor || !raw->showingInterface())
                return;
            const uint32_t w = editor->pictureWidth(), h = editor->pictureHeight();
            Rect frame;
            if (w == 0 || (fitted->first == w && fitted->second == h) || !dock_->floatingFrame(*p, frame))
                return;
            *fitted = {w, h};
            const Vec2 need = editor->preferredSize(gui_.theme());
            const Rect have = editor->rect();
            const float width = std::clamp(frame.w + need.x - have.w, 240.0f, 2400.0f);
            const float height = std::clamp(frame.h + need.y - have.h, 160.0f, 1500.0f);
            gui_.post([this, p, width, height] { dock_->resizeFloating(*p, width, height); });
        });
        const Rect bounds = gui_.bounds();
        const float width = synth ? 1180.0f : 800.0f, height = synth ? 860.0f : 560.0f;
        const Vec2 corner = gui_.toScreen({bounds.x + bounds.w * 0.5f - width * 0.5f, bounds.y + 100.0f});
        dock_->float_(panel, {corner.x, corner.y, width, height});
    });
}

void DawApp::pluginEditorDemo()
{
    auto tick = std::make_shared<std::function<void()>>();
    auto stage = std::make_shared<int>(0);
    *tick = [this, tick, stage] {
        if (!plugins_)
            return;
        const int lead = std::min<int>(2, static_cast<int>(project().tracks.size()) - 1);
        if (*stage == 0) {
            if (plugins_->scanning() || plugins_->version() == 0) {
                gui_.post(*tick);
                return;
            }
            // The first instrument plugin installed, the project's own test plugins last
            const CatalogueEntry *chosen = nullptr;
            for (const CatalogueEntry &e : catalogue_->entries())
                if (e.instrument && e.plugin() && (!chosen || chosen->device.id.rfind("vybn.test.", 0) == 0))
                    chosen = &e;
            if (!chosen || lead < 0)
                return;
            setInstrument(lead, chosen->device);
            showEffects();
            *stage = 1;
            gui_.post(*tick);
            return;
        }
        // Its device exists from the next sync: open it on its interface
        if (!bridge_->deviceAt(project(), DevicePlace::instrumentOf(lead))) {
            gui_.post(*tick);
            return;
        }
        openDevice(DevicePlace::instrumentOf(lead), true);
    };
    gui_.post(*tick);
}

void DawApp::reloadPlugin(const std::string &id)
{
    for (plugin::Slot *slot : bridge_->pluginSlots())
        if (id.empty() || slot->id() == id)
            slot->reload();
}

void DawApp::importPlugin()
{
    ui::FileDialogOptions options;
    options.title = "Import plugins";
    options.multiple = true;
    options.filters = {{"Plugins", "*.clap;*.vst3"}, {"All files", "*.*"}};
    options.remember = "plugins";
    options.action = "Import";
    chooseFiles(options, [this](std::vector<std::string> paths) {
        const fs::path folder = fs::u8path(Settings::pluginsPath());
        std::error_code ec;
        fs::create_directories(folder, ec);
        std::vector<std::string> copied, failed;
        for (const std::string &path : paths) {
            fs::path from = fs::u8path(path);
            // A VST3 bundle's module, picked inside it: the whole bundle is copied
            const fs::path bundle = from.parent_path().parent_path().parent_path();
            if (plugin::isVst3Path(from.u8string()) && plugin::isVst3Path(bundle.u8string()) &&
                fs::is_directory(bundle, ec) && from.parent_path().parent_path().filename() == "Contents")
                from = bundle;
            const fs::path to = folder / from.filename();
            // A plugin in use keeps its file open in its host: copying over it fails, so say so
            if (fs::is_directory(from, ec))
                fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
            else
                fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
            if (ec)
                failed.push_back(from.filename().u8string() + " (" + ec.message() + ")");
            else
                copied.push_back(from.filename().u8string());
        }
        if (plugins_)
            plugins_->rescan();
        std::string message = copied.empty() ? std::string()
                                             : format("%.0f plugin file", static_cast<double>(copied.size())) +
                                                   (copied.size() == 1 ? "" : "s") + " copied into " + folder.u8string() +
                                                   ". Scanning them now; they can be used as soon as they are listed.";
        if (!failed.empty()) {
            message += (message.empty() ? "" : "\n\n") + std::string("Couldn't copy: ");
            for (size_t i = 0; i < failed.size(); ++i)
                message += (i ? ", " : "") + failed[i];
            message += ". Close projects using them, or reload them after the copy.";
        }
        Dialog::open(gui_, copied.empty() ? "Nothing was imported" : "Plugins imported", message).setWidth(520.0f).addButton("OK", {}, true);
    });
}

void DawApp::buildPluginSettings(Container &body)
{
    body.add<Label>("Each plugin runs in a sandboxed process of its own: it has no network and can use only its own file "
                    "and the folders you give it (one that keeps presets or a database in your documents needs that folder, to "
                    "write in). One that crashes or stops responding is silenced and can be reloaded; "
                    "the rest of the app carries on.",
                    TextStyle::Caption, TextTone::Secondary)
        .setWrapWidth(560.0f);

    auto &sandbox = row(body);
    sandbox.add<Label>("Sandbox plugins", TextStyle::Body, TextTone::Secondary).setMinSize({180.0f, 0.0f});
    sandbox.add<Toggle>(settings_.pluginSandbox, [this](bool on) {
        settings_.pluginSandbox = on;
        applyPluginSettings();
    });
    body.add<Label>("Turning it off lets plugins reach your files and the network. Plugins already playing keep how they "
                    "were started until they are reloaded.",
                    TextStyle::Caption, TextTone::Muted)
        .setWrapWidth(560.0f);

    // Importing, and scanning
    auto &actions = row(body);
    actions.add<Button>([this] { importPlugin(); }).setLabel("Import plugin...").setIcon(Icon::Plus).setSize(Size::Small);
    actions.add<Button>([this] {
               if (plugins_)
                   plugins_->rescan(true); // every file loaded again, the cache aside
           })
        .setLabel("Scan again")
        .setSize(Size::Small);
    actions.add<Button>([this] { reloadPlugin({}); }).setLabel("Reload all").setSize(Size::Small);
    actions.add<Spacer>();
    actions.add<LiveLabel>([this] {
        if (!plugins_)
            return std::string("Plugins aren't available");
        if (plugins_->scanning())
            return std::string("Scanning...");
        const size_t n = plugins_->plugins().size();
        const plugin::ScanStats scan = plugins_->lastScan();
        return format("%.0f plugin", static_cast<double>(n)) + (n == 1 ? "" : "s") + " found" +
               (scan.cached > 0 ? format(",  %.0f file", static_cast<double>(scan.cached)) + (scan.cached == 1 ? "" : "s") + " unchanged"
                                : std::string());
    }, TextStyle::Caption, TextTone::Muted);

    // The folders
    body.add<Label>("Folders", TextStyle::Subtitle);
    Container &folders = body.add<Container>(Direction::Column);
    folders.setAlign(Align::Stretch);
    Container *foldersPtr = &folders;
    auto buildFolders = std::make_shared<std::function<void()>>();
    *buildFolders = [this, foldersPtr, buildFolders] {
        foldersPtr->clear();
        auto line = [&](const std::string &path, const std::string &note, bool removable) {
            auto &r = row(*foldersPtr);
            std::error_code ec;
            const bool there = fs::is_directory(fs::u8path(path), ec);
            r.add<Label>(path, TextStyle::Body, there ? TextTone::Primary : TextTone::Muted).setMaxWidth(430.0f);
            r.add<Spacer>();
            r.add<Label>(there ? note : "not there yet", TextStyle::Caption, TextTone::Muted);
            if (removable) {
                r.add<Button>([this, path, buildFolders] {
                     auto &list = settings_.pluginFolders;
                     list.erase(std::remove(list.begin(), list.end(), path), list.end());
                     applyPluginSettings();
                     if (plugins_)
                         plugins_->rescan();
                     gui_.post([buildFolders] { (*buildFolders)(); });
                 })
                    .setIcon(Icon::Close)
                    .setVariant(ButtonVariant::Flat)
                    .setShape(Shape::Circle)
                    .setSize(Size::Small);
            }
        };
        for (const std::string &f : plugin::PluginManager::standardFolders())
            line(f, "standard folder", false);
        line(Settings::pluginsPath(), "imported plugins", false);
        for (const std::string &f : settings_.pluginFolders)
            line(f, "added", true);
        for (const std::string &f : extraPluginFolders_)
            line(f, "this run only", false);
        auto &add = row(*foldersPtr);
        auto &field = add.add<TextField>("A folder to scan, such as D:\\Plugins");
        field.setGrow(1.0f);
        TextField *fieldPtr = &field;
        auto addFolder = [this, fieldPtr, buildFolders](const std::string &typed) {
            std::error_code ec;
            const fs::path p = fs::u8path(typed);
            if (typed.empty() || !fs::is_directory(p, ec)) {
                notify("\"" + typed + "\" isn't a folder");
                return;
            }
            const std::string path = fs::absolute(p, ec).lexically_normal().u8string();
            if (!contains(settings_.pluginFolders, path))
                settings_.pluginFolders.push_back(path);
            applyPluginSettings();
            if (plugins_)
                plugins_->rescan();
            fieldPtr->setText({});
            gui_.post([buildFolders] { (*buildFolders)(); });
        };
        field.onSubmit(addFolder);
        add.add<Button>([fieldPtr, addFolder] { addFolder(fieldPtr->text()); }).setLabel("Add folder").setSize(Size::Small);
    };
    (*buildFolders)();

    // The plugins found, rebuilt when a scan finishes or a setting changes
    body.add<Label>("Installed", TextStyle::Subtitle);
    Container &list = body.add<Container>(Direction::Column);
    list.setAlign(Align::Stretch);
    Container *listPtr = &list;
    auto built = std::make_shared<uint64_t>(~0ull);
    auto buildList = std::make_shared<std::function<void()>>();
    *buildList = [this, listPtr, buildList] {
        listPtr->clear();
        if (!plugins_)
            return;
        const std::vector<plugin::Descriptor> found = plugins_->plugins();
        if (found.empty() && !plugins_->scanning())
            listPtr->add<Label>("No plugins in these folders yet. Import one, or add the folder you keep them in.",
                                TextStyle::Body, TextTone::Muted)
                .setWrapWidth(560.0f);
        for (const plugin::Descriptor &d : found) {
            auto &card = listPtr->add<Panel>(Direction::Column);
            card.setAlign(Align::Stretch);
            card.setDepth(Depth::Subtle);
            auto &head = row(card);
            auto &names = head.add<Container>(Direction::Column);
            names.add<Label>(d.name, TextStyle::Body);
            std::string about = d.instrument() ? "Instrument" : "Effect";
            if (!d.vendor.empty())
                about += "  /  " + d.vendor;
            if (!d.version.empty())
                about += "  /  " + d.version;
            about += d.format() == "vst3" ? "  /  VST3" : "  /  CLAP"; // the one place the format shows
            names.add<Label>(about, TextStyle::Caption, TextTone::Secondary);
            head.add<Spacer>();
            const std::string id = d.id;
            head.add<LiveLabel>([this, id] {
                size_t inUse = 0;
                std::string trouble;
                for (plugin::Slot *slot : bridge_->pluginSlots()) {
                    if (slot->id() != id)
                        continue;
                    ++inUse;
                    if (slot->state() == plugin::Slot::State::Crashed || slot->state() == plugin::Slot::State::Hung)
                        trouble = slot->message();
                }
                if (!trouble.empty())
                    return trouble;
                return inUse == 0 ? std::string("Not in use") : format("In use %.0f", static_cast<double>(inUse)) + (inUse == 1 ? " time" : " times");
            }, TextStyle::Caption, TextTone::Muted);
            head.add<SmallToggle>("On", [this, id] { return !contains(settings_.pluginsDisabled, id); },
                                  [this, id](bool on) {
                                      auto &off = settings_.pluginsDisabled;
                                      off.erase(std::remove(off.begin(), off.end(), id), off.end());
                                      if (!on)
                                          off.push_back(id);
                                      applyPluginSettings();
                                      reloadPlugin(id); // off: its places fall silent (effects pass the sound)
                                  });
            head.add<Button>([this, id] { reloadPlugin(id); }).setLabel("Reload").setSize(Size::Small);

            // The folders this plugin may read (its sample content, say)
            const std::string grantPrefix = id + "|";
            for (const std::string &grant : settings_.pluginGrants) {
                if (grant.compare(0, grantPrefix.size(), grantPrefix) != 0)
                    continue;
                auto &g = row(card);
                const std::string folder = grant.substr(grantPrefix.size());
                const bool writes = !folder.empty() && folder[0] == '*';
                g.add<Label>((writes ? "May read and write in " + folder.substr(1) : "May read " + folder), TextStyle::Caption,
                             TextTone::Secondary)
                    .setMaxWidth(470.0f);
                g.add<Spacer>();
                g.add<Button>([this, grant, buildList] {
                     auto &grants = settings_.pluginGrants;
                     grants.erase(std::remove(grants.begin(), grants.end(), grant), grants.end());
                     applyPluginSettings();
                     gui_.post([buildList] { (*buildList)(); });
                 })
                    .setIcon(Icon::Close)
                    .setVariant(ButtonVariant::Flat)
                    .setShape(Shape::Circle)
                    .setSize(Size::Small);
            }
            auto &foot = row(card);
            foot.add<Label>(d.path, TextStyle::Caption, TextTone::Muted).setMaxWidth(300.0f);
            foot.add<Spacer>();
            auto &grantField = foot.add<TextField>("A folder it may use");
            grantField.setMinSize({170.0f, 0.0f});
            TextField *grantPtr = &grantField;
            auto grant = [this, id, grantPtr, buildList](const std::string &typed, bool write) {
                std::error_code ec;
                if (typed.empty() || !fs::is_directory(fs::u8path(typed), ec)) {
                    notify("\"" + typed + "\" isn't a folder");
                    return;
                }
                const std::string folder = fs::absolute(fs::u8path(typed), ec).lexically_normal().u8string();
                // One entry a folder: writing replaces reading
                auto &grants = settings_.pluginGrants;
                grants.erase(std::remove(grants.begin(), grants.end(), id + "|" + folder), grants.end());
                grants.erase(std::remove(grants.begin(), grants.end(), id + "|*" + folder), grants.end());
                grants.push_back(id + "|" + (write ? "*" : "") + folder);
                applyPluginSettings();
                reloadPlugin(id); // a new host, allowed in
                grantPtr->setText({});
                gui_.post([buildList] { (*buildList)(); });
            };
            grantField.onSubmit([grant](const std::string &typed) { grant(typed, false); });
            foot.add<Button>([grantPtr, grant] { grant(grantPtr->text(), false); }).setLabel("Allow reading").setSize(Size::Small);
            foot.add<Button>([grantPtr, grant] { grant(grantPtr->text(), true); }).setLabel("Allow writing").setSize(Size::Small);
            const std::string path = d.path;
            foot.add<Button>([path] { engine::showInFolder(path); })
                .setIcon(Icon::Folder)
                .setVariant(ButtonVariant::Flat)
                .setShape(Shape::Circle)
                .setSize(Size::Small);
        }
        for (const plugin::ScanProblem &p : plugins_->problems()) {
            auto &r = listPtr->add<Container>(Direction::Column);
            r.add<Label>(fs::u8path(p.path).filename().u8string() + " couldn't be read", TextStyle::Body, TextTone::Secondary);
            r.add<Label>(p.error, TextStyle::Caption, TextTone::Muted).setWrapWidth(560.0f);
        }
    };
    // Rebuilt after the frame whenever a scan finishes
    body.add<FrameHook>([this, built, buildList](const Input &) {
        const uint64_t version = plugins_ ? plugins_->version() * 2 + (plugins_->scanning() ? 1 : 0) : 0;
        if (version == *built)
            return;
        *built = version;
        gui_.post([buildList] { (*buildList)(); });
    });
}

}
