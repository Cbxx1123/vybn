#include "Keymap.h"

#include <algorithm>

namespace ui {

using engine::Key;
using engine::ModCtrl;

Keymap::Keymap()
{
    define({"edit.selectAll", "Select all", "Editing", {{Key::A, ModCtrl}}});
    define({"edit.copy", "Copy", "Editing", {{Key::C, ModCtrl}}});
    define({"edit.cut", "Cut", "Editing", {{Key::X, ModCtrl}}});
    define({"edit.paste", "Paste", "Editing", {{Key::V, ModCtrl}}, true});
    define({"edit.delete", "Delete", "Editing", {{Key::Delete, 0}, {Key::Backspace, 0}}, true});
}

int Keymap::indexOf(const std::string &id) const
{
    for (size_t i = 0; i < infos_.size(); ++i) {
        if (infos_[i].id == id)
            return static_cast<int>(i);
    }
    return -1;
}

void Keymap::define(ActionInfo info)
{
    const int i = indexOf(info.id);
    if (i >= 0) {
        const bool wasDefault = isDefault(info.id);
        infos_[static_cast<size_t>(i)] = std::move(info);
        if (wasDefault)
            chords_[static_cast<size_t>(i)] = infos_[static_cast<size_t>(i)].defaults;
    } else {
        auto pending = pending_.find(info.id);
        if (pending != pending_.end()) {
            chords_.push_back(pending->second);
            pending_.erase(pending);
        } else {
            chords_.push_back(info.defaults);
        }
        infos_.push_back(std::move(info));
    }
    ++version_;
}

const ActionInfo *Keymap::find(const std::string &id) const
{
    const int i = indexOf(id);
    return i >= 0 ? &infos_[static_cast<size_t>(i)] : nullptr;
}

const std::vector<Chord> &Keymap::chords(const std::string &id) const
{
    static const std::vector<Chord> none;
    const int i = indexOf(id);
    return i >= 0 ? chords_[static_cast<size_t>(i)] : none;
}

std::string Keymap::shortcutText(const std::string &id) const
{
    const auto &c = chords(id);
    return c.empty() ? std::string() : c.front().toString();
}

std::vector<std::string> Keymap::actionsFor(const Chord &chord) const
{
    std::vector<std::string> ids;
    for (size_t i = 0; i < infos_.size(); ++i) {
        if (std::find(chords_[i].begin(), chords_[i].end(), chord) != chords_[i].end())
            ids.push_back(infos_[i].id);
    }
    return ids;
}

void Keymap::setChords(const std::string &id, std::vector<Chord> chords)
{
    const int i = indexOf(id);
    if (i < 0) {
        pending_[id] = std::move(chords);
        return;
    }
    chords_[static_cast<size_t>(i)] = std::move(chords);
    ++version_;
}

std::vector<std::string> Keymap::assign(const std::string &id, const Chord &chord, int replace)
{
    std::vector<std::string> taken;
    const int target = indexOf(id);
    if (target < 0 || chord.empty())
        return taken;
    for (size_t i = 0; i < infos_.size(); ++i) {
        if (static_cast<int>(i) == target)
            continue;
        auto &list = chords_[i];
        const auto before = list.size();
        list.erase(std::remove(list.begin(), list.end(), chord), list.end());
        if (list.size() != before)
            taken.push_back(infos_[i].label);
    }
    auto &list = chords_[static_cast<size_t>(target)];
    if (replace >= 0 && replace < static_cast<int>(list.size()))
        list[static_cast<size_t>(replace)] = chord;
    else if (std::find(list.begin(), list.end(), chord) == list.end())
        list.push_back(chord);
    // The same chord twice in one action, after a replacement
    for (size_t a = 0; a < list.size(); ++a) {
        for (size_t b = list.size(); b-- > a + 1;) {
            if (list[b] == list[a])
                list.erase(list.begin() + static_cast<std::ptrdiff_t>(b));
        }
    }
    ++version_;
    return taken;
}

void Keymap::removeChord(const std::string &id, const Chord &chord)
{
    const int i = indexOf(id);
    if (i < 0)
        return;
    auto &list = chords_[static_cast<size_t>(i)];
    list.erase(std::remove(list.begin(), list.end(), chord), list.end());
    ++version_;
}

void Keymap::reset(const std::string &id)
{
    const int i = indexOf(id);
    if (i < 0)
        return;
    chords_[static_cast<size_t>(i)] = infos_[static_cast<size_t>(i)].defaults;
    ++version_;
}

void Keymap::resetAll()
{
    for (size_t i = 0; i < infos_.size(); ++i)
        chords_[i] = infos_[i].defaults;
    pending_.clear();
    ++version_;
}

bool Keymap::isDefault(const std::string &id) const
{
    const int i = indexOf(id);
    return i < 0 || chords_[static_cast<size_t>(i)] == infos_[static_cast<size_t>(i)].defaults;
}

std::vector<std::pair<std::string, std::vector<Chord>>> Keymap::overrides() const
{
    std::vector<std::pair<std::string, std::vector<Chord>>> out;
    for (size_t i = 0; i < infos_.size(); ++i) {
        if (chords_[i] != infos_[i].defaults)
            out.emplace_back(infos_[i].id, chords_[i]);
    }
    // Saved ones for actions this build hasn't defined (yet) are kept, so they aren't lost
    for (const auto &[id, chords] : pending_)
        out.emplace_back(id, chords);
    return out;
}

void Keymap::setOverrides(const std::vector<std::pair<std::string, std::vector<Chord>>> &overrides)
{
    for (const auto &[id, chords] : overrides)
        setChords(id, chords);
    ++version_;
}

}
