#include "Arrangement.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "Format.h"
#include "Routing.h"
#include "Widgets.h"

namespace daw {

using namespace ui;

namespace {

constexpr float kHeaderWidth = 268.0f;
constexpr float kRulerHeight = 30.0f;
constexpr float kTrackHeight = 76.0f;
constexpr float kMasterHeight = 56.0f; // the master's row, which holds no clips
constexpr float kBarHeight = 10.0f; // the scroll bar under the lanes
constexpr float kGap = 10.0f;
constexpr float kLaneHeight = 56.0f; // an automation lane
constexpr float kAddHeight = 30.0f;  // the strip under a track's lanes that adds one
constexpr float kIndent = 12.0f;     // a header's indent for each group it is inside
constexpr float kLanePad = 7.0f;     // between a lane's edge and its curve's top and bottom

}

// TrackName

TrackName::TrackName(ProjectStore &store, int index)
    : store_(store), index_(index), label_(std::make_unique<Label>(std::string(), TextStyle::Body)),
      field_(std::make_unique<TextField>("Track name"))
{
    field_->setSize(Size::Small);
    field_->onChange([this](const std::string &text) {
        const Track *t = project().track(index_);
        if (t && !text.empty() && t->name != text)
            store_.editMix(index_).name = text;
    });
    field_->onSubmit([this](const std::string &) { endEdit(); });
}

void TrackName::setMaxWidth(float width)
{
    maxWidth_ = width;
    label_->setMaxWidth(width);
}

void TrackName::beginEdit()
{
    const Track *t = project().track(index_);
    if (!t || editing_)
        return;
    before_ = t->name;
    field_->setText(t->name);
    editing_ = true;
    wantFocus_ = true;
    hadFocus_ = false;
}

void TrackName::endEdit()
{
    if (!editing_)
        return;
    editing_ = false;
    const Track *t = project().track(index_);
    if (t && t->name.empty())
        store_.editMix(index_).name = before_;
}

Vec2 TrackName::measure(const Theme &theme) const
{
    const Vec2 text = label_->preferredSize(theme);
    if (!editing_)
        return text;
    return {std::max(text.x, maxWidth_), field_->preferredSize(theme).y};
}

void TrackName::arrange(const Rect &rect, const Theme &theme)
{
    rect_ = rect;
    label_->arrange(rect, theme);
    if (editing_) {
        const float h = field_->preferredSize(theme).y;
        // Exactly the room the name has, so it stays clear of the M/S/R toggles
        const float w = maxWidth_ > 0.0f ? maxWidth_ : rect.w;
        field_->arrange({rect.x - 6.0f, rect.y + (rect.h - h) * 0.5f, w + 6.0f, h}, theme);
    }
}

void TrackName::handleInput(const Input &input)
{
    const Track *t = project().track(index_);
    label_->setText(t ? t->name : std::string());
    if (!editing_) {
        // A double-click on the name renames it
        if (input.pressed && rect_.contains(input.mouse)) {
            if (input.time - lastPress_ < 0.5)
                beginEdit();
            lastPress_ = input.time;
        }
        return;
    }
    field_->handleInput(input);
    if (wantFocus_) {
        field_->focus();
        field_->selectAll();
        wantFocus_ = false;
    } else if (field_->focused()) {
        hadFocus_ = true;
    } else if (hadFocus_) {
        endEdit(); // Escape, or a click elsewhere
    }
}

void TrackName::draw(DrawList &list, const Theme &theme) const
{
    if (editing_)
        field_->draw(list, theme);
    else
        label_->draw(list, theme);
}

// TrackHeader

TrackHeader::TrackHeader(ProjectStore &store, int index, const TransportHooks &hooks, std::function<bool()> automationShown,
                         std::function<void(bool)> showAutomation, std::function<bool()> folded,
                         std::function<void(bool)> fold, std::function<int()> foldedCount)
    : Container(Direction::Row), store_(store), index_(index), chooseInstrument_(hooks.chooseInstrument)
{
    setAlign(Align::Center);
    setPadding(10.0f);
    if (folded && fold) // a group: its chevron where the number would be
        chevron_ = &add<FoldChevron>(std::move(folded), std::move(fold));
    else
        add<Label>(std::to_string(index + 1), TextStyle::Caption, TextTone::Muted).setMinSize({16.0f, 0.0f});
    auto &names = add<ListView>(); // packs its lines tighter than a Container
    names_ = &names;
    names.setAlign(Align::Start);
    title_ = &names.add<TrackName>(store, index);
    auto listening = hooks.trackListening;
    // Above the kind, in the accent colour while the track plays its incoming MIDI
    midi_ = &names.add<LiveLabel>([index, listening] {
        return listening && listening(index) ? std::string("MIDI in") : std::string();
    }, TextStyle::Caption, TextTone::Accent);
    auto input = hooks.trackInput;
    caption_ = &names.add<LiveLabel>([&store, index, input, foldedCount] {
        std::string text;
        if (input) {
            text = input(index);
        } else {
            const Track *t = store.project().track(index);
            text = t ? kindName(t->kind) : "";
        }
        // A folded group says how many tracks it holds
        const int inside = foldedCount ? foldedCount() : 0;
        if (inside > 0)
            text += std::string(text.empty() ? "" : "  /  ") + std::to_string(inside) + (inside == 1 ? " track folded" : " tracks folded");
        return text;
    }, TextStyle::Caption, TextTone::Secondary);
    add<Spacer>();
    auto flag = [&store, index](bool Track::*member) {
        return std::make_pair(
            [&store, index, member] {
                const Track *t = store.project().track(index);
                return t && t->*member;
            },
            [&store, index, member](bool on) {
                if (store.project().track(index))
                    store.editMix(index).*member = on;
            });
    };
    auto [muteGet, muteSet] = flag(&Track::mute);
    auto [soloGet, soloSet] = flag(&Track::solo);
    auto [armGet, armSet] = flag(&Track::armed);
    // The track's toggles folded into one disc that opens them as a radial menu: mute, solo, record
    // arm, its automation lanes shown under its row (A) and, while the song plays, what its controls
    // do written into its automation (W)
    std::vector<RadialToggle> toggles{
        {"M", "Mute", engine::Key::M, muteGet, muteSet},
        {"S", "Solo", engine::Key::S, soloGet, soloSet},
    };
    const Track *self = store.project().track(index);
    if (!(self && self->bus())) // buses record nothing
        toggles.push_back({"R", "Record arm", engine::Key::R, armGet, armSet});
    toggles.push_back({"A", "Automation lanes", engine::Key::A, std::move(automationShown), std::move(showAutomation)});
    if (hooks.automationArmed && hooks.armAutomation) {
        auto armed = hooks.automationArmed;
        auto arm = hooks.armAutomation;
        toggles.push_back({"W", "Write automation", engine::Key::W, [armed, index] { return armed(index); },
                           [arm, index](bool on) { arm(index, on); }});
    }
    flags_ = &add<RadialToggles>(std::move(toggles));
}

void TrackHeader::arrange(const Rect &rect, const Theme &theme)
{
    // The names get the width the number and the toggles' disc leave, and cut themselves short
    // to fit, so the disc stays inside the header
    float others = 0.0f;
    for (const auto &child : children_) {
        if (child.get() != names_)
            others += child->preferredSize(theme).x;
        // (the name and caption are inside names_, which is skipped above)
    }
    // One gap sits between each pair of the row's children
    const float gaps = gap(theme) * static_cast<float>(children_.size() - 1);
    const float room = std::max(0.0f, rect.w - 2.0f * padding(theme) - others - gaps);
    if (title_)
        title_->setMaxWidth(room);
    if (caption_)
        caption_->setMaxWidth(room);
    if (midi_)
        midi_->setMaxWidth(room);
    Container::arrange(rect, theme);
}

void TrackHeader::handleInput(const Input &input)
{
    Container::handleInput(input);
    if (!input.pressed || !rect_.contains(input.mouse))
        return;
    if (flags_ && (flags_->rect().contains(input.mouse) || flags_->menuOpen()))
        return;
    if (chevron_ && chevron_->rect().contains(input.mouse))
        return;
    store_.select(index_);
    // The instrument line of an instrument track: choose its instrument
    const Track *t = project().track(index_);
    if (t && t->kind == TrackKind::Instrument && caption_ && caption_->rect().contains(input.mouse) && chooseInstrument_)
        chooseInstrument_(index_, caption_->rect());
}

bool TrackHeader::canDrag(Vec2 p) const
{
    if (!rect_.contains(p) || title_->editing())
        return false;
    const Track *t = project().track(index_);
    if ((flags_ && flags_->rect().contains(p)) || (chevron_ && chevron_->rect().contains(p)))
        return false;
    return !(t && t->kind == TrackKind::Instrument && caption_ && caption_->rect().contains(p));
}

Rect TrackHeader::chevronRect() const
{
    return chevron_ ? chevron_->rect() : Rect{};
}

void TrackHeader::draw(DrawList &list, const Theme &theme) const
{
    if (project().selectedTrack == index_)
        list.add(theme.surface(rect_, Shape::Soft, Elevation::Inset, 0.6f));
    Container::draw(list, theme);
}

// MasterHeader

MasterHeader::MasterHeader(const TransportHooks &hooks, std::function<bool()> automationShown,
                           std::function<void(bool)> showAutomation)
    : Container(Direction::Row)
{
    setAlign(Align::Center);
    setPadding(10.0f);
    add<Label>(std::string(), TextStyle::Caption).setMinSize({16.0f, 0.0f}); // where a track's number is
    auto &names = add<ListView>();
    names_ = &names;
    names.setAlign(Align::Start);
    names.add<Label>("Master", TextStyle::Body);
    names.add<Label>("Output", TextStyle::Caption, TextTone::Secondary);
    add<Spacer>();
    std::vector<RadialToggle> toggles{
        {"A", "Automation lanes", engine::Key::A, std::move(automationShown), std::move(showAutomation)}};
    if (hooks.masterAutomationArmed && hooks.armMasterAutomation)
        toggles.push_back({"W", "Write automation", engine::Key::W, hooks.masterAutomationArmed, hooks.armMasterAutomation});
    add<RadialToggles>(std::move(toggles));
}

// ArrangementView

ArrangementView::ArrangementView(ProjectStore &store, TransportHooks transport, FoldState *folds)
    : store_(store), transport_(std::move(transport)), ownFolds_(store), folds_(folds ? folds : &ownFolds_)
{
    rebuild();
}

void ArrangementView::rebuild()
{
    shownStructure_ = store_.structureRevision();
    shownShape_ = trackShape(project());
    headers_.clear();
    for (size_t i = 0; i < project().tracks.size(); ++i) {
        const int index = static_cast<int>(i);
        const bool group = project().tracks[i].kind == TrackKind::Group;
        const ObjectId id = project().tracks[i].id;
        headers_.push_back(std::make_unique<TrackHeader>(
            store_, index, transport_, [this, index] { return automationShown(index); },
            [this, index](bool on) { showAutomation(index, on); },
            group ? std::function<bool()>([this, index] { return folded(index); }) : std::function<bool()>(),
            group ? std::function<void(bool)>([this, index](bool on) { fold(index, on); }) : std::function<void(bool)>(),
            group ? std::function<int()>([this, id] { return foldedCount(project().trackIndex(id)); }) : std::function<int()>()));
    }
    masterHeader_ = std::make_unique<MasterHeader>(
        transport_, [this] { return openMaster_; }, [this](bool on) { showMasterAutomation(on); });
    // The headers are new components: a layout must see them
    ui::Component::treeChanged();
}

void ArrangementView::renameTrack(int index)
{
    if (index >= 0 && static_cast<size_t>(index) < headers_.size())
        headers_[static_cast<size_t>(index)]->name().beginEdit();
}

int ArrangementView::trackAt(Vec2 p) const
{
    const bool across = (p.x >= headerArea_.x && p.x < lanes_.x + lanes_.w);
    if (!across || p.y < lanes_.y || p.y >= lanes_.y + lanes_.h)
        return -1;
    const int row = rowAt(p.y);
    return row >= 0 ? row : static_cast<int>(project().tracks.size());
}

void ArrangementView::setDropTarget(int row, std::string hint)
{
    dropRow_ = row;
    dropHint_ = std::move(hint);
}

void ArrangementView::zoom(float factor)
{
    const double left = xBeat(lanes_.x);
    pixelsPerBeat_ = std::clamp(pixelsPerBeat_ * factor, 6.0f, 120.0f);
    scrollX_ = static_cast<float>(left) * pixelsPerBeat_;
    clampScroll();
}

Vec2 ArrangementView::measure(const Theme &) const
{
    return {kHeaderWidth + 400.0f, kRulerHeight + kTrackHeight + kBarHeight + kGap};
}

void ArrangementView::arrange(const Rect &rect, const Theme &theme)
{
    rect_ = rect;
    const float laneHeight = std::max(0.0f, rect.h - kRulerHeight - kBarHeight - kGap);
    headerArea_ = {rect.x, rect.y + kRulerHeight, kHeaderWidth, laneHeight};
    ruler_ = {rect.x + kHeaderWidth + kGap, rect.y, std::max(0.0f, rect.w - kHeaderWidth - kGap), kRulerHeight};
    well_ = {ruler_.x, rect.y + kRulerHeight, ruler_.w, laneHeight};
    lanes_ = well_;
    bar_ = {well_.x, well_.y + well_.h + kGap, well_.w, kBarHeight};
    layoutRows();
    clampScroll();

    // While a track is dragged past the threshold, it follows the mouse and the others make room for
    // it at the row it would land in (the drop preview). Laid out every frame, so this follows the mouse.
    const bool previewing = reorderFrom_ >= 0 && reorderMoved_ && static_cast<size_t>(reorderFrom_) < rows_.size();
    std::vector<float> tops(headers_.size(), 0.0f);
    if (previewing) {
        // The others in order, with the dragged track's height left open at the drop row
        float y = 0.0f;
        int slot = 0;
        for (size_t i = 0; i < headers_.size() && i < rows_.size(); ++i) {
            if (std::find(reorderBlock_.begin(), reorderBlock_.end(), static_cast<int>(i)) != reorderBlock_.end())
                continue;
            if (slot == reorderTo_)
                y += rows_[static_cast<size_t>(reorderFrom_)].height;
            tops[i] = y;
            y += rows_[i].height;
            ++slot;
        }
    }
    for (size_t i = 0; i < headers_.size(); ++i) {
        if (hidden(static_cast<int>(i)))
            continue; // takes no room: never drawn or given input
        float y = (previewing ? lanes_.y + tops[i] - scrollY_ : rowTop(static_cast<int>(i))) + 4.0f;
        if (previewing && static_cast<int>(i) == reorderFrom_)
            y = std::clamp(reorderMouseY_ - reorderGrab_, lanes_.y - kTrackHeight * 0.5f, lanes_.y + lanes_.h - kTrackHeight * 0.5f);
        // Indented by how many groups deep it goes
        const float indent = kIndent * static_cast<float>(depth(static_cast<int>(i)));
        headers_[i]->arrange({headerArea_.x + indent, y, headerArea_.w - indent, kTrackHeight - 8.0f}, theme);
    }
    if (masterHeader_)
        masterHeader_->arrange({headerArea_.x, rowTop(static_cast<int>(project().tracks.size())) + 4.0f, headerArea_.w,
                                kMasterHeight - 8.0f},
                               theme);
}

void ArrangementView::layoutRows()
{
    const size_t count = project().tracks.size();
    rows_.resize(count);
    float top = 0.0f;
    // What folding works out (which tracks are hidden, how deep, how many a folded group holds) only
    // changes with the project (the outputs are in it) or the fold state, so it is done again only then
    if (foldedFor_ != store_.revision() || foldedVersion_ != folds_->version() || hidden_.size() != count) {
        foldedFor_ = store_.revision();
        foldedVersion_ = folds_->version();
        hidden_ = hiddenTracks(project());
        depth_ = groupDepth(project());
        foldedCounts_.assign(count, 0);
        // For each folded group, the tracks inside it (a track in two folded groups counts in both)
        for (size_t i = 0; i < count; ++i) {
            for (int g : outputGroups(project(), static_cast<int>(i))) {
                if (project().tracks[static_cast<size_t>(g)].folded)
                    ++foldedCounts_[static_cast<size_t>(g)];
            }
        }
    }
    for (size_t i = 0; i < count; ++i) {
        Row &row = rows_[i];
        row.top = top;
        if (hidden_[i]) { // inside a folded group: no height, and no lanes
            row.open = false;
            row.lanes.clear();
            row.height = 0.0f;
            continue;
        }
        row.open = openAutomation_.count(project().tracks[i].id) > 0;
        row.lanes = row.open ? automationRows(project(), static_cast<int>(i)) : std::vector<AutomationRow>{};
        row.height = kTrackHeight + (row.open ? static_cast<float>(row.lanes.size()) * kLaneHeight + kAddHeight : 0.0f);
        top += row.height;
    }
    master_.top = top;
    master_.open = openMaster_;
    master_.lanes = openMaster_ ? masterAutomationRows(project()) : std::vector<AutomationRow>{};
    master_.height = kMasterHeight + (openMaster_ ? static_cast<float>(master_.lanes.size()) * kLaneHeight + kAddHeight : 0.0f);
}

const ArrangementView::Row *ArrangementView::rowOf(int track) const
{
    if (isMaster(track))
        return &master_;
    return track >= 0 && static_cast<size_t>(track) < rows_.size() ? &rows_[static_cast<size_t>(track)] : nullptr;
}

float ArrangementView::headHeight(int track) const
{
    return isMaster(track) ? kMasterHeight : kTrackHeight;
}

int ArrangementView::laneRowAt(float y) const
{
    const int row = rowAt(y);
    if (row >= 0)
        return row;
    const float offset = y - lanes_.y + scrollY_;
    return offset >= master_.top && offset < master_.top + master_.height ? static_cast<int>(project().tracks.size()) : -1;
}

float ArrangementView::rowTop(int track) const
{
    if (track < 0)
        return lanes_.y - scrollY_;
    // (the master's row follows the last track: the place a drop below the tracks highlights)
    const size_t i = static_cast<size_t>(track);
    const float top = i < rows_.size() ? rows_[i].top : master_.top + static_cast<float>(i - rows_.size()) * kTrackHeight;
    return lanes_.y + top - scrollY_;
}

float ArrangementView::rowHeight(int track) const
{
    if (isMaster(track))
        return master_.height;
    return track >= 0 && static_cast<size_t>(track) < rows_.size() ? rows_[static_cast<size_t>(track)].height : kTrackHeight;
}

float ArrangementView::rowsHeight() const
{
    return master_.top + master_.height;
}

bool ArrangementView::automationShown(int track) const
{
    const Track *t = project().track(track);
    return t && openAutomation_.count(t->id) > 0;
}

void ArrangementView::showAutomation(int track, bool shown)
{
    const Track *t = project().track(track);
    if (!t)
        return;
    if (shown)
        openAutomation_.insert(t->id);
    else
        openAutomation_.erase(t->id);
    layoutRows();
    clampScroll();
}

bool ArrangementView::folded(int track) const
{
    const Track *t = project().track(track);
    return t && t->kind == TrackKind::Group && t->folded;
}

void ArrangementView::fold(int track, bool fold)
{
    const Track *t = project().track(track);
    if (!t || t->kind != TrackKind::Group)
        return;
    folds_->set(t->id, fold);
    layoutRows();
    clampScroll();
}

void ArrangementView::foldAll()
{
    for (const Track &t : project().tracks) {
        if (t.kind == TrackKind::Group)
            folds_->set(t.id, true);
    }
    layoutRows();
    clampScroll();
}

void ArrangementView::unfoldAll()
{
    folds_->clear();
    layoutRows();
    clampScroll();
}

int ArrangementView::depth(int track) const
{
    return track >= 0 && static_cast<size_t>(track) < depth_.size() ? depth_[static_cast<size_t>(track)] : 0;
}

int ArrangementView::foldedCount(int track) const
{
    return track >= 0 && static_cast<size_t>(track) < foldedCounts_.size() ? foldedCounts_[static_cast<size_t>(track)] : 0;
}

const TrackHeader *ArrangementView::header(int track) const
{
    return !hidden(track) && track >= 0 && static_cast<size_t>(track) < headers_.size() ? headers_[static_cast<size_t>(track)].get()
                                                                                         : nullptr;
}

bool ArrangementView::hidden(int track) const
{
    return track >= 0 && static_cast<size_t>(track) < hidden_.size() && hidden_[static_cast<size_t>(track)];
}

void ArrangementView::showMasterAutomation(bool shown)
{
    openMaster_ = shown;
    layoutRows();
    clampScroll();
}

float ArrangementView::beatX(double beat) const
{
    return lanes_.x + static_cast<float>(beat) * pixelsPerBeat_ - scrollX_;
}

double ArrangementView::xBeat(float x) const
{
    return static_cast<double>(x - lanes_.x + scrollX_) / pixelsPerBeat_;
}

int ArrangementView::rowAt(float y) const
{
    const float offset = y - lanes_.y + scrollY_;
    if (offset < 0.0f)
        return -1;
    const size_t count = std::min(rows_.size(), project().tracks.size());
    for (size_t i = 0; i < count; ++i) {
        if (!hidden(static_cast<int>(i)) && offset < rows_[i].top + rows_[i].height)
            return static_cast<int>(i);
    }
    return -1;
}

Rect ArrangementView::clipRect(int track, const Clip &clip) const
{
    return {beatX(clip.start) + 1.0f, rowTop(track) + 6.0f, static_cast<float>(clip.length) * pixelsPerBeat_ - 2.0f,
            kTrackHeight - 12.0f};
}

float ArrangementView::contentWidth() const
{
    return static_cast<float>(project().lengthBeats()) * pixelsPerBeat_;
}

Rect ArrangementView::thumb() const
{
    const float width = contentWidth();
    const float maxX = std::max(0.0f, width - lanes_.w);
    const float share = width > 0.0f ? std::min(1.0f, lanes_.w / width) : 1.0f;
    const float w = std::max(40.0f, bar_.w * share);
    const float x = bar_.x + (maxX > 0.0f ? scrollX_ / maxX * (bar_.w - w) : 0.0f);
    return {x, bar_.y, w, bar_.h};
}

void ArrangementView::clampScroll()
{
    const float maxX = std::max(0.0f, contentWidth() - lanes_.w);
    const float maxY = std::max(0.0f, rowsHeight() - lanes_.h);
    scrollX_ = std::clamp(scrollX_, 0.0f, maxX);
    scrollY_ = std::clamp(scrollY_, 0.0f, maxY);
}

void ArrangementView::handleInput(const Input &input)
{
    // A selection that lands on a hidden track (the mixer, the keyboard, an undo) unfolds the groups
    // hiding it, once per change of the selection (FoldState keeps that, so the mixer can do it too)
    if (folds_->revealSelection()) {
        layoutRows();
        clampScroll();
    }

    // The headers aren't tree children, so they're rebuilt here. Not while a name is being typed:
    // its field holds the keyboard focus, which a new header would leave dangling; the rebuild waits
    bool renaming = false;
    for (const auto &header : headers_)
        renaming = renaming || header->name().editing();
    // Any edit() moves the structure; only a change in the tracks themselves (trackShape) rebuilds
    if (!renaming && store_.structureRevision() != shownStructure_) {
        if (trackShape(project()) != shownShape_)
            rebuild();
        else
            shownStructure_ = store_.structureRevision();
    }

    const size_t count = std::min(headers_.size(), project().tracks.size());
    const Input headerInput = headerArea_.contains(input.mouse) ? input : input.occluded();
    for (size_t i = 0; i < count; ++i) {
        if (hidden(static_cast<int>(i)))
            continue;
        // A press anywhere but on the name being edited finishes it
        if (input.pressed && headers_[i]->name().editing() && !headers_[i]->name().hitTest(input.mouse))
            headers_[i]->name().endEdit();
        headers_[i]->handleInput(headerInput);
    }
    if (masterHeader_)
        masterHeader_->handleInput(headerInput);

    if (rect_.contains(input.mouse)) {
        float across = input.scrollX;
        float down = input.scroll;
        // Ctrl-wheel zooms in time around the mouse
        if ((input.mods & engine::ModCtrl) && down != 0.0f) {
            const float x = std::clamp(input.mouse.x, lanes_.x, lanes_.x + lanes_.w);
            const double at = xBeat(x);
            pixelsPerBeat_ = std::clamp(pixelsPerBeat_ * std::pow(1.15f, down), 6.0f, 120.0f);
            scrollX_ = static_cast<float>(at) * pixelsPerBeat_ - (x - lanes_.x);
            down = 0.0f;
            input.scroll = 0.0f;
        }
        // Middle-drag pans
        if (input.middlePressed) {
            input.middlePressed = false;
            panning_ = true;
            panMouse_ = input.mouse;
            panX_ = scrollX_;
            panY_ = scrollY_;
        }
        if (input.shift && across == 0.0f)
            std::swap(across, down);
        if (down != 0.0f) {
            scrollY_ -= down * 40.0f;
            input.scroll = 0.0f;
        }
        if (across != 0.0f) {
            scrollX_ -= across * 60.0f;
            input.scrollX = 0.0f;
        }
        clampScroll();
    }

    if (panning_) {
        if (input.middleDown) {
            scrollX_ = panX_ - (input.mouse.x - panMouse_.x);
            scrollY_ = panY_ - (input.mouse.y - panMouse_.y);
            clampScroll();
        } else {
            panning_ = false;
        }
    }

    // Dragging a track's header up or down moves the track to the row the mouse is over
    if (input.pressed && reorderFrom_ < 0 && headerArea_.contains(input.mouse)) {
        const int row = rowAt(input.mouse.y);
        if (row >= 0 && static_cast<size_t>(row) < headers_.size() && headers_[static_cast<size_t>(row)]->canDrag(input.mouse)) {
            reorderFrom_ = reorderTo_ = row;
            reorderBlock_ = dragBlock(project(), row);
            reorderPressY_ = reorderMouseY_ = input.mouse.y;
            reorderGrab_ = input.mouse.y - (rowTop(row) + 4.0f);
            reorderMoved_ = false;
        }
    }
    if (reorderFrom_ >= 0) {
        reorderMoved_ = reorderMoved_ || std::fabs(input.mouse.y - reorderPressY_) > 4.0f;
        reorderMouseY_ = input.mouse.y;
        // The drop row: how many of the other tracks the dragged header's middle is past the middle of
        const float middle = input.mouse.y - reorderGrab_ + kTrackHeight * 0.5f;
        int passed = 0;
        for (size_t i = 0; i < rows_.size(); ++i) {
            // (a hidden row has no height, so it is passed along with the row before it; what moves
            // with the dragged track isn't counted)
            if (std::find(reorderBlock_.begin(), reorderBlock_.end(), static_cast<int>(i)) == reorderBlock_.end() &&
                middle > rowTop(static_cast<int>(i)) + rows_[i].height * 0.5f)
                ++passed;
        }
        // Never between a folded group and its hidden tracks, or among them
        reorderTo_ = settleDrop(project(), reorderBlock_, passed);
        input.setCursor(engine::Cursor::Hand);
        if (!input.down) {
            if (reorderMoved_) {
                const std::vector<int> order = dropOrder(project(), reorderBlock_, reorderTo_);
                bool moves = false;
                for (size_t i = 0; i < order.size(); ++i)
                    moves = moves || order[i] != static_cast<int>(i);
                if (moves) {
                    moveBlock(store_.edit(), reorderBlock_, reorderTo_);
                    rebuild(); // the headers follow the new order from this frame on
                }
            }
            reorderFrom_ = reorderTo_ = -1;
            reorderBlock_.clear();
        }
    }

    // Automation lanes and their headers take the input over them
    const bool inLanes = handleLaneInput(input);

    // What's under the mouse in the lanes
    hoveredTrack_ = -1;
    hoveredClip_ = -1;
    if (well_.contains(input.mouse) && !inLanes) {
        hoveredTrack_ = rowAt(input.mouse.y);
        if (const Track *t = project().track(hoveredTrack_)) {
            for (int i = static_cast<int>(t->clips.size()) - 1; i >= 0; --i) {
                if (clipRect(hoveredTrack_, t->clips[static_cast<size_t>(i)]).contains(input.mouse)) {
                    hoveredClip_ = i;
                    break;
                }
            }
        }
    }

    // Near a clip's ends, dragging trims it
    hoverEdge_ = 0;
    if (hoveredClip_ >= 0) {
        const Rect r = clipRect(hoveredTrack_, project().tracks[static_cast<size_t>(hoveredTrack_)].clips[static_cast<size_t>(hoveredClip_)]);
        const float edge = std::min(8.0f, r.w * 0.25f);
        if (input.mouse.x <= r.x + edge)
            hoverEdge_ = -1;
        else if (input.mouse.x >= r.x + r.w - edge)
            hoverEdge_ = 1;
    }

    // Right-click: the clip's menu, or the track's
    if (input.secondaryPressed) {
        if (hoveredClip_ >= 0) {
            input.secondaryPressed = false;
            store_.select(hoveredTrack_, hoveredClip_);
            if (transport_.clipMenu)
                transport_.clipMenu(input.mouse, xBeat(input.mouse.x));
        } else if (hoveredTrack_ >= 0) {
            input.secondaryPressed = false;
            store_.select(hoveredTrack_);
            if (transport_.trackMenu)
                transport_.trackMenu(hoveredTrack_, input.mouse);
        } else if (headerArea_.contains(input.mouse)) {
            const int row = rowAt(input.mouse.y);
            if (row >= 0) {
                input.secondaryPressed = false;
                store_.select(row);
                if (transport_.trackMenu)
                    transport_.trackMenu(row, input.mouse);
            }
        }
    }

    // The loop strip: the pointer says what a press does there
    const Rect band = loopBand();
    const float loopX0 = beatX(project().loopStart), loopX1 = beatX(project().loopEnd);
    const bool onEdge = project().hasLoop() && (std::fabs(input.mouse.x - loopX0) < 6.0f || std::fabs(input.mouse.x - loopX1) < 6.0f);
    if (loopDrag_ != 0 || (band.contains(input.mouse) && onEdge))
        input.setCursor(engine::Cursor::ResizeX);
    else if (band.contains(input.mouse) || ruler_.contains(input.mouse))
        input.setCursor(engine::Cursor::Hand);

    if (input.pressed && !lanePress_) {
        if (band.contains(input.mouse)) {
            loopPress_ = xBeat(input.mouse.x);
            loopPressX_ = input.mouse.x;
            loopMoved_ = false;
            if (project().hasLoop() && std::fabs(input.mouse.x - loopX0) < 6.0f)
                loopDrag_ = 2;
            else if (project().hasLoop() && std::fabs(input.mouse.x - loopX1) < 6.0f)
                loopDrag_ = 3;
            else
                loopDrag_ = 1;
        } else if (ruler_.contains(input.mouse)) {
            scrubbing_ = true;
        } else if (bar_.contains(input.mouse)) {
            const Rect t = thumb();
            grabX_ = t.contains(input.mouse) ? input.mouse.x - t.x : t.w * 0.5f;
            draggingBar_ = true;
        } else if (well_.contains(input.mouse)) {
            const bool doubleClick = lastPress_ >= 0.0 && input.time - lastPress_ < 0.3;
            lastPress_ = input.time;
            if (hoveredClip_ >= 0) {
                store_.select(hoveredTrack_, hoveredClip_);
                grabBeat_ = xBeat(input.mouse.x) - project().clip()->start;
                trimming_ = doubleClick ? 0 : hoverEdge_;
                draggingClip_ = !doubleClick && hoverEdge_ == 0;
                if (doubleClick && transport_.openClip)
                    transport_.openClip();
            } else if (const Track *t = project().track(hoveredTrack_)) {
                if (doubleClick && t->kind == TrackKind::Instrument) {
                    // A one-bar clip in the bar that was clicked, if it fits on the timeline
                    const double bar = project().beatsPerBar;
                    const double start = std::floor(xBeat(input.mouse.x) / bar) * bar;
                    if (start >= 0.0 && start + bar <= project().lengthBeats()) {
                        store_.editTrack(hoveredTrack_).clips.push_back({"Clip", start, bar, {}});
                        store_.select(hoveredTrack_,
                                      static_cast<int>(project().tracks[static_cast<size_t>(hoveredTrack_)].clips.size()) - 1);
                    }
                } else {
                    store_.select(hoveredTrack_);
                }
            }
        }
    }
    if (loopDrag_ != 0) {
        loopMoved_ = loopMoved_ || std::fabs(input.mouse.x - loopPressX_) > 4.0f;
        const double at = loopSnap(xBeat(input.mouse.x), input.shift);
        // The loop's new span and looping are worked out here and written once, below
        double start = project().loopStart, end = project().loopEnd;
        bool looping = project().looping;
        if (loopDrag_ == 1 && loopMoved_) {
            const double from = loopSnap(loopPress_, input.shift);
            start = std::min(from, at);
            end = std::max(from, at);
        } else if (loopDrag_ == 2) {
            start = std::min(at, end - (input.shift ? 1.0 : project().beatsPerBar));
        } else if (loopDrag_ == 3) {
            end = std::max(at, start + (input.shift ? 1.0 : project().beatsPerBar));
        }
        start = std::max(0.0, start);
        if (!input.down) {
            if (loopDrag_ == 1 && !loopMoved_) {
                // A click: in the loop turns looping on or off; elsewhere the loop goes
                if (end > start && loopPress_ >= start && loopPress_ <= end)
                    looping = !looping;
                else {
                    start = end = 0.0;
                    looping = false;
                }
            } else if (end > start) {
                looping = true;
            }
            loopDrag_ = 0;
        }
        store_.setLoop(start, end, looping);
    }
    if (!input.down) {
        scrubbing_ = draggingClip_ = draggingBar_ = false;
        trimming_ = 0;
    }

    if (trimming_ != 0) {
        if (const Clip *clip = project().clip()) {
            // Snapped to beats unless Shift is held
            double beat = std::clamp(xBeat(input.mouse.x), 0.0, project().lengthBeats());
            if (!input.shift)
                beat = std::round(beat);
            if (trimming_ < 0) {
                project().trimStart(store_.editClip(project().selectedTrack, project().selectedClip), beat, 0.25);
            } else {
                double end = std::max(beat, clip->start + 0.25);
                // An audio clip ends with its sound file
                const audio::AudioData *data = transport_.audioFor
                                                   ? transport_.audioFor(project().selectedTrack, project().selectedClip)
                                                   : nullptr;
                if (clip->hasAudio() && data)
                    end = std::min(end, clip->start + (data->seconds() - clip->offset) / project().secondsPerBeat());
                store_.editClip(project().selectedTrack, project().selectedClip).length = std::max(0.25, end - clip->start);
            }
        } else {
            trimming_ = 0;
        }
    }

    if (scrubbing_ && transport_.seek)
        transport_.seek(std::clamp(xBeat(input.mouse.x), 0.0, project().lengthBeats()));

    if (draggingBar_) {
        const Rect t = thumb();
        const float maxX = std::max(0.0f, contentWidth() - lanes_.w);
        const float travel = std::max(1.0f, bar_.w - t.w);
        scrollX_ = (input.mouse.x - grabX_ - bar_.x) / travel * maxX;
        clampScroll();
    }

    if (draggingClip_) {
        if (const Clip *clip = project().clip()) {
            // Along the lane, snapped to beats
            const double start = std::round(xBeat(input.mouse.x) - grabBeat_);
            const double length = clip->length;
            store_.editClip(project().selectedTrack, project().selectedClip).start =
                std::clamp(start, 0.0, std::max(0.0, project().lengthBeats() - length));
            // Onto another track of its kind (notes onto an instrument track, audio onto an audio track)
            const float y = std::clamp(input.mouse.y, well_.y, well_.y + well_.h - 1.0f);
            const int row = rowAt(y);
            const Track *onto = project().track(row);
            const Track *from = project().track(project().selectedTrack);
            if (row >= 0 && row != project().selectedTrack && onto && from && onto->kind == from->kind) {
                const Clip moved = *clip;
                store_.edit().deleteClip();
                auto &clips = store_.edit().tracks[static_cast<size_t>(row)].clips;
                clips.push_back(moved);
                store_.select(row, static_cast<int>(clips.size()) - 1);
            }
        } else {
            draggingClip_ = false;
        }
    }

    // Keep the playhead in view while playing
    if (follow_ && !scrubbing_ && !draggingBar_ && transport_.playing && transport_.playing() && transport_.position) {
        const float x = beatX(transport_.position());
        if (x > lanes_.x + lanes_.w - 24.0f || x < lanes_.x) {
            scrollX_ = static_cast<float>(transport_.position()) * pixelsPerBeat_ - lanes_.w * 0.1f;
            clampScroll();
        }
    }
}

void ArrangementView::drawClip(DrawList &list, const Theme &theme, int track, int index) const
{
    const Track &t = project().tracks[static_cast<size_t>(track)];
    const Clip &clip = t.clips[static_cast<size_t>(index)];
    const Rect r = clipRect(track, clip);
    if (!clipShown(r))
        return;

    const bool selected = track == project().selectedTrack && index == project().selectedClip;
    const bool hovered = track == hoveredTrack_ && index == hoveredClip_;
    const float depth = t.mute ? 0.25f : selected ? 0.9f : hovered ? 0.75f : 0.55f;
    list.add(theme.surface(r, Shape::Soft, Elevation::Raised, depth, Finish::Convex));

    list.pushClip(r);
    const float px = theme.fontSize(TextStyle::Caption);
    float textX = r.x + 10.0f;
    if (selected) {
        list.add(theme.accent({textX, r.y + 6.0f + px * 0.5f - 3.0f, 6.0f, 6.0f}, Shape::Circle));
        textX += 12.0f;
    }
    const TextTone tone = t.mute ? TextTone::Muted : selected ? TextTone::Accent : TextTone::Primary;
    list.add(theme.text(clip.name, {textX, r.y + 6.0f, r.w, px}, TextStyle::Caption, tone, TextAlign::Start));

    // Audio clips show their waveform (the take so far, while recording); instrument clips their
    // notes
    const Rect body{r.x + 8.0f, r.y + px + 14.0f, r.w - 16.0f, r.h - px - 20.0f};
    const float visibleStart = std::max(r.x, well_.x);
    const float visibleEnd = std::min(r.x + r.w, well_.x + well_.w);
    const float opacity = t.mute ? 0.25f : 0.5f;
    const bool recording = transport_.isRecording && transport_.isRecording(track, index);
    if (recording)
        list.add(theme.accent({r.x + r.w - 16.0f, r.y + 8.0f, 8.0f, 8.0f}, Shape::Circle));
    if (t.kind == TrackKind::Audio) {
        const audio::AudioData *data = transport_.audioFor ? transport_.audioFor(track, index) : nullptr;
        const float cy = body.y + body.h * 0.5f;
        const float half = body.h * 0.5f + 2.0f;
        if (data) {
            const double spb = project().secondsPerBeat();
            // The waveform's columns, each a low and a high value (after gain, clamped), one plot
            const float firstX = std::max(r.x, visibleStart);
            std::vector<float> pairs;
            pairs.reserve(static_cast<size_t>(std::max(0.0f, visibleEnd - firstX) / 2.0f + 1.0f) * 2);
            uint32_t columns = 0;
            for (float x = firstX; x < visibleEnd; x += 2.0f) {
                const double b0 = (x - r.x) / pixelsPerBeat_;
                const double b1 = (x + 2.0f - r.x) / pixelsPerBeat_;
                if (b0 >= clip.length)
                    break;
                float low = 0.0f, high = 0.0f;
                data->range(clip.offset + b0 * spb, clip.offset + b1 * spb, low, high);
                pairs.push_back(std::max(-1.0f, low * clip.gain));
                pairs.push_back(std::min(1.0f, high * clip.gain));
                ++columns;
            }
            if (columns > 0) {
                ui::Plot wave = theme.inkPlot({firstX, cy - half, 2.0f * static_cast<float>(columns), 2.0f * half},
                                              ui::PlotMode::Range, TextTone::Secondary, opacity + 0.15f);
                wave.low = -1.0f;
                wave.high = 1.0f;
                wave.barFraction = 0.75f;
                wave.values = pairs.data();
                wave.count = columns;
                list.add(wave);
            }
            // Fades, as a line from silence to full
            auto fade = [&](double from, double beats, bool in) {
                const int steps = std::max(2, static_cast<int>(beats * pixelsPerBeat_ / 5.0));
                // The levels at evenly spaced x (the first at the fade's start, the last at its end)
                std::vector<float> levels;
                levels.reserve(static_cast<size_t>(steps) + 1);
                float firstX = 0.0f, lastX = 0.0f;
                for (int k = 0; k <= steps; ++k) {
                    const double f = static_cast<double>(k) / steps;
                    const double level = in ? std::sin(f * 1.5707963) : std::cos(f * 1.5707963);
                    const float x = r.x + static_cast<float>((from + f * beats) * pixelsPerBeat_);
                    if (k == 0)
                        firstX = x;
                    lastX = x;
                    levels.push_back(static_cast<float>(level));
                }
                if (levels.size() < 2)
                    return;
                ui::Plot curve = theme.accentPlot({firstX, body.y, lastX - firstX, body.h}, ui::PlotMode::Line, 0.7f);
                curve.values = levels.data();
                curve.count = static_cast<uint32_t>(levels.size());
                curve.low = 0.0f;
                curve.high = 1.0f;
                curve.width = 1.5f;
                list.add(curve);
            };
            if (clip.fadeIn > 0.0)
                fade(0.0, clip.fadeIn / spb, true);
            if (clip.fadeOut > 0.0)
                fade(clip.length - clip.fadeOut / spb, clip.fadeOut / spb, false);
        } else {
            const std::string note = clip.hasAudio() ? "Loading..." : recording ? "Waiting for input..." : "Empty";
            list.add(theme.text(note, body, TextStyle::Caption, TextTone::Muted, TextAlign::Start));
        }
    } else if (!clip.notes.empty()) {
        // The notes' pitch range fills the clip's height (at least an octave)
        int low = 127, high = 0;
        for (const Note &n : clip.notes) {
            low = std::min(low, n.key);
            high = std::max(high, n.key);
        }
        if (high - low < 12) {
            const int pad = (12 - (high - low)) / 2;
            low = std::max(0, low - pad);
            high = std::min(127, low + 12);
        }
        const float rowHeight = body.h / static_cast<float>(high - low + 1);
        for (const Note &n : clip.notes) {
            if (n.start >= clip.length)
                continue;
            const float x = body.x - 8.0f + static_cast<float>(n.start) * pixelsPerBeat_;
            const float w = std::max(2.0f, static_cast<float>(std::min(n.length, clip.length - n.start)) * pixelsPerBeat_ - 1.0f);
            if (x + w < visibleStart || x > visibleEnd)
                continue;
            const float y = body.y + static_cast<float>(high - n.key) * rowHeight;
            // Muted notes faint, since they don't play
            list.add(theme.ink({x, y, w, std::max(2.0f, rowHeight - 1.0f)}, Shape::Pill, TextTone::Secondary, true,
                               n.muted ? opacity * 0.35f : opacity + 0.15f));
        }
    }
    // The edge being trimmed (or about to be)
    if (track == hoveredTrack_ && index == hoveredClip_ && (hoverEdge_ != 0 || trimming_ != 0)) {
        const int edge = trimming_ != 0 ? trimming_ : hoverEdge_;
        list.add(theme.accent({edge < 0 ? r.x : r.x + r.w - 3.0f, r.y + 4.0f, 3.0f, r.h - 8.0f}, Shape::Pill, 0.8f));
    } else if (selected && trimming_ != 0) {
        list.add(theme.accent({trimming_ < 0 ? r.x : r.x + r.w - 3.0f, r.y + 4.0f, 3.0f, r.h - 8.0f}, Shape::Pill, 0.8f));
    }
    list.popClip();
}

bool ArrangementView::clipShown(const Rect &r) const
{
    return !(r.x > well_.x + well_.w || r.x + r.w < well_.x || r.y > well_.y + well_.h || r.y + r.h < well_.y);
}

bool ArrangementView::anyRecording() const
{
    if (!transport_.isRecording)
        return false;
    for (size_t t = 0; t < project().tracks.size(); ++t) {
        const size_t clips = project().tracks[t].clips.size();
        for (size_t c = 0; c < clips; ++c) {
            if (transport_.isRecording(static_cast<int>(t), static_cast<int>(c)))
                return true;
        }
    }
    return false;
}

uint64_t ArrangementView::contentKey(bool recording) const
{
    Fingerprint fp;
    // The project (every edit moves revision) and the selection
    fp.add(store_.revision());
    fp.add(store_.selectionRevision());
    // Layout and view
    fp.add(rect_);
    fp.add(headerArea_);
    fp.add(ruler_);
    fp.add(well_);
    fp.add(lanes_);
    fp.add(bar_);
    fp.add(scrollX_);
    fp.add(scrollY_);
    fp.add(pixelsPerBeat_);
    // Hover, trimming, the loop strip's drag and the scroll bar's drag
    fp.add(hoveredTrack_);
    fp.add(hoveredClip_);
    fp.add(hoverEdge_);
    fp.add(trimming_);
    fp.add(loopDrag_);
    fp.add(draggingBar_);
    // The automation lanes: which are shown, and the point under the mouse
    fp.add(rowsHeight());
    for (const Row &row : rows_)
        fp.add(row.open);
    fp.add(master_.open);
    fp.add(folds_->version());
    for (char h : hidden_) // the folded groups' tracks: their rows, clips and lanes aren't drawn
        fp.add(h != 0);
    fp.add(laneTrack_);
    fp.add(laneIndex_);
    fp.add(lanePoint_);
    fp.add(draggingPoint_);
    // A take grows every frame while recording (the cache doesn't reuse then); the flag also makes
    // the first frame after a take repaint
    fp.add(recording);
    // The audio of the visible audio clips arrives later (loading) and can be let go: its address
    for (size_t t = 0; t < project().tracks.size(); ++t) {
        const Track &track = project().tracks[t];
        if (track.kind != TrackKind::Audio || hidden(static_cast<int>(t)))
            continue;
        for (size_t c = 0; c < track.clips.size(); ++c) {
            if (!clipShown(clipRect(static_cast<int>(t), track.clips[c])))
                continue;
            const audio::AudioData *data =
                transport_.audioFor ? transport_.audioFor(static_cast<int>(t), static_cast<int>(c)) : nullptr;
            fp.add(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(data)));
        }
    }
    return fp.value();
}

void ArrangementView::drawContent(DrawList &list, const Theme &theme) const
{
    const int beatsPerBar = std::max(1, project().beatsPerBar);
    const float barPixels = pixelsPerBeat_ * static_cast<float>(beatsPerBar);
    int labelEvery = 1;
    while (barPixels * static_cast<float>(labelEvery) < 44.0f)
        labelEvery *= 2;
    const int firstBar = std::max(0, static_cast<int>(scrollX_ / barPixels));
    const int lastBar = std::min(project().bars, static_cast<int>((scrollX_ + lanes_.w) / barPixels) + 1);

    // Ruler: the loop strip (an inset groove, the loop a raised accent band in it), bar numbers
    // and ticks
    const Rect band = loopBand();
    list.add(theme.surface(band, Shape::Pill, Elevation::Inset, 0.4f));
    list.pushClip(ruler_);
    if (project().hasLoop()) {
        const float x0 = beatX(project().loopStart), x1 = beatX(project().loopEnd);
        const Rect loop{x0, band.y + 1.0f, std::max(4.0f, x1 - x0), band.h - 2.0f};
        list.add(theme.surface(loop, Shape::Pill, Elevation::Raised, 0.45f, Finish::Convex));
        list.add(theme.accent(loop, Shape::Pill, project().looping ? 0.85f : 0.3f));
    }
    const float rulerBottom = ruler_.y + ruler_.h;
    for (int bar = firstBar; bar <= lastBar; ++bar) {
        const float x = beatX(static_cast<double>(bar) * beatsPerBar);
        list.add(theme.ink({x, rulerBottom - 12.0f, 1.0f, 10.0f}, Shape::Rounded, TextTone::Muted, true, 0.8f));
        if (bar % labelEvery == 0 && bar < project().bars)
            list.add(theme.text(std::to_string(bar + 1), {x + 5.0f, band.y + band.h, 40.0f, rulerBottom - band.y - band.h - 3.0f}, TextStyle::Caption,
                                TextTone::Secondary, TextAlign::Start));
        if (pixelsPerBeat_ >= 10.0f && bar < project().bars) {
            for (int b = 1; b < beatsPerBar; ++b)
                list.add(theme.ink({x + static_cast<float>(b) * pixelsPerBeat_, rulerBottom - 7.0f, 1.0f, 5.0f},
                                   Shape::Rounded, TextTone::Muted, true, 0.6f));
        }
    }
    list.popClip();

    // The lanes, in one well
    list.add(theme.surface(well_, Shape::Soft, Elevation::Inset, 0.6f));
    list.pushClip(well_.inset(2.0f));
    for (int bar = firstBar; bar <= lastBar; ++bar) {
        const float x = beatX(static_cast<double>(bar) * beatsPerBar);
        list.add(theme.ink({x, well_.y, 1.0f, well_.h}, Shape::Rounded, TextTone::Muted, true, 0.28f));
        if (pixelsPerBeat_ >= 14.0f && bar < project().bars) {
            for (int b = 1; b < beatsPerBar; ++b)
                list.add(theme.ink({x + static_cast<float>(b) * pixelsPerBeat_, well_.y, 1.0f, well_.h}, Shape::Rounded,
                                   TextTone::Muted, true, 0.1f));
        }
    }
    const int tracks = static_cast<int>(project().tracks.size());
    for (int i = 0; i < tracks; ++i) {
        if (hidden(i))
            continue;
        const float y = rowTop(i) + rowHeight(i);
        list.add(theme.ink({well_.x, y, well_.w, 1.0f}, Shape::Rounded, TextTone::Muted, true, 0.25f));
    }
    list.add(theme.ink({well_.x, rowTop(tracks) + rowHeight(tracks), well_.w, 1.0f}, Shape::Rounded, TextTone::Muted, true, 0.25f));
    drawLanes(list, theme);
    // A folded group's row shows where the tracks folded into it have clips: the spans of all their
    // clips, merged, as blocks in the accent with a thin edge, centred in the row
    bool anyFolded = false;
    for (int g = 0; g < tracks && !anyFolded; ++g)
        anyFolded = foldedCount(g) > 0;
    if (anyFolded) {
        std::vector<std::vector<std::pair<double, double>>> spans(static_cast<size_t>(tracks));
        for (int t = 0; t < tracks; ++t) {
            if (!hidden(t))
                continue;
            for (int g : outputGroups(project(), t)) {
                if (foldedCount(g) == 0)
                    continue;
                for (const Clip &clip : project().tracks[static_cast<size_t>(t)].clips)
                    spans[static_cast<size_t>(g)].push_back({clip.start, clip.start + clip.length});
            }
        }
        const float blockHeight = 30.0f;
        for (int g = 0; g < tracks; ++g) {
            auto &s = spans[static_cast<size_t>(g)];
            const float y = rowTop(g) + (kTrackHeight - blockHeight) * 0.5f;
            if (s.empty() || hidden(g) || y > well_.y + well_.h || y + blockHeight < well_.y)
                continue;
            std::sort(s.begin(), s.end());
            for (size_t i = 0; i < s.size();) {
                double end = s[i].second;
                size_t j = i + 1;
                while (j < s.size() && s[j].first <= end)
                    end = std::max(end, s[j++].second);
                const float x0 = beatX(s[i].first) + 1.0f, x1 = beatX(end) - 1.0f;
                i = j;
                if (x1 < well_.x || x0 > well_.x + well_.w)
                    continue;
                const Rect r{x0, y, std::max(3.0f, x1 - x0), blockHeight};
                list.add(theme.accent(r, Shape::Soft, 0.3f));
                list.add(theme.accent({r.x, r.y, r.w, 1.0f}, Shape::Rounded, 0.65f));
                list.add(theme.accent({r.x, r.y + r.h - 1.0f, r.w, 1.0f}, Shape::Rounded, 0.65f));
                list.add(theme.accent({r.x, r.y, 1.0f, r.h}, Shape::Rounded, 0.65f));
                list.add(theme.accent({r.x + r.w - 1.0f, r.y, 1.0f, r.h}, Shape::Rounded, 0.65f));
            }
        }
    }
    // The loop's range, faintly across the lanes
    if (project().hasLoop()) {
        const float x0 = beatX(project().loopStart), x1 = beatX(project().loopEnd);
        list.add(theme.accent({x0, well_.y, x1 - x0, well_.h}, Shape::Rounded, project().looping ? 0.04f : 0.02f));
    }
    // Past the end of the timeline, a veil
    const float end = beatX(project().lengthBeats());
    if (end < well_.x + well_.w)
        list.add(theme.scrim({end, well_.y, well_.x + well_.w - end, well_.h}, 0.5f));
    for (int t = 0; t < tracks; ++t) {
        if (hidden(t))
            continue;
        const int clips = static_cast<int>(project().tracks[static_cast<size_t>(t)].clips.size());
        for (int c = 0; c < clips; ++c)
            drawClip(list, theme, t, c);
    }
    list.popClip();

    // Scroll bar for time (below the lanes, where nothing else is drawn)
    list.add(theme.surface(bar_, Shape::Pill, Elevation::Inset, 0.4f));
    if (contentWidth() > lanes_.w)
        list.add(theme.surface(thumb().inset(1.0f), Shape::Pill, Elevation::Raised, draggingBar_ ? 0.7f : 0.45f,
                               Finish::Convex));
}

void ArrangementView::draw(DrawList &list, const Theme &theme) const
{
    // Cached: the ruler, the lanes with their clips, and the scroll bar. Kept while the key says
    // nothing they read has moved; a take growing while recording paints every frame.
    const bool recording = anyRecording();
    cache_.draw(list, theme, rect_, contentKey(recording), [&] { drawContent(list, theme); }, !recording);

    // Overlay, every frame: the playhead across the ruler and the lanes. Its triangle starts past
    // the headers' column (the ruler starts after the gap), so it covers nothing they draw.
    const float rulerBottom = ruler_.y + ruler_.h;
    if (transport_.position) {
        const float x = beatX(transport_.position());
        if (x >= ruler_.x - 1.0f && x <= ruler_.x + ruler_.w + 1.0f) {
            list.add(theme.accent({x - 1.0f, rulerBottom - 4.0f, 2.0f, well_.y + well_.h - rulerBottom + 2.0f},
                                  Shape::Rounded, 0.9f));
            list.add(theme.accent({x - 6.0f, rulerBottom - 12.0f, 12.0f, 9.0f}, Shape::TriangleDown));
        }
    }

    // Track headers, scrolled with the lanes. Their labels and toggles follow the input (live MIDI,
    // hover), so they are never cached. They lie left of the ruler, clear of the playhead.
    list.pushClip(headerArea_);
    const size_t count = std::min(headers_.size(), project().tracks.size());
    const bool lifted = reorderFrom_ >= 0 && reorderMoved_; // drawn as the preview below
    for (size_t i = 0; i < count; ++i) {
        if ((lifted && static_cast<int>(i) == reorderFrom_) || hidden(static_cast<int>(i)))
            continue;
        headers_[i]->draw(list, theme);
    }
    if (masterHeader_)
        masterHeader_->draw(list, theme);
    // A thin bar for each group a track is inside, down the whole of its row, so the nesting reads
    for (size_t i = 0; i < count && !lifted; ++i) {
        const int d = depth(static_cast<int>(i));
        if (d == 0 || hidden(static_cast<int>(i)))
            continue;
        const float top = rowTop(static_cast<int>(i)) + 4.0f, h = rowHeight(static_cast<int>(i)) - 8.0f;
        if (top > headerArea_.y + headerArea_.h || top + h < headerArea_.y)
            continue;
        for (int level = 0; level < d; ++level)
            list.add(theme.accent({headerArea_.x + 4.0f + kIndent * static_cast<float>(level), top, 2.0f, h}, Shape::Pill, 0.35f));
    }
    if (!lifted)
        drawLaneHeaders(list, theme);
    list.popClip();

    // The dragged track's preview: its header lifted above the others, following the mouse
    if (reorderFrom_ >= 0 && reorderMoved_ && static_cast<size_t>(reorderFrom_) < headers_.size()) {
        const Rect area{headerArea_.x, lanes_.y, lanes_.x + lanes_.w - headerArea_.x, lanes_.h};
        const Rect r = headers_[static_cast<size_t>(reorderFrom_)]->rect();
        list.pushClip(area);
        list.add(theme.surface(r, Shape::Soft, Elevation::Raised, 0.9f, Finish::Convex));
        headers_[static_cast<size_t>(reorderFrom_)]->draw(list, theme);
        list.popClip();
    }

    // Where a dragged instrument would go: its row, header and lane, outlined in the accent
    if (dropRow_ >= 0) {
        const Rect area{headerArea_.x, lanes_.y, lanes_.x + lanes_.w - headerArea_.x, lanes_.h};
        const Rect r{headerArea_.x, rowTop(dropRow_) + 3.0f, area.w, kTrackHeight - 6.0f};
        list.pushClip(area);
        list.add(theme.accent(r, Shape::Rounded, 0.07f));
        const float t = 2.0f;
        list.add(theme.accent({r.x + 8.0f, r.y, r.w - 16.0f, t}, Shape::Pill, 0.8f));
        list.add(theme.accent({r.x + 8.0f, r.y + r.h - t, r.w - 16.0f, t}, Shape::Pill, 0.8f));
        if (!dropHint_.empty())
            list.add(theme.text(dropHint_, r, theme.fontSize(TextStyle::Body), TextAlign::Center, theme.textColor(TextTone::Accent)));
        list.popClip();
    }
}

// Automation lanes

Rect ArrangementView::laneRect(int track, size_t lane) const
{
    return {well_.x, rowTop(track) + headHeight(track) + static_cast<float>(lane) * kLaneHeight, well_.w, kLaneHeight};
}

Rect ArrangementView::laneHeaderRect(int track, size_t lane) const
{
    return {headerArea_.x, rowTop(track) + headHeight(track) + static_cast<float>(lane) * kLaneHeight, headerArea_.w, kLaneHeight};
}

Rect ArrangementView::laneRemoveRect(int track, size_t lane) const
{
    const Rect h = laneHeaderRect(track, lane);
    return {h.x + h.w - 30.0f, h.y + h.h * 0.5f - 10.0f, 20.0f, 20.0f};
}

Rect ArrangementView::addLaneRect(int track) const
{
    const Row *row = rowOf(track);
    const size_t lanes = row ? row->lanes.size() : 0;
    return {headerArea_.x, rowTop(track) + headHeight(track) + static_cast<float>(lanes) * kLaneHeight, headerArea_.w, kAddHeight};
}

float ArrangementView::laneY(const Rect &lane, float position) const
{
    return lane.y + kLanePad + (1.0f - std::clamp(position, 0.0f, 1.0f)) * (lane.h - 2.0f * kLanePad);
}

float ArrangementView::lanePosition(const Rect &lane, float y) const
{
    return std::clamp(1.0f - (y - lane.y - kLanePad) / std::max(1.0f, lane.h - 2.0f * kLanePad), 0.0f, 1.0f);
}

double ArrangementView::laneSnap(double beat, bool free) const
{
    beat = std::clamp(beat, 0.0, project().lengthBeats());
    if (free)
        return beat;
    // The finest of a bar, a beat, a half and a quarter whose lines stay at least 12 pixels apart
    double step = std::max(1, project().beatsPerBar);
    for (double finer : {1.0, 0.5, 0.25}) {
        if (finer * pixelsPerBeat_ >= 12.0)
            step = finer;
    }
    return std::clamp(std::round(beat / step) * step, 0.0, project().lengthBeats());
}

const audio::AutomationLane *ArrangementView::laneOf(int track, const AutomationRow &row) const
{
    const std::vector<audio::AutomationLane> *lanes = lanesAt(project(), track, row.place);
    return lanes ? audio::findAutomation(*lanes, row.param) : nullptr;
}

audio::DeviceParam ArrangementView::laneParam(int track, const AutomationRow &row) const
{
    (void)track;
    if (row.place.own)
        return trackParam(row.param);
    if (transport_.deviceParams) {
        for (audio::DeviceParam &p : transport_.deviceParams(row.place.device)) {
            if (p.id == row.param)
                return std::move(p);
        }
    }
    audio::DeviceParam unknown;
    unknown.id = row.param;
    unknown.name = "Parameter " + std::to_string(row.param);
    return unknown;
}

std::string ArrangementView::laneName(int track, const AutomationRow &row) const
{
    const std::string param = laneParam(track, row).name;
    if (row.place.own && isSendParam(row.param)) {
        const Track *ret = project().findTrack(sendTarget(row.param));
        return "Send to " + (ret ? ret->name : std::string("a deleted return"));
    }
    if (row.place.own)
        return param;
    const audio::DeviceSetting *device = project().device(row.place.device);
    return device ? device->name + ": " + param : param;
}

float ArrangementView::restingPosition(int track, const AutomationRow &row) const
{
    // The master's lanes: its volume or an effect's parameter
    const bool master = row.place.master || (!row.place.own && row.place.device.master);
    const Track *t = master ? nullptr : project().track(track);
    if (!master && !t)
        return 0.0f;
    if (row.place.own) {
        if (master)
            return faderPosition(project().masterVolume, 2.0f);
        if (row.param == audio::kPanParam)
            return (t->pan + 1.0f) * 0.5f;
        if (isSendParam(row.param)) {
            const Send *send = findSend(*t, sendTarget(row.param));
            return faderPosition(send ? send->level : 0.0f, 1.0f);
        }
        return faderPosition(t->gain, 2.0f);
    }
    const audio::DeviceSetting *device = project().device(row.place.device);
    const audio::DeviceParam param = laneParam(track, row);
    const double value = device ? device->value(param.id, param.def) : param.def;
    return static_cast<float>(automationCurve(param).position(value));
}

void ArrangementView::drawLanes(DrawList &list, const Theme &theme) const
{
    const size_t count = std::min(rows_.size(), project().tracks.size());
    for (size_t t = 0; t <= count; ++t) {
        // (the last is the master's row)
        const int track = t == count ? static_cast<int>(project().tracks.size()) : static_cast<int>(t);
        const Row *rowp = rowOf(track);
        if (!rowp || !rowp->open)
            continue;
        const Row &row = *rowp;
        for (size_t l = 0; l < row.lanes.size(); ++l) {
            const Rect lane = laneRect(track, l);
            if (lane.y > well_.y + well_.h || lane.y + lane.h < well_.y)
                continue;
            const bool hot = track == laneTrack_ && static_cast<int>(l) == laneIndex_;
            list.add(theme.surface({lane.x + 4.0f, lane.y + 2.0f, lane.w - 8.0f, lane.h - 4.0f}, Shape::Soft, Elevation::Inset,
                                   hot ? 0.45f : 0.3f));
            list.add(theme.ink({lane.x, laneY(lane, 0.5f), lane.w, 1.0f}, Shape::Rounded, TextTone::Muted, true, 0.12f));
            const audio::AutomationLane *data = laneOf(track, row.lanes[l]);
            const audio::DeviceParam param = laneParam(track, row.lanes[l]);
            const bool stepped = automationCurve(param).stepped;
            if (!data || data->points.empty()) {
                // No points yet: where the parameter rests, dashed, and a hint
                const float y = laneY(lane, restingPosition(track, row.lanes[l]));
                for (float x = lane.x + 6.0f; x < lane.x + lane.w - 6.0f; x += 12.0f)
                    list.add(theme.ink({x, y - 0.5f, 6.0f, 1.5f}, Shape::Pill, TextTone::Muted, true, 0.5f));
                list.add(theme.text("Click to add a point", {lane.x + 14.0f, lane.y + 4.0f, 240.0f, 16.0f}, TextStyle::Caption,
                                    TextTone::Muted, TextAlign::Start));
                continue;
            }
            // The curve: held flat before the first point and after the last, straight (or stepped) between
            const auto &points = data->points;
            std::vector<Vec2> line;
            line.reserve(points.size() * 2 + 2);
            line.push_back({lane.x, laneY(lane, points.front().value)});
            for (size_t i = 0; i < points.size(); ++i) {
                const Vec2 p{beatX(points[i].beat), laneY(lane, points[i].value)};
                if (stepped && i > 0)
                    line.push_back({p.x, line.back().y});
                line.push_back(p);
            }
            line.push_back({lane.x + lane.w, line.back().y});
            for (Vec2 &p : line)
                p.x = std::clamp(p.x, lane.x, lane.x + lane.w);
            list.addPolyline(line.data(), line.size(), theme.accentLine({}, {}, 2.0f, 0.85f));
            // Its points, as raised knobs with an accent core
            for (size_t i = 0; i < points.size(); ++i) {
                const float x = beatX(points[i].beat);
                if (x < lane.x - 8.0f || x > lane.x + lane.w + 8.0f)
                    continue;
                const float y = laneY(lane, points[i].value);
                const bool on = hot && static_cast<int>(i) == lanePoint_;
                const float d = on ? 13.0f : 10.0f;
                list.add(theme.surface({x - d * 0.5f, y - d * 0.5f, d, d}, Shape::Circle, Elevation::Raised, on ? 0.75f : 0.45f,
                                       Finish::Convex));
                list.add(theme.accent({x - d * 0.5f + 2.5f, y - d * 0.5f + 2.5f, d - 5.0f, d - 5.0f}, Shape::Circle, on ? 1.0f : 0.8f));
            }
        }
    }
}

void ArrangementView::drawLaneHeaders(DrawList &list, const Theme &theme) const
{
    const double playhead = transport_.position ? transport_.position() : 0.0;
    const size_t count = std::min(rows_.size(), project().tracks.size());
    for (size_t t = 0; t <= count; ++t) {
        const int track = t == count ? static_cast<int>(project().tracks.size()) : static_cast<int>(t);
        const Row *rowp = rowOf(track);
        if (!rowp || !rowp->open)
            continue;
        const Row &row = *rowp;
        for (size_t l = 0; l < row.lanes.size(); ++l) {
            const Rect h = laneHeaderRect(track, l);
            if (h.y > headerArea_.y + headerArea_.h || h.y + h.h < headerArea_.y)
                continue;
            const AutomationRow &lane = row.lanes[l];
            const audio::DeviceParam param = laneParam(track, lane);
            const audio::AutomationLane *data = laneOf(track, lane);
            const bool playing = data && !data->points.empty();
            // The track's own lanes have their own curves (the volume on the fader's taper)
            const audio::ParamCurve curve = lane.place.own ? automationCurve(lane.param) : automationCurve(param);
            const float position = playing ? data->positionAt(playhead, curve.stepped) : restingPosition(track, lane);
            const std::string value = automationValueText(param, curve.value(position));
            // Indented under the track's header (and its groups), a line from it to the lane
            const float in = kIndent * static_cast<float>(depth(track));
            list.add(theme.ink({h.x + in + 20.0f, h.y + 4.0f, 2.0f, h.h - 8.0f}, Shape::Pill, TextTone::Muted, true, 0.3f));
            const float textW = h.w - 70.0f - in;
            list.add(theme.text(laneName(track, lane), {h.x + in + 32.0f, h.y + 9.0f, textW, 18.0f}, TextStyle::Caption,
                                TextTone::Secondary, TextAlign::Start));
            list.add(theme.text(value, {h.x + in + 32.0f, h.y + 28.0f, textW, 18.0f}, TextStyle::Caption,
                                playing ? TextTone::Accent : TextTone::Muted, TextAlign::Start));
            // The remove button: a cross in a small well
            const Rect x = laneRemoveRect(track, l);
            const bool hot = track == laneTrack_ && static_cast<int>(l) == hoveredRemove_;
            list.add(theme.surface(x, Shape::Circle, hot ? Elevation::Raised : Elevation::Inset, hot ? 0.7f : 0.35f));
            const Vec2 c{x.x + x.w * 0.5f, x.y + x.h * 0.5f};
            const TextTone tone = hot ? TextTone::Accent : TextTone::Muted;
            list.add(theme.inkLine({c.x - 3.5f, c.y - 3.5f}, {c.x + 3.5f, c.y + 3.5f}, 1.5f, tone));
            list.add(theme.inkLine({c.x - 3.5f, c.y + 3.5f}, {c.x + 3.5f, c.y - 3.5f}, 1.5f, tone));
        }
        // The strip that adds a lane
        const Rect add = addLaneRect(track);
        const bool hot = hoveredAdd_ == track;
        const Rect pill{add.x + 32.0f + kIndent * static_cast<float>(depth(track)), add.y + 4.0f, 150.0f, add.h - 8.0f};
        list.add(theme.surface(pill, Shape::Pill, Elevation::Raised, hot ? 0.7f : 0.4f, Finish::Convex));
        list.add(theme.text("+ Add automation", pill, TextStyle::Caption, hot ? TextTone::Accent : TextTone::Secondary,
                            TextAlign::Center));
    }
}

bool ArrangementView::handleLaneInput(const Input &input)
{
    lanePress_ = false;
    // A point being dragged: it follows the mouse, between its neighbours, until the button lets go
    if (draggingPoint_) {
        const Row *row = rowOf(laneTrack_);
        const AutomationRow *lane = row && laneIndex_ >= 0 && static_cast<size_t>(laneIndex_) < row->lanes.size()
                                        ? &row->lanes[static_cast<size_t>(laneIndex_)]
                                        : nullptr;
        const audio::AutomationLane *data = lane ? laneOf(laneTrack_, *lane) : nullptr;
        if (!input.down || !data || lanePoint_ < 0 || static_cast<size_t>(lanePoint_) >= data->points.size()) {
            draggingPoint_ = false;
            return true;
        }
        const size_t i = static_cast<size_t>(lanePoint_);
        const double low = i > 0 ? data->points[i - 1].beat : 0.0;
        const double high = i + 1 < data->points.size() ? data->points[i + 1].beat : project().lengthBeats();
        pointMoved_ = pointMoved_ || std::fabs(input.mouse.x - beatX(data->points[i].beat)) > 2.0f;
        const double beat = std::clamp(laneSnap(xBeat(input.mouse.x), input.shift), low, high);
        const float value = lanePosition(laneRect(laneTrack_, static_cast<size_t>(laneIndex_)), input.mouse.y);
        // Compared first: a point that hasn't moved edits nothing
        if (data->points[i].beat != beat || data->points[i].value != value) {
            if (std::vector<audio::AutomationLane> *lanes = editLanesAt(store_, laneTrack_, lane->place)) {
                if (audio::AutomationLane *edit = audio::findAutomation(*lanes, lane->param)) {
                    edit->points[i].beat = beat;
                    edit->points[i].value = value;
                }
            }
        }
        input.setCursor(engine::Cursor::Hand);
        return true;
    }

    laneTrack_ = laneIndex_ = lanePoint_ = hoveredRemove_ = hoveredAdd_ = -1;
    const bool inHeaders = headerArea_.contains(input.mouse);
    const bool inWell = well_.contains(input.mouse);
    if (!inHeaders && !inWell)
        return false;
    const int track = laneRowAt(input.mouse.y);
    const Row *found = rowOf(track);
    if (track < 0 || !found || !found->open || input.mouse.y < rowTop(track) + headHeight(track))
        return false;
    const Row &row = *found;

    // The strip under the lanes: its button opens the menu of what can be automated
    const Rect add = addLaneRect(track);
    if (add.contains(input.mouse)) {
        if (inHeaders)
            hoveredAdd_ = track;
        if (input.pressed) {
            lanePress_ = true;
            if (inHeaders && isMaster(track) && transport_.masterAutomationMenu)
                transport_.masterAutomationMenu({add.x + 32.0f, add.y + add.h});
            else if (inHeaders && !isMaster(track) && transport_.automationMenu)
                transport_.automationMenu(track, {add.x + 32.0f, add.y + add.h});
        }
        return true;
    }
    const size_t index = static_cast<size_t>((input.mouse.y - rowTop(track) - headHeight(track)) / kLaneHeight);
    if (index >= row.lanes.size())
        return true;
    laneTrack_ = track;
    laneIndex_ = static_cast<int>(index);
    const AutomationRow &lane = row.lanes[index];

    // A lane's header: its x removes the lane, points and all
    if (inHeaders) {
        if (laneRemoveRect(track, index).contains(input.mouse)) {
            hoveredRemove_ = laneIndex_;
            if (input.pressed) {
                lanePress_ = true;
                if (std::vector<audio::AutomationLane> *lanes = editLanesAt(store_, track, lane.place))
                    lanes->erase(std::remove_if(lanes->begin(), lanes->end(),
                                                [&lane](const audio::AutomationLane &l) { return l.param == lane.param; }),
                                 lanes->end());
                laneTrack_ = laneIndex_ = hoveredRemove_ = -1;
            }
        }
        if (input.pressed)
            lanePress_ = true; // the header itself does nothing
        return true;
    }

    // In the lane: the point under the mouse
    const Rect rect = laneRect(track, index);
    const audio::AutomationLane *data = laneOf(track, lane);
    if (data) {
        float best = 8.0f;
        for (size_t i = 0; i < data->points.size(); ++i) {
            const float dx = std::fabs(beatX(data->points[i].beat) - input.mouse.x);
            const float dy = std::fabs(laneY(rect, data->points[i].value) - input.mouse.y);
            if (dx < best && dy < 10.0f) {
                best = dx;
                lanePoint_ = static_cast<int>(i);
            }
        }
    }
    if (lanePoint_ >= 0)
        input.setCursor(engine::Cursor::Hand);

    auto erasePoint = [&](int point) {
        if (std::vector<audio::AutomationLane> *lanes = editLanesAt(store_, track, lane.place))
            if (audio::AutomationLane *edit = audio::findAutomation(*lanes, lane.param))
                if (point >= 0 && static_cast<size_t>(point) < edit->points.size())
                    edit->points.erase(edit->points.begin() + point);
        lanePoint_ = -1;
    };
    // Right-click a point to remove it
    if (input.secondaryPressed && lanePoint_ >= 0) {
        input.secondaryPressed = false;
        erasePoint(lanePoint_);
        return true;
    }
    if (input.pressed) {
        lanePress_ = true;
        const bool doubleClick = lastPointPress_ >= 0.0 && input.time - lastPointPress_ < 0.3 && lanePoint_ >= 0 &&
                                 lanePoint_ == lastPressedPoint_;
        lastPointPress_ = input.time;
        if (doubleClick) {
            erasePoint(lanePoint_);
            lastPointPress_ = -1.0;
            return true;
        }
        if (lanePoint_ < 0) {
            // A new point where the click was, snapped unless Shift is held
            if (std::vector<audio::AutomationLane> *lanes = editLanesAt(store_, track, lane.place)) {
                audio::AutomationLane &edit = audio::automationFor(*lanes, lane.param);
                const double beat = laneSnap(xBeat(input.mouse.x), input.shift);
                lanePoint_ = static_cast<int>(audio::setAutomationPoint(edit, beat, lanePosition(rect, input.mouse.y), 1e-9));
            }
        }
        lastPressedPoint_ = lanePoint_;
        draggingPoint_ = lanePoint_ >= 0;
        pointMoved_ = false;
    }
    return true;
}

Rect ArrangementView::loopBand() const
{
    return {ruler_.x, ruler_.y + 1.0f, ruler_.w, 10.0f};
}

double ArrangementView::loopSnap(double beat, bool shift) const
{
    const double step = shift ? 1.0 : std::max(1, project().beatsPerBar);
    return std::clamp(std::round(beat / step) * step, 0.0, project().lengthBeats());
}

}
