#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace engine {

// A file type choice in an open or save dialog: a label and semicolon-separated patterns,
// e.g. {"Instruments", "*.sfz;*.wav"}
struct FileFilter {
    std::string label;
    std::string patterns;
};

// The system's own dialogs, below, are slow to open on some machines (programs that hook window
// creation) and look nothing like the app: ui::FileDialog is the in-app one, and these stay as a
// fallback (Settings > Performance). The folder functions further down serve the in-app one.

// Shows the system's open-file dialog over the active window and blocks until it closes.
// Returns the chosen paths (empty if cancelled). With `multiple`, several files can be picked.
// Windows only; elsewhere it returns nothing. Call it outside the UI's input pass (from a
// posted action), since the app doesn't redraw while it is open.
// `folder` is where it starts (empty for the system's choice).
std::vector<std::string> openFileDialog(const std::string &title, const std::vector<FileFilter> &filters,
                                        bool multiple = false, const std::string &folder = {});

// The system's save dialog, likewise. It starts in `folder` with `name` filled in, adds
// `extension` (".vbp") when the typed name has none, and asks before replacing a file. Returns the
// chosen path, or empty if cancelled.
std::string saveFileDialog(const std::string &title, const std::vector<FileFilter> &filters, const std::string &folder,
                           const std::string &name, const std::string &extension);

// One thing in a folder, as listFolder reads it
struct FolderEntry {
    std::string name;
    bool folder = false;
    uint64_t size = 0;    // bytes (0 for folders)
    int64_t modified = 0; // seconds since 1970 (UTC)
};

// Reads a folder's files and subfolders, leaving out hidden and system ones (and "." and "..").
// Blocks for as long as the disk (or network) takes: call it from a background job. False with
// the reason when the folder can't be read.
bool listFolder(const std::string &path, std::vector<FolderEntry> &out, std::string &error);

// The user's own folders, or empty when there is none
enum class KnownFolder { Home, Documents, Desktop, Downloads, Music };
std::string knownFolder(KnownFolder which);

// The drives that are there now ("C:\"), with a label saying what kind each is ("Local disk (C:)").
// Doesn't touch the drives themselves, so an empty card reader or a lost network drive can't stall it.
struct DriveInfo {
    std::string path;
    std::string label;
};
std::vector<DriveInfo> drives();

}
