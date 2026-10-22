#include "daw/app/DawApp.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "daw/io/Autosave.h"
#include "daw/io/ProjectFile.h"
#include "daw/app/Settings.h"
#include "engine/Shell.h"
#include "ui/FileDialog.h"

namespace daw {

using namespace ui;

namespace {

// A row of the Versions dialog's list: a label of fixed width, then its control
Container &labelledRow(Container &parent, const char *label)
{
    auto &r = parent.add<Container>(Direction::Row);
    r.setAlign(Align::Center);
    r.add<Label>(label, TextStyle::Body, TextTone::Secondary).setMinSize({110.0f, 0.0f});
    return r;
}

// The versions a Versions dialog lists, newest first, with the rows made from them
struct VersionRows {
    std::string folder;
    Container *rows = nullptr;              // the scroll view's content
    std::vector<AutosaveVersion> versions;  // newest first
    std::vector<ListItem *> items;          // the rows, in the same order
    size_t selected = 0;
    double updated = -1.0;                  // the manifest's stamp when the rows were made
    size_t count = 0;                       // and how many versions it listed
    int frames = 0;
    bool alive = true;                      // false once the dialog has closed
};

std::string versionSize(const std::string &path)
{
    std::error_code ec;
    const uintmax_t bytes = std::filesystem::file_size(std::filesystem::path(path), ec);
    return ec ? std::string() : ui::files::sizeText(bytes);
}

// Reads the folder's manifest again and rebuilds the rows (after the frame, never during input)
void fillRows(VersionRows &list)
{
    const AutosaveManifest manifest = readManifest(list.folder);
    list.updated = manifest.updated;
    list.count = manifest.versions.size();
    list.versions = manifest.versions;
    std::stable_sort(list.versions.begin(), list.versions.end(),
                     [](const AutosaveVersion &a, const AutosaveVersion &b) { return a.time > b.time; });
    list.items.clear();
    list.rows->clear();
    if (list.selected >= list.versions.size())
        list.selected = 0;
    if (list.versions.empty()) {
        list.rows->add<Label>("No versions yet. Autosaves appear here while a project has unsaved changes, and "
                              "Save version... keeps one under a name.",
                              TextStyle::Caption, TextTone::Muted)
            .setWrapWidth(460.0f);
        return;
    }
    const double now = unixSeconds();
    VersionRows *raw = &list;
    for (size_t i = 0; i < list.versions.size(); ++i) {
        const AutosaveVersion &v = list.versions[i];
        const bool manual = v.kind == "manual";
        ListItem &item = list.rows->add<ListItem>(autosaveTimeText(v.time, now) + (manual ? "  Manual" : "  Auto"),
                                                  [raw, i] {
                                                      raw->selected = i;
                                                      for (size_t j = 0; j < raw->items.size(); ++j)
                                                          raw->items[j]->setSelected(j == i);
                                                  });
        item.setSubtitle(v.label.empty() ? std::string(manual ? "Saved by hand" : "Automatic") : v.label);
        item.setTrailing(versionSize((std::filesystem::path(list.folder) / v.file).string()));
        item.setSelected(i == list.selected);
        list.items.push_back(&item);
    }
}

// The chosen version (the first when none was clicked), or null when there are none
const AutosaveVersion *chosen(const VersionRows &list)
{
    return list.selected < list.versions.size() ? &list.versions[list.selected] : nullptr;
}

}

void DawApp::offerRecovery()
{
    if (!recoverable_.found)
        return;
    // After the first frames, so the window is on screen behind it
    ui_.post([this] {
        if (!recoverable_.found)
            return;
        const std::string name = recoverable_.name.empty() ? std::string("Untitled") : recoverable_.name;
        // Escape or the close button counts as Not now; a button that was pressed already answered
        auto answered = std::make_shared<bool>(false);
        Dialog &dialog = Dialog::open(gui_, "Vibin didn't close properly",
                                      name + " has unsaved work from " +
                                          autosaveTimeText(recoverable_.autosaveTime, unixSeconds()) + ".");
        dialog.setWidth(460.0f);
        dialog.onClose([this, answered] {
            if (!*answered)
                ui_.post([this] { declineRecovery(); });
        });
        dialog.addButton("Recover", [this, answered] {
            *answered = true;
            ui_.post([this] { recoverSession(); });
        }, true);
        dialog.addButton("Show versions...", [this, answered] {
            *answered = true;
            ui_.post([this] { showRecoverableVersions(); });
        });
        dialog.addButton("Not now", [this, answered] {
            *answered = true;
            ui_.post([this] { declineRecovery(); });
        });
        dialog.addButton("Discard", [this, answered] {
            *answered = true;
            ui_.post([this] { discardSession(); });
        });
    });
}

void DawApp::recoverSession()
{
    const Recoverable found = recoverable_;
    recoverable_.found = false;
    Project project;
    std::string error;
    if (!loadProjectFile(found.autosave, project, error)) {
        Dialog::open(gui_, "Couldn't recover the project", error).setWidth(480.0f).addButton("OK", {}, true);
        return;
    }
    // The project is the file's content with the recovered changes: what the file holds (none for a
    // project never saved) is what it differs from, so it reads as modified and Save writes it there
    Project saved;
    if (!found.path.empty() && std::filesystem::exists(found.path)) {
        std::string ignored;
        loadProjectFile(found.path, saved, ignored);
    }
    removeSessionLockFile(autosave_->root(), found.pid);
    replaceProject(std::move(project), found.path, &saved);
    reportMissingInstruments();
    notify("Recovered the work from " + autosaveTimeText(found.autosaveTime, unixSeconds()));
}

void DawApp::discardSession()
{
    const Recoverable found = recoverable_;
    recoverable_.found = false;
    removeSessionLockFile(autosave_->root(), found.pid);
    // An unsaved session's autosaves go with it; a saved project's stay, since they are its versions
    if (found.path.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path(autosave_->root()) / found.key, ec);
    }
}

void DawApp::declineRecovery()
{
    const Recoverable found = recoverable_;
    recoverable_.found = false;
    // The files stay; the session is offered again only once a newer autosave exists
    std::string error;
    writeDeclined((std::filesystem::path(autosave_->root()) / found.key).string(), found.autosaveTime, error);
    removeSessionLockFile(autosave_->root(), found.pid);
}

void DawApp::showRecoverableVersions()
{
    const Recoverable found = recoverable_;
    recoverable_.found = false;
    removeSessionLockFile(autosave_->root(), found.pid);
    openVersionsFor((std::filesystem::path(autosave_->root()) / found.key).string(), found.path,
                    found.name.empty() ? std::string("Untitled") : found.name);
}

void DawApp::openVersions()
{
    openVersionsFor(autosave_->folderFor(session_), session_.path(), project().name);
}

void DawApp::openVersionsFor(const std::string &folder, const std::string &path, const std::string &name)
{
    auto list = std::make_shared<VersionRows>();
    list->folder = folder;
    UISystem *ui = &ui_;

    Dialog &dialog = Dialog::open(gui_, "Versions",
                                  "Autosaves and saved versions of \"" + name + "\", newest first.");
    dialog.setWidth(560.0f);
    dialog.onClose([list] { list->alive = false; });
    ScrollView &scroll = dialog.body().add<ScrollView>();
    scroll.setViewHeight(240.0f);
    scroll.content().setAlign(Align::Stretch);
    list->rows = &scroll.content();
    fillRows(*list);
    // Autosaves written while the dialog is open show up in the list
    dialog.body().add<FrameHook>([ui, list](const Input &) {
        if (!list->alive || ++list->frames % 30 != 0)
            return;
        const AutosaveManifest now = readManifest(list->folder);
        if (now.updated != list->updated || now.versions.size() != list->count)
            ui->post([list] {
                if (list->alive)
                    fillRows(*list);
            });
    });

    dialog.addButton("Open as copy", [this, list, folder, name] {
        const AutosaveVersion *v = chosen(*list);
        if (!v) {
            notify("There is no version to open");
            return;
        }
        const AutosaveVersion version = *v;
        ui_.post([this, folder, version, name] {
            confirmDiscard([this, folder, version, name] { openVersionCopy(folder, version, name); });
        });
    }, true);
    dialog.addButton("Restore", [this, list, folder, path] {
        const AutosaveVersion *v = chosen(*list);
        if (!v) {
            notify("There is no version to restore");
            return;
        }
        const AutosaveVersion version = *v;
        ui_.post([this, folder, version, path] {
            Dialog &confirm = Dialog::open(gui_, "Restore this version?",
                                           "Replace the current project with the version from " +
                                               autosaveTimeText(version.time, unixSeconds()) +
                                               "? You can't undo this, but the current state is kept as a version.");
            confirm.setWidth(460.0f);
            confirm.addButton("Cancel");
            confirm.addButton("Restore", [this, folder, version, path] {
                ui_.post([this, folder, version, path] { restoreVersion(folder, version, path); });
            }, true);
        });
    });
    dialog.addButton("Delete", [this, list, folder, path, name] {
        const AutosaveVersion *v = chosen(*list);
        if (!v) {
            notify("There is no version to delete");
            return;
        }
        const AutosaveVersion version = *v;
        ui_.post([this, folder, version, path, name] { confirmDeleteVersion(folder, version, path, name); });
    });
    dialog.addButton("Save version...", [this] { ui_.post([this] { openSaveVersion(); }); });
    dialog.addButton("Close");
}

void DawApp::openVersionCopy(const std::string &folder, const AutosaveVersion &version, const std::string &name)
{
    Project project;
    std::string error;
    if (!loadProjectFile((std::filesystem::path(folder) / version.file).string(), project, error)) {
        Dialog::open(gui_, "Couldn't open the version", error).setWidth(480.0f).addButton("OK", {}, true);
        return;
    }
    // An unsaved copy: it has no file, so it reads as modified until it is saved
    project.name = name + " (version " + autosaveTimeText(version.time, unixSeconds()) + ")";
    replaceProject(std::move(project), {});
    reportMissingInstruments();
}

void DawApp::restoreVersion(const std::string &folder, const AutosaveVersion &version, const std::string &path)
{
    // What is there now stays as a version of its own first
    session_.store().normalizeIds();
    autosave_->saveVersion(session_, "Before restore");
    Project project;
    std::string error;
    if (!loadProjectFile((std::filesystem::path(folder) / version.file).string(), project, error)) {
        Dialog::open(gui_, "Couldn't restore the version", error).setWidth(480.0f).addButton("OK", {}, true);
        return;
    }
    // Like a recovery: the file it differs from is kept as what is saved, so the project reads as modified
    Project saved;
    if (!path.empty() && std::filesystem::exists(path)) {
        std::string ignored;
        loadProjectFile(path, saved, ignored);
    }
    replaceProject(std::move(project), path, &saved);
    reportMissingInstruments();
    notify("Restored the version from " + autosaveTimeText(version.time, unixSeconds()));
}

void DawApp::confirmDeleteVersion(const std::string &folder, const AutosaveVersion &version, const std::string &path,
                                  const std::string &name)
{
    Dialog &dialog = Dialog::open(gui_, "Delete this version?",
                                  "The " + std::string(version.kind == "manual" ? "saved" : "automatic") +
                                      " version from " + autosaveTimeText(version.time, unixSeconds()) +
                                      " is deleted for good.");
    dialog.setWidth(440.0f);
    dialog.addButton("Cancel");
    dialog.addButton("Delete", [this, folder, version, path, name] {
        if (autosave_->busy()) {
            notify("Still saving, try again in a moment");
            return;
        }
        std::string error;
        if (!deleteVersion(folder, version.file, error))
            notify(error);
        else
            notify("Version deleted");
        // The list is shown again without it
        ui_.post([this, folder, path, name] { openVersionsFor(folder, path, name); });
    }, true);
}

void DawApp::openSaveVersion()
{
    Dialog &dialog = Dialog::open(gui_, "Save version",
                                  "Keeps the project as it is now under a name, so it stays when autosaves are "
                                  "rotated. Versions are listed under Project > Versions.");
    dialog.setWidth(460.0f);
    TextField &label = dialog.body().add<TextField>("Name, e.g. Before the bridge");
    TextField *field = &label;
    auto save = [this, field] {
        session_.store().normalizeIds();
        autosave_->saveVersion(session_, field->text().empty() ? std::string("Saved version") : field->text());
    };
    // Enter saves too
    label.onSubmit([save, &dialog](const std::string &) {
        save();
        dialog.close();
    });
    dialog.addButton("Cancel");
    dialog.addButton("Save", save, true);
}

void DawApp::versionsDemo()
{
    autosave_->saveVersion(session_, "First idea");
    autosave_->saveVersion(session_, "With bass");
    openVersions();
}

void DawApp::buildSavingSettings(Container &body)
{
    body.add<Label>("Saving", TextStyle::Subtitle);
    body.add<Label>("While a project has unsaved changes, Vibin keeps a copy of it, so a crash loses little. "
                    "Autosaves and the versions you save by hand are listed under Project > Versions.",
                    TextStyle::Caption, TextTone::Secondary)
        .setWrapWidth(500.0f);
    body.add<Checkbox>("Autosave unsaved changes", settings_.autosave, [this](bool on) {
        settings_.autosave = on;
        applyAutosaveSettings();
    }).setSize(Size::Small);

    labelledRow(body, "Every")
        .add<Stepper>(static_cast<float>(settings_.autosaveMinutes), 1.0f, 30.0f, 1.0f, [this](float v) {
            settings_.autosaveMinutes = static_cast<int>(v);
            applyAutosaveSettings();
        })
        .setSuffix(" minutes")
        .setSize(Size::Small);
    labelledRow(body, "Keep")
        .add<Stepper>(static_cast<float>(settings_.autosaveVersions), 1.0f, 50.0f, 1.0f, [this](float v) {
            settings_.autosaveVersions = static_cast<int>(v);
            applyAutosaveSettings();
        })
        .setSuffix(" automatic versions")
        .setSize(Size::Small);

    body.add<Label>("Kept in " + autosave_->root(), TextStyle::Caption, TextTone::Muted).setWrapWidth(500.0f);
    body.add<Button>([this] {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(autosave_->root()), ec);
        engine::showInFolder(autosave_->root());
    }).setLabel("Open autosave folder").setVariant(ButtonVariant::Flat).setSize(Size::Small);
}

}
