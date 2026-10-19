#include "FileDialog.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

#include "Dialogs.h"
#include "env/Scheduler.h"

namespace ui {

namespace fs = std::filesystem;

// The plain logic

namespace files {

namespace {

bool isSeparator(char c)
{
    return c == '\\' || c == '/';
}

char lower(char c)
{
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

std::string lowered(std::string s)
{
    for (char &c : s)
        c = lower(c);
    return s;
}

// Length of the root at the start of a normalized path: "C:\" is 3, "\\server\share\" its
// length, "\" 1; 0 when relative
size_t rootLength(const std::string &p)
{
    if (p.size() >= 2 && std::isalpha(static_cast<unsigned char>(p[0])) && p[1] == ':')
        return p.size() >= 3 && p[2] == '\\' ? 3 : 2;
    if (p.size() >= 2 && p[0] == '\\' && p[1] == '\\') {
        // \\server\share\ ...
        size_t server = p.find('\\', 2);
        if (server == std::string::npos)
            return p.size();
        size_t share = p.find('\\', server + 1);
        return share == std::string::npos ? p.size() : share + 1;
    }
    if (!p.empty() && p[0] == '\\')
        return 1;
    return 0;
}

}

std::string normalize(const std::string &path)
{
    std::string p;
    p.reserve(path.size() + 1);
    // Backslashes, and no doubled separators (except the two that start a network path)
    for (size_t i = 0; i < path.size(); ++i) {
        const char c = isSeparator(path[i]) ? '\\' : path[i];
        if (c == '\\' && !p.empty() && p.back() == '\\' && p.size() != 1)
            continue;
        p += c;
    }
    // Trim spaces at the ends: pasted paths often have them
    while (!p.empty() && (p.back() == ' ' || p.back() == '"'))
        p.pop_back();
    size_t start = 0;
    while (start < p.size() && (p[start] == ' ' || p[start] == '"'))
        ++start;
    p = p.substr(start);
    if (p.size() == 2 && p[1] == ':')
        p += '\\';
    // No trailing separator, except on a root
    while (p.size() > 1 && p.back() == '\\' && p.size() > rootLength(p))
        p.pop_back();
    if (p.size() >= 2 && p[1] == ':')
        p[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(p[0])));
    return p;
}

std::vector<Crumb> breadcrumbs(const std::string &path)
{
    const std::string p = normalize(path);
    std::vector<Crumb> crumbs;
    if (p.empty())
        return crumbs;
    size_t root = rootLength(p);
    size_t pos = 0;
    if (root > 0) {
        std::string label = p.substr(0, root);
        // "C:\" shows as "C:", a share without its trailing separator
        if (label.size() > 1 && label.back() == '\\')
            label.pop_back();
        if (label == "\\")
            label = "/";
        crumbs.push_back({label, p.substr(0, std::max<size_t>(root, root == 2 ? 2 : root))});
        if (crumbs.back().path.size() == 2 && crumbs.back().path[1] == ':')
            crumbs.back().path += '\\';
        pos = root;
    }
    while (pos < p.size()) {
        if (p[pos] == '\\') {
            ++pos;
            continue;
        }
        size_t end = p.find('\\', pos);
        if (end == std::string::npos)
            end = p.size();
        crumbs.push_back({p.substr(pos, end - pos), p.substr(0, end)});
        pos = end;
    }
    return crumbs;
}

std::string parent(const std::string &path)
{
    const std::vector<Crumb> crumbs = breadcrumbs(path);
    if (crumbs.size() < 2)
        return {};
    return crumbs[crumbs.size() - 2].path;
}

std::string join(const std::string &folder, const std::string &name)
{
    if (folder.empty())
        return normalize(name);
    if (isAbsolute(name))
        return normalize(name);
    std::string f = normalize(folder);
    if (!f.empty() && f.back() != '\\')
        f += '\\';
    return normalize(f + name);
}

bool isAbsolute(const std::string &path)
{
    const std::string p = normalize(path);
    if (p.size() >= 3 && p[1] == ':' && p[2] == '\\')
        return true;
    return !p.empty() && p[0] == '\\';
}

bool samePath(const std::string &a, const std::string &b)
{
    return lowered(normalize(a)) == lowered(normalize(b));
}

std::vector<std::string> extensions(const std::string &patterns)
{
    std::vector<std::string> list;
    size_t pos = 0;
    while (pos <= patterns.size()) {
        size_t end = patterns.find(';', pos);
        if (end == std::string::npos)
            end = patterns.size();
        std::string pattern = patterns.substr(pos, end - pos);
        pattern.erase(0, pattern.find_first_not_of(' '));
        while (!pattern.empty() && pattern.back() == ' ')
            pattern.pop_back();
        pos = end + 1;
        if (pattern.empty())
            continue;
        if (pattern == "*" || pattern == "*.*")
            return {}; // everything
        if (pattern.rfind("*.", 0) == 0)
            pattern = pattern.substr(1);
        else if (pattern[0] != '.')
            pattern = "." + pattern;
        list.push_back(lowered(pattern));
    }
    return list;
}

bool matches(const std::string &name, const std::vector<std::string> &extensions)
{
    if (extensions.empty())
        return true;
    const std::string n = lowered(name);
    for (const std::string &e : extensions) {
        if (n.size() > e.size() && n.compare(n.size() - e.size(), e.size(), e) == 0)
            return true;
    }
    return false;
}

std::string withExtension(const std::string &name, const std::string &extension)
{
    if (extension.empty() || name.empty())
        return name;
    const std::string ext = extension[0] == '.' ? extension : "." + extension;
    if (matches(name, {lowered(ext)}))
        return name;
    // A name ending in a dot gets the extension after it, not a second dot
    std::string base = name;
    while (!base.empty() && base.back() == '.')
        base.pop_back();
    return base + ext;
}

std::string cleanFileName(const std::string &name)
{
    std::string out = name;
    const std::string refused = "<>:\"/\\|?*";
    for (char &c : out) {
        if (static_cast<unsigned char>(c) < 32 || refused.find(c) != std::string::npos)
            c = '_';
    }
    return out;
}

bool naturalLess(const std::string &a, const std::string &b)
{
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const bool da = std::isdigit(static_cast<unsigned char>(a[i])) != 0;
        const bool db = std::isdigit(static_cast<unsigned char>(b[j])) != 0;
        if (da && db) {
            // Compare the numbers: skip leading zeros, then the longer run is larger
            size_t ia = i, jb = j;
            while (ia < a.size() && a[ia] == '0')
                ++ia;
            while (jb < b.size() && b[jb] == '0')
                ++jb;
            size_t ea = ia, eb = jb;
            while (ea < a.size() && std::isdigit(static_cast<unsigned char>(a[ea])))
                ++ea;
            while (eb < b.size() && std::isdigit(static_cast<unsigned char>(b[eb])))
                ++eb;
            if (ea - ia != eb - jb)
                return ea - ia < eb - jb;
            const int c = a.compare(ia, ea - ia, b, jb, eb - jb);
            if (c != 0)
                return c < 0;
            i = ea;
            j = eb;
            continue;
        }
        const char ca = lower(a[i]), cb = lower(b[j]);
        if (ca != cb)
            return ca < cb;
        ++i;
        ++j;
    }
    if (a.size() - i != b.size() - j)
        return a.size() - i < b.size() - j;
    return a < b; // the same but for case: a stable order all the same
}

void sort(std::vector<engine::FolderEntry> &entries, SortBy by, bool descending)
{
    std::stable_sort(entries.begin(), entries.end(), [by, descending](const engine::FolderEntry &x,
                                                                      const engine::FolderEntry &y) {
        if (x.folder != y.folder)
            return x.folder;
        const engine::FolderEntry &a = descending ? y : x;
        const engine::FolderEntry &b = descending ? x : y;
        if (by == SortBy::Date && a.modified != b.modified)
            return a.modified < b.modified;
        if (by == SortBy::Size && a.size != b.size)
            return a.size < b.size;
        if (by == SortBy::Name)
            return naturalLess(a.name, b.name);
        return naturalLess(x.name, y.name); // ties always by name, A to Z
    });
}

std::vector<engine::FolderEntry> filter(const std::vector<engine::FolderEntry> &entries,
                                        const std::vector<std::string> &extensions, const std::string &search)
{
    const std::string needle = lowered(search);
    std::vector<engine::FolderEntry> out;
    for (const engine::FolderEntry &e : entries) {
        if (!e.folder && !matches(e.name, extensions))
            continue;
        if (!needle.empty() && lowered(e.name).find(needle) == std::string::npos)
            continue;
        out.push_back(e);
    }
    return out;
}

std::string sizeText(uint64_t bytes)
{
    char buffer[32];
    if (bytes < 1024) {
        std::snprintf(buffer, sizeof buffer, "%u bytes", static_cast<unsigned>(bytes));
        return buffer;
    }
    const char *units[] = {"KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes) / 1024.0;
    int unit = 0;
    while (value >= 1024.0 && unit < 3) {
        value /= 1024.0;
        ++unit;
    }
    if (value < 10.0 && unit > 0)
        std::snprintf(buffer, sizeof buffer, "%.1f %s", value, units[unit]);
    else
        std::snprintf(buffer, sizeof buffer, "%.0f %s", std::ceil(value), units[unit]);
    return buffer;
}

std::string dateText(int64_t seconds, int64_t now)
{
    if (seconds <= 0)
        return {};
    const std::time_t t = static_cast<std::time_t>(seconds);
    const std::time_t n = static_cast<std::time_t>(now);
    std::tm when{}, today{};
#ifdef _WIN32
    localtime_s(&when, &t);
    localtime_s(&today, &n);
#else
    localtime_r(&t, &when);
    localtime_r(&n, &today);
#endif
    char buffer[48];
    if (when.tm_year == today.tm_year && when.tm_yday == today.tm_yday)
        std::strftime(buffer, sizeof buffer, "Today, %H:%M", &when);
    else if (when.tm_year == today.tm_year && when.tm_yday + 1 == today.tm_yday)
        std::strftime(buffer, sizeof buffer, "Yesterday, %H:%M", &when);
    else if (when.tm_year == today.tm_year)
        std::strftime(buffer, sizeof buffer, "%d %b, %H:%M", &when);
    else
        std::strftime(buffer, sizeof buffer, "%d %b %Y", &when);
    std::string s = buffer;
    if (s.size() > 1 && s[0] == '0')
        s.erase(0, 1); // "3 Mar", not "03 Mar"
    return s;
}

}

namespace {

constexpr double kDoubleClick = 0.4;
constexpr double kShowReading = 0.25; // seconds before a slow folder says it is being read

// The folder each kind of dialog was last in, for this session
std::map<std::string, std::string> &remembered()
{
    static std::map<std::string, std::string> folders;
    return folders;
}

// Listing jobs share one group, so they are waited for (or dropped) when the program ends
env::JobGroup &listingJobs()
{
    static env::JobGroup group;
    return group;
}

// `text` cut to fit `width` at `px`, with "..." when cut
std::string elide(const Theme &theme, const std::string &text, float px, float width)
{
    if (theme.measureText(text, px).x <= width)
        return text;
    size_t lo = 0, hi = text.size();
    while (lo < hi) {
        const size_t mid = (lo + hi + 1) / 2;
        if (theme.measureText(text.substr(0, mid) + "...", px).x <= width)
            lo = mid;
        else
            hi = mid - 1;
    }
    return text.substr(0, lo) + "...";
}

Icon iconFor(const engine::FolderEntry &e)
{
    if (e.folder)
        return Icon::Folder;
    if (files::matches(e.name, {".wav", ".flac", ".mp3", ".ogg", ".aif", ".aiff"}))
        return Icon::Wave;
    if (files::matches(e.name, {".sfz"}))
        return Icon::Keys;
    return Icon::File;
}

}

// The path bar: the folder as breadcrumbs, or a field to type a path into

class PathBar : public Component {
public:
    explicit PathBar(std::function<void(const std::string &)> onGo)
        : onGo_(std::move(onGo)), field_(std::make_unique<TextField>("Type a folder or file, then Enter"))
    {
        field_->setSize(Size::Small);
        field_->onSubmit([this](const std::string &submitted) {
            const std::string text = submitted; // the field's own text, which stopEditing resets
            stopEditing();
            if (onGo_)
                onGo_(text);
        });
    }

    void setPath(const std::string &path)
    {
        path_ = path;
        crumbs_ = files::breadcrumbs(path);
        clicks_.assign(crumbs_.size(), ClickState{});
        if (!editing_)
            field_->setText(path);
    }

    void edit()
    {
        editing_ = true;
        focusPending_ = true;
        field_->setText(path_);
    }
    bool editing() const { return editing_; }
    bool typing() const { return editing_ && field_->focused(); }

    Vec2 measure(const Theme &theme) const override
    {
        return {240.0f, field_->preferredSize(theme).y};
    }

    void arrange(const Rect &rect, const Theme &theme) override
    {
        rect_ = rect;
        field_->arrange(rect, theme);
        // Crumbs from the last back, as many as fit; the rest fold into a "..." that goes to the
        // one just before the first shown
        const float px = theme.fontSize(Size::Small);
        const float pad = rect.h * 0.35f;
        const float sep = rect.h * 0.45f;
        const float room = rect.w - 2.0f * pad - rect.h; // leave a stretch at the end to click into
        const float ellipsis = theme.measureText("...", px).x + 2.0f * pad;
        std::vector<float> widths(crumbs_.size());
        for (size_t i = 0; i < crumbs_.size(); ++i)
            widths[i] = std::min(theme.measureText(crumbs_[i].label, px).x, rect.w * 0.5f) + 2.0f * pad;
        first_ = 0;
        float total = 0.0f;
        for (size_t i = crumbs_.size(); i-- > 0;) {
            const float w = widths[i] + (i + 1 < crumbs_.size() ? sep : 0.0f);
            const float extra = i > 0 ? ellipsis + sep : 0.0f;
            if (total + w + extra > room && i + 1 < crumbs_.size()) {
                first_ = i + 1;
                break;
            }
            total += w;
        }
        rects_.assign(crumbs_.size(), Rect{});
        const float h = rect.h - 8.0f;
        float x = rect.x + pad * 0.5f;
        const float y = rect.y + 4.0f;
        if (first_ > 0) {
            ellipsis_ = {x, y, ellipsis, h};
            x += ellipsis + sep;
        } else {
            ellipsis_ = {};
        }
        for (size_t i = first_; i < crumbs_.size(); ++i) {
            rects_[i] = {x, y, widths[i], h};
            x += widths[i] + sep;
        }
        tail_ = {x - sep, rect.y, std::max(0.0f, rect.x + rect.w - (x - sep)), rect.h};
        px_ = px;
        sep_ = sep;
    }

    void handleInput(const Input &input) override
    {
        signals_ = input.signals;
        if (editing_) {
            field_->handleInput(input);
            if (focusPending_ && input.signals) {
                field_->focus();
                field_->selectAll();
                focusPending_ = false;
            } else if (!focusPending_ && !field_->focused()) {
                // Escape, Enter or a click elsewhere: back to the crumbs
                stopEditing();
            }
            return;
        }
        for (size_t i = first_; i < crumbs_.size(); ++i) {
            if (clicks_[i].update(rects_[i], input, true) && onGo_)
                onGo_(crumbs_[i].path);
        }
        if (ellipsisClick_.update(ellipsis_, input, first_ > 0) && onGo_ && first_ > 0)
            onGo_(crumbs_[first_ - 1].path);
        // A press on the bar's empty end types a path instead
        if (input.pressed && tail_.contains(input.mouse))
            edit();
    }

    void draw(DrawList &list, const Theme &theme) const override
    {
        if (editing_) {
            field_->draw(list, theme);
            return;
        }
        list.add(theme.surface(rect_, Shape::Pill, Elevation::Inset, 0.6f));
        list.pushClip(rect_);
        if (first_ > 0) {
            drawCrumb(list, theme, ellipsis_, ellipsisClick_, "...", false);
            drawSeparator(list, theme, ellipsis_);
        }
        for (size_t i = first_; i < crumbs_.size(); ++i) {
            const bool last = i + 1 == crumbs_.size();
            drawCrumb(list, theme, rects_[i], clicks_[i], crumbs_[i].label, last);
            if (!last)
                drawSeparator(list, theme, rects_[i]);
        }
        list.popClip();
    }

private:
    void stopEditing()
    {
        editing_ = false;
        focusPending_ = false;
        if (signals_ && field_->focused())
            signals_->setFocus(nullptr);
        field_->setText(path_);
    }

    void drawCrumb(DrawList &list, const Theme &theme, const Rect &r, const ClickState &click, const std::string &label,
                   bool current) const
    {
        if (click.held())
            list.add(theme.surface(r, Shape::Pill, Elevation::Inset, 0.4f));
        else if (click.hovered)
            list.add(theme.surface(r, Shape::Pill, Elevation::Raised, 0.35f));
        const TextTone tone = click.hovered ? TextTone::Accent : current ? TextTone::Primary : TextTone::Secondary;
        const float pad = rect_.h * 0.35f;
        list.add(theme.text(elide(theme, label, px_, r.w - 2.0f * pad), {r.x + pad, r.y, r.w - 2.0f * pad, r.h}, px_,
                            TextAlign::Start, theme.textColor(tone)));
    }

    void drawSeparator(DrawList &list, const Theme &theme, const Rect &after) const
    {
        const float s = sep_ * 0.7f;
        drawIcon(list, theme, Icon::Chevron, {after.x + after.w + (sep_ - s) * 0.5f, after.y + (after.h - s) * 0.5f, s, s},
                 TextTone::Muted);
    }

    std::function<void(const std::string &)> onGo_;
    std::unique_ptr<TextField> field_;
    std::string path_;
    std::vector<files::Crumb> crumbs_;
    std::vector<Rect> rects_;
    std::vector<ClickState> clicks_;
    ClickState ellipsisClick_;
    Rect ellipsis_, tail_;
    size_t first_ = 0;
    float px_ = 13.0f, sep_ = 10.0f;
    bool editing_ = false;
    bool focusPending_ = false;
    InputSignals *signals_ = nullptr;
};

// The folder's contents: a header of sortable columns over the rows, drawn here rather than
// made of a component per row, so a folder of thousands of files costs only the rows in view

class FileList : public Component {
public:
    std::function<void(size_t)> onOpen;  // double-click
    std::function<void()> onSelect;      // the selection changed by the mouse
    std::function<void(files::SortBy)> onSort;

    explicit FileList(bool multiple) : multiple_(multiple) {}

    // Keeps the selection and the cursor on the same names where they are still there
    void setEntries(std::vector<engine::FolderEntry> entries)
    {
        std::vector<std::string> selectedNames;
        for (size_t i = 0; i < entries_.size(); ++i) {
            if (selected_[i])
                selectedNames.push_back(entries_[i].name);
        }
        const std::string cursorName = cursor_ >= 0 ? entries_[static_cast<size_t>(cursor_)].name : std::string();
        const bool sameFolder = !entries_.empty() || keepScroll_;
        entries_ = std::move(entries);
        selected_.assign(entries_.size(), 0);
        cursor_ = anchor_ = -1;
        for (size_t i = 0; i < entries_.size(); ++i) {
            if (std::find(selectedNames.begin(), selectedNames.end(), entries_[i].name) != selectedNames.end())
                selected_[i] = 1;
            if (entries_[i].name == cursorName)
                cursor_ = anchor_ = static_cast<int>(i);
        }
        if (!sameFolder)
            scroll_ = target_ = 0.0f;
        keepScroll_ = true;
        now_ = static_cast<int64_t>(std::time(nullptr));
        hovered_ = -1;
        lastClickRow_ = -1;
    }
    // A new folder: nothing selected, back at the top
    void clear()
    {
        entries_.clear();
        selected_.clear();
        cursor_ = anchor_ = -1;
        scroll_ = target_ = 0.0f;
        keepScroll_ = false;
    }
    const std::vector<engine::FolderEntry> &entries() const { return entries_; }
    std::vector<size_t> selection() const
    {
        std::vector<size_t> out;
        for (size_t i = 0; i < selected_.size(); ++i) {
            if (selected_[i])
                out.push_back(i);
        }
        return out;
    }
    int cursor() const { return cursor_; }
    void setMessage(std::string message) { message_ = std::move(message); }
    void setSort(files::SortBy by, bool descending)
    {
        sortBy_ = by;
        descending_ = descending;
    }

    // The keyboard: moves the cursor and selects it (Shift extends from the anchor)
    void moveTo(int index, bool extend)
    {
        if (entries_.empty())
            return;
        index = std::clamp(index, 0, static_cast<int>(entries_.size()) - 1);
        if (!extend || !multiple_ || anchor_ < 0)
            anchor_ = index;
        std::fill(selected_.begin(), selected_.end(), 0);
        const int from = std::min(anchor_, index), to = std::max(anchor_, index);
        for (int i = from; i <= to; ++i)
            selected_[static_cast<size_t>(i)] = 1;
        cursor_ = index;
        reveal_ = true;
    }
    void move(int by, bool extend) { moveTo(cursor_ < 0 ? (by > 0 ? 0 : static_cast<int>(entries_.size()) - 1) : cursor_ + by, extend); }
    int pageRows() const { return std::max(1, static_cast<int>(body_.h / std::max(1.0f, rowH_)) - 1); }
    void selectAll()
    {
        if (!multiple_)
            return;
        std::fill(selected_.begin(), selected_.end(), 1);
    }
    void selectName(const std::string &name)
    {
        for (size_t i = 0; i < entries_.size(); ++i) {
            if (files::samePath(entries_[i].name, name)) {
                moveTo(static_cast<int>(i), false);
                return;
            }
        }
    }
    // Type-ahead: the next name starting with `prefix` (any case), from the cursor on
    bool jumpTo(const std::string &prefix)
    {
        if (entries_.empty() || prefix.empty())
            return false;
        const size_t n = entries_.size();
        // A single letter typed again moves on to the next match; longer prefixes stay put if they still match
        const size_t start = cursor_ < 0 ? 0 : static_cast<size_t>(cursor_) + (prefix.size() == 1 ? 1 : 0);
        for (size_t k = 0; k < n; ++k) {
            const size_t i = (start + k) % n;
            const std::string &name = entries_[i].name;
            if (name.size() >= prefix.size() && files::samePath(name.substr(0, prefix.size()), prefix)) {
                moveTo(static_cast<int>(i), false);
                return true;
            }
        }
        return false;
    }

    Vec2 measure(const Theme &theme) const override
    {
        return {360.0f, theme.controlHeight(Size::Small) * 7.0f};
    }

    void arrange(const Rect &rect, const Theme &theme) override
    {
        rect_ = rect;
        rowH_ = std::round(theme.controlHeight(Size::Small) * 0.95f);
        const float pad = 8.0f;
        header_ = {rect.x + pad, rect.y + 4.0f, rect.w - 2.0f * pad, rowH_};
        body_ = {rect.x + 4.0f, header_.y + header_.h + 6.0f, rect.w - 8.0f, 0.0f};
        body_.h = std::max(0.0f, rect.y + rect.h - 4.0f - body_.y);
        const float caption = theme.fontSize(TextStyle::Caption);
        dateW_ = theme.measureText("Yesterday, 00:00", caption).x + 24.0f;
        sizeW_ = theme.measureText("000.0 MB", caption).x + 24.0f;
        // Columns, right to left: size, date, then the name takes what is left
        const float bar = 10.0f;
        sizeX_ = header_.x + header_.w - bar - sizeW_;
        dateX_ = sizeX_ - dateW_;
        nameX_ = header_.x;
        headerRects_[0] = {nameX_, header_.y, dateX_ - nameX_, header_.h};
        headerRects_[1] = {dateX_, header_.y, dateW_, header_.h};
        headerRects_[2] = {sizeX_, header_.y, sizeW_, header_.h};

        maxScroll_ = std::max(0.0f, static_cast<float>(entries_.size()) * rowH_ - body_.h);
        if (reveal_ && cursor_ >= 0) {
            const float top = cursor_ * rowH_;
            if (top < target_)
                target_ = top;
            else if (top + rowH_ > target_ + body_.h)
                target_ = top + rowH_ - body_.h;
            reveal_ = false;
        }
        target_ = std::clamp(target_, 0.0f, maxScroll_);
        scroll_ = std::clamp(scroll_, 0.0f, maxScroll_);
        track_ = maxScroll_ > 0.0f ? Rect{body_.x + body_.w - 8.0f, body_.y + 2.0f, 6.0f, body_.h - 4.0f} : Rect{};
    }

    void handleInput(const Input &input) override
    {
        scroll_ += (target_ - scroll_) * input.ease(22.0f);
        if (std::fabs(target_ - scroll_) < 0.5f)
            scroll_ = target_;

        if (body_.contains(input.mouse) && input.scroll != 0.0f) {
            target_ = std::clamp(target_ - input.scroll * rowH_ * 3.0f, 0.0f, maxScroll_);
            input.scroll = 0.0f;
        }

        // Column headers sort
        const files::SortBy columns[] = {files::SortBy::Name, files::SortBy::Date, files::SortBy::Size};
        for (int i = 0; i < 3; ++i) {
            if (headerClicks_[i].update(headerRects_[i], input, true) && onSort)
                onSort(columns[i]);
        }

        // The scroll bar
        const Rect th = thumb();
        thumbHovered_ = maxScroll_ > 0.0f && Rect{track_.x - 4.0f, track_.y, track_.w + 8.0f, track_.h}.contains(input.mouse);
        if (input.pressed && thumbHovered_) {
            if (th.contains(input.mouse)) {
                dragging_ = true;
                dragStart_ = input.mouse.y;
                dragOffset_ = target_;
            } else {
                const float page = body_.h * 0.9f;
                target_ = std::clamp(target_ + (input.mouse.y < th.y ? -page : page), 0.0f, maxScroll_);
            }
            return;
        }
        if (dragging_) {
            if (!input.down) {
                dragging_ = false;
            } else {
                const float span = std::max(1.0f, track_.h - th.h);
                target_ = scroll_ = std::clamp(dragOffset_ + (input.mouse.y - dragStart_) / span * maxScroll_, 0.0f,
                                               maxScroll_);
            }
            return;
        }

        hovered_ = -1;
        if (body_.contains(input.mouse)) {
            const int row = static_cast<int>(std::floor((input.mouse.y - body_.y + scroll_) / rowH_));
            if (row >= 0 && row < static_cast<int>(entries_.size()))
                hovered_ = row;
        }
        if (!input.pressed || !body_.contains(input.mouse))
            return;

        const bool ctrl = (input.mods & engine::ModCtrl) != 0;
        const bool shift = input.shift;
        const int row = hovered_;
        if (row < 0) {
            if (!ctrl && !shift) {
                std::fill(selected_.begin(), selected_.end(), 0);
                if (onSelect)
                    onSelect();
            }
            return;
        }
        if (row == lastClickRow_ && input.time - lastClickTime_ < kDoubleClick && !ctrl && !shift) {
            lastClickRow_ = -1;
            if (onOpen)
                onOpen(static_cast<size_t>(row));
            return;
        }
        lastClickRow_ = row;
        lastClickTime_ = input.time;
        const size_t r = static_cast<size_t>(row);
        if (multiple_ && ctrl && !shift) {
            selected_[r] = !selected_[r];
            cursor_ = anchor_ = row;
        } else if (multiple_ && shift && anchor_ >= 0) {
            if (!ctrl)
                std::fill(selected_.begin(), selected_.end(), 0);
            for (int i = std::min(anchor_, row); i <= std::max(anchor_, row); ++i)
                selected_[static_cast<size_t>(i)] = 1;
            cursor_ = row;
        } else {
            std::fill(selected_.begin(), selected_.end(), 0);
            selected_[r] = 1;
            cursor_ = anchor_ = row;
        }
        if (onSelect)
            onSelect();
    }

    void draw(DrawList &list, const Theme &theme) const override
    {
        // The list sits in a well
        list.add(theme.surface(rect_, Shape::Rounded, Elevation::Inset, 0.7f));

        // Headers, the sorted one lit with an arrow its way
        const char *titles[] = {"Name", "Date modified", "Size"};
        const files::SortBy columns[] = {files::SortBy::Name, files::SortBy::Date, files::SortBy::Size};
        const float caption = theme.fontSize(TextStyle::Caption);
        for (int i = 0; i < 3; ++i) {
            const Rect &r = headerRects_[i];
            const bool sorted = sortBy_ == columns[i];
            const TextTone tone = headerClicks_[i].hovered || sorted ? TextTone::Accent : TextTone::Secondary;
            const float textX = r.x + (i == 0 ? iconRoom() + 6.0f : 12.0f);
            list.add(theme.text(titles[i], {textX, r.y, r.w, r.h}, caption, TextAlign::Start, theme.textColor(tone)));
            if (sorted) {
                const float w = theme.measureText(titles[i], caption).x;
                const float a = caption * 0.5f;
                list.add(theme.ink({textX + w + 6.0f, r.y + (r.h - a * 0.7f) * 0.5f, a, a * 0.7f},
                                   descending_ ? Shape::TriangleDown : Shape::TriangleUp, TextTone::Accent));
            }
        }
        list.add(theme.surface({header_.x, header_.y + header_.h + 2.0f, header_.w, 3.0f}, Shape::Pill,
                               Elevation::Inset, 0.2f));

        list.pushClip(body_);
        const float px = theme.fontSize(Size::Small);
        const float rowW = body_.w - (maxScroll_ > 0.0f ? 14.0f : 0.0f);
        if (rowH_ > 0.0f && !entries_.empty()) {
            const int first = std::max(0, static_cast<int>(scroll_ / rowH_));
            const int last = std::min(static_cast<int>(entries_.size()) - 1,
                                      static_cast<int>((scroll_ + body_.h) / rowH_) + 1);
            for (int i = first; i <= last; ++i) {
                const engine::FolderEntry &e = entries_[static_cast<size_t>(i)];
                const Rect r{body_.x + 2.0f, body_.y + i * rowH_ - scroll_, rowW - 4.0f, rowH_ - 2.0f};
                const bool selected = selected_[static_cast<size_t>(i)] != 0;
                if (selected)
                    list.add(theme.surface(r, Shape::Rounded, Elevation::Inset, 0.5f));
                else if (i == hovered_)
                    list.add(theme.surface(r, Shape::Rounded, Elevation::Raised, 0.3f));
                if (i == cursor_ && !selected)
                    list.add(theme.accent({r.x + 3.0f, r.y + r.h * 0.3f, 3.0f, r.h * 0.4f}, Shape::Pill, 0.7f));

                const TextTone tone = selected ? TextTone::Accent : TextTone::Primary;
                const float icon = px * 1.25f;
                drawIcon(list, theme, iconFor(e), {nameX_ + 6.0f, r.y + (r.h - icon) * 0.5f, icon, icon},
                         e.folder ? TextTone::Accent : TextTone::Secondary);
                const float nameLeft = nameX_ + iconRoom() + 6.0f;
                const float nameW = dateX_ - nameLeft - 8.0f;
                list.add(theme.text(elide(theme, e.name, px, nameW), {nameLeft, r.y, nameW, r.h}, px, TextAlign::Start,
                                    theme.textColor(tone)));
                list.add(theme.text(files::dateText(e.modified, now_), {dateX_ + 12.0f, r.y, dateW_ - 12.0f, r.h},
                                    caption, TextAlign::Start, theme.textColor(TextTone::Secondary)));
                if (!e.folder)
                    list.add(theme.text(files::sizeText(e.size), {sizeX_, r.y, sizeW_ - 8.0f, r.h}, caption,
                                        TextAlign::End, theme.textColor(TextTone::Secondary)));
            }
        }
        if (entries_.empty() && !message_.empty())
            list.add(theme.text(message_, {body_.x, body_.y, body_.w, std::min(body_.h, rowH_ * 3.0f)}, TextStyle::Body,
                                TextTone::Muted, TextAlign::Center));
        list.popClip();

        if (maxScroll_ > 0.0f) {
            list.add(theme.surface(track_, Shape::Pill, Elevation::Inset, 0.3f));
            list.add(theme.surface(thumb(), Shape::Pill, Elevation::Raised, thumbHovered_ || dragging_ ? 0.5f : 0.3f));
        }
    }

private:
    float iconRoom() const { return rowH_ * 0.9f; }
    Rect thumb() const
    {
        if (maxScroll_ <= 0.0f)
            return {};
        const float content = maxScroll_ + body_.h;
        const float h = std::max(24.0f, track_.h * body_.h / content);
        const float y = track_.y + (track_.h - h) * (scroll_ / maxScroll_);
        return {track_.x, y, track_.w, h};
    }

    bool multiple_;
    std::vector<engine::FolderEntry> entries_;
    std::vector<char> selected_;
    int cursor_ = -1, anchor_ = -1, hovered_ = -1;
    int lastClickRow_ = -1;
    double lastClickTime_ = -10.0;
    std::string message_;
    files::SortBy sortBy_ = files::SortBy::Name;
    bool descending_ = false;
    int64_t now_ = 0;
    Rect header_, body_, track_;
    Rect headerRects_[3];
    ClickState headerClicks_[3];
    float nameX_ = 0.0f, dateX_ = 0.0f, sizeX_ = 0.0f, dateW_ = 0.0f, sizeW_ = 0.0f;
    float rowH_ = 24.0f;
    float scroll_ = 0.0f, target_ = 0.0f, maxScroll_ = 0.0f;
    bool reveal_ = false;
    bool keepScroll_ = true;
    bool dragging_ = false;
    bool thumbHovered_ = false;
    float dragStart_ = 0.0f, dragOffset_ = 0.0f;
};

// A folder being read on a background job
struct FileDialog::Listing {
    std::mutex mutex;
    bool done = false;
    bool ok = false;
    std::vector<engine::FolderEntry> entries;
    std::string error;
};

// FileDialog

FileDialog::FileDialog(Context &context, FileDialogOptions options, Done done)
    : context_(context), options_(std::move(options)), done_(std::move(done)),
      panel_(std::make_unique<Panel>(Direction::Column))
{
    if (options_.action.empty())
        options_.action = options_.save ? "Save" : "Open";
    if (options_.title.empty())
        options_.title = options_.save ? "Save" : "Open";
    if (options_.save)
        options_.multiple = false;

    panel_->setAlign(Align::Stretch);
    panel_->setDepth(Depth::Deep);

    auto &header = panel_->add<Container>(Direction::Row);
    header.setAlign(Align::Center);
    header.add<Label>(options_.title, TextStyle::Title);
    header.add<Spacer>();
    header.add<Button>([this] { close(); })
        .setIcon(Icon::Close)
        .setVariant(ButtonVariant::Flat)
        .setShape(Shape::Circle)
        .setSize(Size::Small);

    // Up, the path, the search
    auto &nav = panel_->add<Container>(Direction::Row);
    nav.setAlign(Align::Center);
    nav.add<Button>([this] { goUp(); })
        .setIcon(Icon::Up)
        .setShape(Shape::Circle)
        .setSize(Size::Small);
    pathBar_ = &nav.add<PathBar>([this](const std::string &typed) {
        // A typed path: a folder goes there, a file is picked (or named, to save)
        const std::string path = files::join(folder_, typed);
        std::error_code ec;
        if (fs::is_directory(path, ec)) {
            navigate(path);
        } else if (options_.save) {
            const std::string folder = files::parent(path);
            if (!folder.empty() && !files::samePath(folder, folder_))
                navigate(folder);
            name_->setText(fs::path(path).filename().string());
            commitSave(name_->text());
        } else if (fs::is_regular_file(path, ec)) {
            finish({path});
        } else {
            message_ = "Can't find " + path;
        }
    });
    pathBar_->setGrow(1.0f);
    search_ = &nav.add<TextField>("Search this folder");
    search_->setSize(Size::Small).onChange([this](const std::string &text) {
        searchText_ = text;
        refilter();
    });
    search_->onSubmit([this](const std::string &) {
        // Enter in the search: the first match, if nothing is picked yet
        if (list_->selection().empty())
            list_->moveTo(0, false);
        activate();
    });
    search_->setMinSize({220.0f, 0.0f});

    // Places beside the folder's contents
    auto &middle = panel_->add<Container>(Direction::Row);
    middle.setAlign(Align::Stretch).setGrow(1.0f);
    auto &places = middle.add<ScrollView>();
    places.setMinSize({210.0f, 0.0f});
    buildPlaces(places.content());
    list_ = &middle.add<FileList>(options_.multiple);
    list_->setGrow(1.0f);
    list_->onOpen = [this](size_t i) { openEntry(i); };
    list_->onSelect = [this] { selectionChanged(); };
    list_->onSort = [this](files::SortBy by) {
        if (by == sortBy_) {
            descending_ = !descending_;
        } else {
            sortBy_ = by;
            descending_ = by != files::SortBy::Name; // newest and largest first
        }
        refilter();
    };

    std::vector<std::string> typeNames;
    for (const engine::FileFilter &f : options_.filters)
        typeNames.push_back(f.label);
    if (typeNames.empty())
        typeNames.push_back("All files");
    auto makeTypes = [this, typeNames](Container &row) {
        types_ = &row.add<Dropdown>(context_, typeNames, 0, [this](int i) {
            extensions_ = files::extensions(options_.filters.empty() ? std::string()
                                                                     : options_.filters[static_cast<size_t>(i)].patterns);
            refilter();
        });
        types_->setSize(Size::Small);
    };
    if (!options_.filters.empty())
        extensions_ = files::extensions(options_.filters.front().patterns);

    if (options_.save) {
        auto &nameRow = panel_->add<Container>(Direction::Row);
        nameRow.setAlign(Align::Center);
        nameRow.add<Label>("File name", Size::Small).setTone(TextTone::Secondary);
        name_ = &nameRow.add<TextField>("Name");
        name_->setSize(Size::Small).onSubmit([this](const std::string &text) { commitSave(text); });
        name_->setGrow(1.0f);
        name_->setText(files::cleanFileName(options_.name));
        makeTypes(nameRow);
    }

    auto &footer = panel_->add<Container>(Direction::Row);
    footer.setAlign(Align::Center);
    status_ = &footer.add<Label>("", TextStyle::Caption, TextTone::Secondary);
    status_->setGrow(1.0f);
    if (!options_.save)
        makeTypes(footer);
    footer.add<Button>([this] { close(); }).setLabel("Cancel").setVariant(ButtonVariant::Flat);
    action_ = &footer.add<Button>([this] { activate(); });
    action_->setLabel(options_.action).setVariant(ButtonVariant::Accent).setShape(Shape::Pill);

    // Modal: keys and actions stop here. A focused field (the path, the search, the name) is ahead
    // of these at Priority::Focus; the replace prompt, newer, is ahead of them at the same priority.
    keys_ = context.signals().key.connect([this](const KeyEvent &e) { return onKey(e); }, Priority::Modal);
    text_ = context.signals().text.connect([this](const TextEvent &e) { return onText(e); }, Priority::Modal);
    actions_ = context.signals().action.connect([this](const ActionEvent &e) {
        if (!closed_ && e.id == "edit.selectAll" && !typing()) {
            list_->selectAll();
            selectionChanged();
        }
        return true;
    }, Priority::Modal);

    // Where to start: where this kind of dialog was last, else the caller's folder, else Documents;
    // the nearest folder above that still exists
    std::string start = rememberedFolder(options_.remember);
    std::error_code ec;
    if (start.empty() || !fs::is_directory(start, ec))
        start = options_.folder;
    while (!start.empty() && !fs::is_directory(start, ec))
        start = files::parent(start);
    if (start.empty())
        start = engine::knownFolder(engine::KnownFolder::Documents);
    if (start.empty())
        start = fs::current_path(ec).string();
    navigate(start);
}

FileDialog::~FileDialog() = default;

FileDialog &FileDialog::open(Context &context, FileDialogOptions options, Done done)
{
    OverlayOptions overlay;
    overlay.modal = true;
    overlay.closeOnOutsideClick = false;
    return context.openOverlay<FileDialog>(std::move(overlay), context, std::move(options), std::move(done));
}

std::string FileDialog::rememberedFolder(const std::string &key)
{
    if (key.empty())
        return {};
    auto it = remembered().find(key);
    return it == remembered().end() ? std::string() : it->second;
}

void FileDialog::buildPlaces(Container &column)
{
    auto heading = [&](const std::string &text) {
        column.add<Label>(text, TextStyle::Caption, TextTone::Muted);
    };
    column.setAlign(Align::Stretch);
    ListView *group = nullptr;
    auto add = [&](const std::string &label, const std::string &path, Icon icon) {
        if (path.empty())
            return;
        for (const auto &p : places_) {
            if (files::samePath(p.second, path))
                return;
        }
        std::error_code ec;
        if (!fs::is_directory(path, ec))
            return;
        const std::string target = files::normalize(path);
        auto &item = group->add<ListItem>(label, [this, target] { navigate(target); });
        item.setIcon(icon).setSize(Size::Small);
        places_.push_back({&item, target});
    };

    if (!options_.places.empty()) {
        heading("This app");
        group = &column.add<ListView>();
    group->setAlign(Align::Stretch);
        group->setAlign(Align::Stretch);
        for (const FilePlace &p : options_.places)
            add(p.label, p.path, p.icon);
    }
    heading("Places");
    group = &column.add<ListView>();
    group->setAlign(Align::Stretch);
    add("Desktop", engine::knownFolder(engine::KnownFolder::Desktop), Icon::Folder);
    add("Documents", engine::knownFolder(engine::KnownFolder::Documents), Icon::Folder);
    add("Downloads", engine::knownFolder(engine::KnownFolder::Downloads), Icon::Download);
    add("Music", engine::knownFolder(engine::KnownFolder::Music), Icon::Wave);
    add("Home", engine::knownFolder(engine::KnownFolder::Home), Icon::Folder);
    heading("Drives");
    group = &column.add<ListView>();
    group->setAlign(Align::Stretch);
    for (const engine::DriveInfo &d : engine::drives()) {
        // Drives aren't checked for existence (an empty card reader would stall); they are listed as they are
        const std::string target = files::normalize(d.path);
        auto &item = group->add<ListItem>(d.label, [this, target] { navigate(target); });
        item.setIcon(Icon::Grid).setSize(Size::Small);
        places_.push_back({&item, target});
    }
}

void FileDialog::navigate(const std::string &folder, std::string select)
{
    const std::string target = files::normalize(folder);
    if (target.empty())
        return;
    folder_ = target;
    selectAfter_ = std::move(select);
    all_.clear();
    error_.clear();
    list_->clear();
    list_->setMessage({});
    pathBar_->setPath(folder_);
    if (!searchText_.empty()) {
        searchText_.clear();
        search_->setText({});
    }
    message_.clear();

    auto listing = std::make_shared<Listing>();
    listing_ = listing;
    listingSince_ = now_;
    auto job = [listing, target] {
        std::vector<engine::FolderEntry> entries;
        std::string error;
        const bool ok = engine::listFolder(target, entries, error);
        std::lock_guard<std::mutex> lock(listing->mutex);
        listing->entries = std::move(entries);
        listing->error = std::move(error);
        listing->ok = ok;
        listing->done = true;
    };
    if (options_.environment)
        env::runJob(*options_.environment, env::Lane::Interactive, std::move(job), listingJobs());
    else
        std::thread(std::move(job)).detach();
}

void FileDialog::receive()
{
    if (!listing_)
        return;
    {
        std::lock_guard<std::mutex> lock(listing_->mutex);
        if (!listing_->done) {
            if (now_ - listingSince_ > kShowReading)
                list_->setMessage("Reading the folder...");
            return;
        }
        all_ = std::move(listing_->entries);
        error_ = listing_->ok ? std::string() : listing_->error;
    }
    listing_.reset();
    refilter();
    if (!selectAfter_.empty()) {
        list_->selectName(selectAfter_);
        selectAfter_.clear();
    }
}

void FileDialog::refilter()
{
    std::vector<engine::FolderEntry> shown = files::filter(all_, extensions_, searchText_);
    files::sort(shown, sortBy_, descending_);
    const bool empty = shown.empty();
    list_->setSort(sortBy_, descending_);
    list_->setEntries(std::move(shown));
    if (listing_)
        return;
    if (!error_.empty())
        list_->setMessage(error_);
    else if (empty)
        list_->setMessage(all_.empty() ? "This folder is empty." : "Nothing here matches the type or the search.");
    else
        list_->setMessage({});
}

void FileDialog::goUp()
{
    const std::string up = files::parent(folder_);
    if (up.empty())
        return;
    // The folder we came from stays selected, so going back down is Enter
    const std::vector<files::Crumb> crumbs = files::breadcrumbs(folder_);
    navigate(up, crumbs.empty() ? std::string() : crumbs.back().label);
}

void FileDialog::selectionChanged()
{
    message_.clear();
    if (!options_.save)
        return;
    // Picking a file to save over puts its name in the field
    const std::vector<size_t> sel = list_->selection();
    if (sel.size() == 1 && !list_->entries()[sel[0]].folder)
        name_->setText(list_->entries()[sel[0]].name);
}

void FileDialog::activate()
{
    if (closed_)
        return;
    const auto &entries = list_->entries();
    std::vector<size_t> sel = list_->selection();
    if (options_.save) {
        // A folder picked in the list (not typing a name) opens; otherwise save with the name
        if (sel.size() == 1 && entries[sel[0]].folder && !(name_ && name_->focused()))
            navigate(files::join(folder_, entries[sel[0]].name));
        else
            commitSave(name_->text());
        return;
    }
    if (sel.empty())
        return;
    std::vector<std::string> paths;
    for (size_t i : sel) {
        if (!entries[i].folder)
            paths.push_back(files::join(folder_, entries[i].name));
    }
    if (paths.empty()) {
        // Only folders picked: go into the first
        navigate(files::join(folder_, entries[sel[0]].name));
        return;
    }
    if (!options_.multiple)
        paths.resize(1);
    finish(std::move(paths));
}

void FileDialog::openEntry(size_t index)
{
    const auto &entries = list_->entries();
    if (index >= entries.size())
        return;
    const engine::FolderEntry &e = entries[index];
    if (e.folder) {
        navigate(files::join(folder_, e.name));
    } else if (options_.save) {
        name_->setText(e.name);
        commitSave(e.name);
    } else {
        finish({files::join(folder_, e.name)});
    }
}

void FileDialog::commitSave(const std::string &typed)
{
    std::string text = typed;
    text.erase(0, text.find_first_not_of(' '));
    while (!text.empty() && text.back() == ' ')
        text.pop_back();
    if (text.empty()) {
        message_ = "Type a name for the file.";
        return;
    }
    // A name may be a path too: relative to this folder, or anywhere
    std::string path = files::join(folder_, text);
    std::error_code ec;
    if (fs::is_directory(path, ec)) {
        navigate(path);
        name_->setText({});
        return;
    }
    const std::string folder = files::parent(path);
    std::string name = fs::path(path).filename().string();
    if (folder.empty() || !fs::is_directory(folder, ec)) {
        message_ = "The folder " + (folder.empty() ? path : folder) + " doesn't exist.";
        return;
    }
    if (files::cleanFileName(name) != name) {
        message_ = "A file name can't contain any of  < > : \" / \\ | ? *";
        return;
    }
    name = files::withExtension(name, options_.extension);
    const std::string full = files::join(folder, name);
    if (fs::exists(full, ec)) {
        if (fs::is_directory(full, ec)) {
            navigate(full);
            return;
        }
        confirming_ = true;
        Dialog &confirm = Dialog::open(context_, "Replace \"" + name + "\"?",
                                       "There's already a file with that name in " + folder +
                                           ". Replacing it overwrites what is in it.");
        confirm.setWidth(480.0f);
        confirm.onClose([this] { confirming_ = false; });
        confirm.addButton("Cancel");
        confirm.addButton("Replace", [this, full] { finish({full}); }, true);
        return;
    }
    finish({full});
}

void FileDialog::finish(std::vector<std::string> paths)
{
    if (closed_)
        return;
    if (!paths.empty() && !options_.remember.empty())
        remembered()[options_.remember] = files::parent(paths.front()).empty() ? folder_ : files::parent(paths.front());
    Done done = std::move(done_);
    done_ = nullptr;
    closed_ = true;
    context_.closeOverlay(*this);
    // After the frame, like the native dialog's posted callers: the caller may change the tree
    if (done)
        context_.post([done = std::move(done), paths = std::move(paths)] { done(paths); });
}

void FileDialog::close()
{
    if (closed_)
        return;
    if (!options_.remember.empty())
        remembered()[options_.remember] = folder_;
    closed_ = true;
    context_.closeOverlay(*this);
}

bool FileDialog::typing() const
{
    return search_->focused() || (name_ && name_->focused()) || pathBar_->typing();
}

bool FileDialog::onKey(const KeyEvent &e)
{
    if (closed_)
        return false;
    const bool ctrl = (e.mods & engine::ModCtrl) != 0;
    const bool alt = (e.mods & engine::ModAlt) != 0;
    const bool shift = (e.mods & engine::ModShift) != 0;
    const bool chord = (e.mods & (engine::ModCtrl | engine::ModAlt | engine::ModSuper)) != 0;
    if (!e.down)
        return !chord;
    if (e.key == Key::Escape && e.mods == 0) {
        close();
        return true;
    }
    if (ctrl && (e.key == Key::L)) {
        pathBar_->edit();
        return true;
    }
    if (ctrl && e.key == Key::F) {
        search_->focus();
        search_->selectAll();
        return true;
    }
    if (alt && e.key == Key::Up) {
        goUp();
        return true;
    }
    if (chord)
        return false; // shortcuts go on to the keymap (the action hook keeps app commands from running)
    const int rows = static_cast<int>(list_->entries().size());
    switch (e.key) {
    case Key::Up:
        list_->move(-1, shift);
        selectionChanged();
        return true;
    case Key::Down:
        list_->move(1, shift);
        selectionChanged();
        return true;
    case Key::PageUp:
        list_->move(-list_->pageRows(), shift);
        selectionChanged();
        return true;
    case Key::PageDown:
        list_->move(list_->pageRows(), shift);
        selectionChanged();
        return true;
    case Key::Home:
        list_->moveTo(0, shift);
        selectionChanged();
        return true;
    case Key::End:
        list_->moveTo(rows - 1, shift);
        selectionChanged();
        return true;
    case Key::Enter:
        activate();
        return true;
    case Key::Backspace:
        goUp();
        return true;
    case Key::Tab:
        // Into the name field when saving, else the search
        if (name_) {
            name_->focus();
            name_->selectAll();
        } else {
            search_->focus();
        }
        return true;
    default:
        return true;
    }
}

bool FileDialog::onText(const TextEvent &e)
{
    if (closed_ || typing())
        return false;
    if (e.codepoint < 32 || e.codepoint >= 128)
        return true;
    // Typing a name's first letters jumps to it; a pause starts over
    if (now_ - typeAheadTime_ > 1.0)
        typeAhead_.clear();
    typeAheadTime_ = now_;
    typeAhead_ += static_cast<char>(e.codepoint);
    if (list_->jumpTo(typeAhead_))
        selectionChanged();
    return true;
}

Vec2 FileDialog::measure(const Theme &) const
{
    return {};
}

void FileDialog::arrange(const Rect &window, const Theme &theme)
{
    const float w = std::min(1000.0f, window.w - 64.0f);
    const float h = std::min(680.0f, window.h - 64.0f);
    const float lift = (1.0f - shown_) * 16.0f;
    rect_ = {window.x + (window.w - w) * 0.5f, window.y + (window.h - h) * 0.45f + lift, w, h};
    panel_->arrange(rect_, theme);
}

void FileDialog::handleInput(const Input &input)
{
    now_ = input.time;
    if (listingSince_ == 0.0)
        listingSince_ = now_;
    shown_ += (1.0f - shown_) * input.ease(16.0f);
    receive();
    for (auto &p : places_)
        p.first->setSelected(files::samePath(p.second, folder_));

    // While the replace prompt is up, this dialog is under its veil
    panel_->handleInput(confirming_ ? input.occluded() : input);

    if (focusName_ && name_ && input.signals) {
        name_->focus();
        name_->selectAll();
        focusName_ = false;
    }
    // What the button will open, or the latest note
    std::string status = message_;
    if (status.empty() && !options_.save && !listing_) {
        const std::vector<size_t> sel = list_->selection();
        size_t picked = 0;
        for (size_t i : sel)
            picked += list_->entries()[i].folder ? 0 : 1;
        if (picked == 1 && sel.size() == 1)
            status = list_->entries()[sel[0]].name;
        else if (picked > 1)
            status = std::to_string(picked) + " files selected";
        else if (sel.empty())
            status = std::to_string(list_->entries().size()) + (list_->entries().size() == 1 ? " item" : " items") +
                     (options_.multiple ? "   (Ctrl or Shift click picks several)" : "");
    }
    status_->setText(status);
}

void FileDialog::draw(DrawList &list, const Theme &theme) const
{
    panel_->draw(list, theme);
}

bool FileDialog::hitTest(Vec2 p) const
{
    return panel_->rect().contains(p);
}

}
