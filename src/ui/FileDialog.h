#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Context.h"
#include "Menus.h"
#include "engine/FileDialog.h"

namespace env {
class Environment;
}

namespace ui {

// The file dialog's plain logic, kept apart so it can be checked without a window
namespace files {

// One step of a path's breadcrumbs: "C:\" then "Users" then "calum", each with the path up to it
struct Crumb {
    std::string label;
    std::string path;
};
// The path as the dialog shows it: backslashes, no trailing separator except on a root ("C:\"),
// "C:" made "C:\"
std::string normalize(const std::string &path);
std::vector<Crumb> breadcrumbs(const std::string &path);
// The folder above, or empty at a root
std::string parent(const std::string &path);
std::string join(const std::string &folder, const std::string &name);
// Whether the path is absolute ("C:\...", "\\server\share", "/...")
bool isAbsolute(const std::string &path);
// Paths compared the way Windows does: case and separators don't matter
bool samePath(const std::string &a, const std::string &b);

// "*.sfz;*.WAV" becomes {".sfz", ".wav"}; "*.*" or "*" (or nothing) is every file, an empty list
std::vector<std::string> extensions(const std::string &patterns);
// Whether a file name has one of the extensions (any case); everything passes an empty list
bool matches(const std::string &name, const std::vector<std::string> &extensions);
// The name with `extension` (".vbp") added unless it already ends in it (any case)
std::string withExtension(const std::string &name, const std::string &extension);
// Characters Windows refuses in a file name ( <>:"/\|?* and control characters) become '_'
std::string cleanFileName(const std::string &name);
// Case-insensitive, with runs of digits compared as numbers: "Take 2" before "Take 10"
bool naturalLess(const std::string &a, const std::string &b);

enum class SortBy { Name, Date, Size };
// Folders first, then by the column; ties by name
void sort(std::vector<engine::FolderEntry> &entries, SortBy by, bool descending);
// Folders always, files with one of the extensions; then both by the search (any case, anywhere
// in the name)
std::vector<engine::FolderEntry> filter(const std::vector<engine::FolderEntry> &entries,
                                        const std::vector<std::string> &extensions, const std::string &search);
std::string sizeText(uint64_t bytes);                // "820 bytes", "12 KB", "3.4 MB"
std::string dateText(int64_t seconds, int64_t now); // "Today, 14:32", "12 Mar, 09:05", "12 Mar 2024"

}

// A place in the file dialog's sidebar: the caller's own (the project's folder, the sample
// library), before the user's folders and the drives
struct FilePlace {
    std::string label;
    std::string path;
    Icon icon = Icon::Folder;
};

struct FileDialogOptions {
    std::string title;
    bool save = false;     // a save dialog: a file name field, and a prompt before replacing
    bool multiple = false; // open: Ctrl and Shift clicks pick several files
    std::vector<engine::FileFilter> filters; // the type dropdown; the first is used at first
    std::string folder;    // where it starts, unless `remember` has a folder from earlier
    std::string remember;  // a key ("project", "audio"): the last folder used is kept per key for the session
    std::string name;      // save: the name filled in
    std::string extension; // save: added when the typed name doesn't end in it (".vbp")
    std::string action;    // the button's label ("Open", "Save" by default)
    std::vector<FilePlace> places;
    // Folders are read on its Interactive lane; without one, on a thread of their own
    env::Environment *environment = nullptr;
};

class PathBar;
class FileList;

// The app's own file dialog: a modal overlay in the window, so it opens at once (no native
// window to make) and looks like the rest of the app. A path bar of breadcrumbs (click one to go
// there; click the bar's empty end or press Ctrl+L to type a path), places on the left, the
// folder's subfolders then files (filtered by the type dropdown and the search, sorted by the
// column headers), and for saving a file name field. Double-click or Enter opens a folder or
// picks a file; Backspace or the Up button goes to the folder above; arrows, Home, End and Page
// keys move (Shift extends, Ctrl+A selects all when several may be picked); typing a name's first
// letters jumps to it; Escape cancels. Folders are read on a background job and shown when they
// arrive, so a slow drive never stalls the window. `done` runs after the frame it closes in, only
// when something was picked: the chosen paths (one for a save, with the extension added and
// replacing confirmed).
class FileDialog : public Component {
public:
    using Done = std::function<void(std::vector<std::string> paths)>;

    FileDialog(Context &context, FileDialogOptions options, Done done);
    ~FileDialog() override;

    static FileDialog &open(Context &context, FileDialogOptions options, Done done);
    // The folder a dialog with this `remember` key was last in, this session (empty if none)
    static std::string rememberedFolder(const std::string &key);

    void navigate(const std::string &folder, std::string select = {});
    const std::string &folder() const { return folder_; }
    void close();

    Vec2 measure(const Theme &theme) const override;
    void arrange(const Rect &window, const Theme &theme) override;
    void handleInput(const Input &input) override;
    void draw(DrawList &list, const Theme &theme) const override;
    bool hitTest(Vec2 p) const override;

private:
    struct Listing;

    void buildPlaces(Container &column);
    void refilter();
    void receive();
    void goUp();
    // Enter, a double-click or the action button
    void activate();
    void openEntry(size_t index);
    void commitSave(const std::string &typed);
    void finish(std::vector<std::string> paths);
    void selectionChanged();
    bool onKey(const KeyEvent &e);
    bool onText(const TextEvent &e);
    bool typing() const; // a text field in the dialog has the focus

    Context &context_;
    FileDialogOptions options_;
    Done done_;
    std::unique_ptr<Panel> panel_;
    PathBar *pathBar_ = nullptr;
    FileList *list_ = nullptr;
    TextField *search_ = nullptr;
    TextField *name_ = nullptr;
    Dropdown *types_ = nullptr;
    Label *status_ = nullptr;
    Button *action_ = nullptr;
    std::vector<std::pair<ListItem *, std::string>> places_;

    std::string folder_;
    std::string selectAfter_; // a name to select once the folder arrives (the one we came up from)
    std::vector<engine::FolderEntry> all_;
    std::shared_ptr<Listing> listing_;
    double listingSince_ = 0.0;
    std::string error_;
    std::vector<std::string> extensions_;
    std::string searchText_;
    std::string message_; // a note under the list (why a save didn't go ahead), until the next move
    files::SortBy sortBy_ = files::SortBy::Name;
    bool descending_ = false;
    std::string typeAhead_;
    double typeAheadTime_ = -10.0;
    double now_ = 0.0;
    float shown_ = 0.0f;
    bool closed_ = false;
    bool confirming_ = false; // the replace prompt is open over it
    bool focusName_ = true;   // save: the name field takes the focus once it has had input
    env::Connection keys_, text_, actions_;
};

}
