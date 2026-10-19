#include "PianoRoll.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <utility>

#include "Format.h"
#include "audio/SampleSet.h"

namespace daw {

using namespace ui;
using engine::Cursor;

std::vector<Note> PianoRollView::clipboard_;

namespace {

constexpr float kKeysWidth = 76.0f;
constexpr float kRulerHeight = 34.0f;
constexpr float kLoopBand = 11.0f;  // the loop strip along the ruler's top
constexpr float kMiniHeight = 22.0f;
constexpr float kGap = 10.0f;
constexpr float kEndGrab = 7.0f;    // pixels at a note's ends that resize it
constexpr float kEdge = 24.0f;      // dragging this close to an edge scrolls
constexpr float kDragStart = 4.0f;  // pixels the mouse goes before a press becomes a drag
constexpr double kMinLength = 1.0 / 64.0;
constexpr double kLeadBeats = 2.0;  // the view can show this much before the clip's start
constexpr double kTailBeats = 8.0;  // and this much after its end

const double kGridBeats[] = {0.0, 4.0, 2.0, 1.0, 0.5, 0.25, 0.125, 0.0625};

bool isBlack(int key)
{
    const int k = key % 12;
    return k == 1 || k == 3 || k == 6 || k == 8 || k == 10;
}

std::string plural(int n, const char *what)
{
    return std::to_string(n) + " " + what + (n == 1 ? "" : "s");
}

// A rectangle's outline as four thin fills
void outline(DrawList &list, const Theme &theme, const Rect &r, float opacity, float width = 1.0f)
{
    for (const Rect &edge : {Rect{r.x, r.y, r.w, width}, Rect{r.x, r.y + r.h - width, r.w, width},
                             Rect{r.x, r.y, width, r.h}, Rect{r.x + r.w - width, r.y, width, r.h}})
        list.add(theme.accent(edge, Shape::Rounded, opacity));
}

bool overlaps(const Rect &a, const Rect &b)
{
    return a.x < b.x + b.w && a.x + a.w > b.x && a.y < b.y + b.h && a.y + a.h > b.y;
}

std::string lengthText(double beats, int beatsPerBar)
{
    const std::string name = gridName(beats, beatsPerBar);
    return name.find("beats") == std::string::npos ? name : format("%.3g beats", beats);
}

}

PianoRollView::PianoRollView(ProjectStore &store, PianoRollHooks hooks)
    : store_(store), hooks_(std::move(hooks))
{
    rebuildRows();
}

const std::vector<std::string> &PianoRollView::gridChoices()
{
    static const std::vector<std::string> names{"Auto", "1/1", "1/2", "1/4", "1/8", "1/16", "1/32", "1/64"};
    return names;
}

double PianoRollView::snap() const
{
    const int choice = std::clamp(gridChoice_, 0, static_cast<int>(std::size(kGridBeats)) - 1);
    if (choice == 0)
        return autoGrid(pixelsPerBeat_, 16.0, triplet_);
    return kGridBeats[choice] * (triplet_ ? 2.0 / 3.0 : 1.0);
}

std::string PianoRollView::gridText() const
{
    return gridName(snap(), std::max(1, project().beatsPerBar));
}

const Clip *PianoRollView::clip() const
{
    const Track *t = project().track(project().selectedTrack);
    return t && t->kind == TrackKind::Instrument ? project().clip() : nullptr;
}

Clip *PianoRollView::editClip()
{
    if (!clip())
        return nullptr;
    return &store_.editClip(project().selectedTrack, project().selectedClip);
}

const Scale *PianoRollView::scale() const
{
    return scale_ >= 0 && scale_ < static_cast<int>(scales().size()) ? &scales()[static_cast<size_t>(scale_)] : nullptr;
}

int PianoRollView::selectedCount() const
{
    const Clip *c = clip();
    return c ? daw::selectedCount(c->notes) : 0;
}

bool PianoRollView::hasSelectedNote() const { return selectedCount() > 0; }

const Note *PianoRollView::firstSelected() const
{
    const Clip *c = clip();
    const Note *first = nullptr;
    if (c) {
        for (const Note &n : c->notes) {
            if (n.selected && (!first || n.start < first->start || (n.start == first->start && n.key < first->key)))
                first = &n;
        }
    }
    return first;
}

void PianoRollView::deleteSelectedNote()
{
    Clip *c = editClip();
    if (!c)
        return;
    const int n = removeSelected(c->notes);
    hovered_ = -1;
    drag_ = Drag::None;
    if (n > 0)
        message_ = "Deleted " + plural(n, "note");
}

bool PianoRollView::edit(const std::function<int(std::vector<Note> &)> &tool, const std::string &done)
{
    Clip *c = editClip();
    if (!c)
        return false;
    const int selected = daw::selectedCount(c->notes);
    const int n = tool(c->notes);
    clampToClip(c->notes, c->length, kMinLength);
    hovered_ = -1;
    message_ = done + ": " + plural(n, "note") + (selected == 0 && n > 0 ? " (all of them)" : "");
    return true;
}

void PianoRollView::transposeSelection(int semitones)
{
    if (!clip() || !hasSelectedNote())
        return;
    Clip *c = editClip();
    transpose(c->notes, semitones);
    if (const Note *n = firstSelected()) {
        audition(n->key, n->velocity);
        soundUntil_ = now_ + 0.25;
    }
}

void PianoRollView::nudgeSelection(double beats)
{
    if (!clip() || !hasSelectedNote())
        return;
    Clip *c = editClip();
    nudge(c->notes, beats);
    clampToClip(c->notes, c->length, kMinLength);
}

void PianoRollView::resizeSelection(double beats)
{
    if (!clip() || !hasSelectedNote())
        return;
    Clip *c = editClip();
    resizeBy(c->notes, beats, kMinLength);
    clampToClip(c->notes, c->length, kMinLength);
    if (const Note *n = firstSelected())
        lastLength_ = n->length;
}

void PianoRollView::velocitySelection(float by)
{
    if (!clip() || !hasSelectedNote())
        return;
    Clip *c = editClip();
    shiftVelocity(c->notes, by);
    if (const Note *n = firstSelected()) {
        lastVelocity_ = n->velocity;
        shownVelocity_ = n->velocity;
    }
}

void PianoRollView::toggleMute()
{
    Clip *c = editClip();
    if (!c)
        return;
    const int changed = daw::toggleMute(c->notes);
    const Note *n = firstSelected();
    const bool muted = n ? n->muted : (!c->notes.empty() && c->notes.front().muted);
    message_ = std::string(muted ? "Muted " : "Unmuted ") + plural(changed, "note");
}

void PianoRollView::setLoop(double start, double end)
{
    const double from = std::max(0.0, std::min(start, end)), to = std::max(start, end);
    // Without a loop (the end not after the start) looping goes off
    store_.setLoop(from, to, to > from && project().looping);
}

void PianoRollView::loopSelection()
{
    const Clip *c = clip();
    if (!c)
        return;
    const NoteBounds b = noteBounds(c->notes);
    const double step = std::max(snap(), 0.25);
    double start = 0.0, end = c->length;
    if (b.count > 0 && hasSelectedNote()) {
        start = std::floor(b.start / step + 1e-9) * step;
        end = std::max(start + step, std::ceil(b.end / step - 1e-9) * step);
    }
    setLoop(c->start + start, c->start + end);
    setLooping(true);
    message_ = "Looping " + barsBeats(project().loopStart, std::max(1, project().beatsPerBar)) + " to " +
               barsBeats(project().loopEnd, std::max(1, project().beatsPerBar));
}

// --- The view ------------------------------------------------------------------------------

void PianoRollView::zoom(float factor)
{
    zoomAround(factor, grid_.x + grid_.w * 0.5f);
}

void PianoRollView::zoomAround(float factor, float x)
{
    if (!zooming_) {
        zoomAtBeat_ = xBeat(x);
        targetZoom_ = pixelsPerBeat_;
    } else {
        // Keep the beat the glide is heading for under the mouse
        zoomAtBeat_ = static_cast<double>(x - grid_.x + targetX_) / targetZoom_;
    }
    zoomAtX_ = x;
    targetZoom_ = std::clamp(targetZoom_ * factor, 6.0f, 600.0f);
    targetX_ = static_cast<float>(zoomAtBeat_) * targetZoom_ - (x - grid_.x);
    zooming_ = true;
}

void PianoRollView::zoomRows(float factor, float y)
{
    const float row = (y - grid_.y + scrollY_) / rowHeight_;
    rowHeight_ = std::clamp(rowHeight_ * factor, 7.0f, 36.0f);
    setScrollY(row * rowHeight_ - (y - grid_.y), false);
}

void PianoRollView::fit()
{
    const Clip *c = clip();
    if (!c || grid_.w <= 0.0f)
        return;
    NoteBounds b = noteBounds(c->notes);
    double start = 0.0, end = c->length;
    if (hasSelectedNote()) {
        start = b.start;
        end = b.end;
    }
    const double span = std::max(1.0, end - start);
    targetZoom_ = std::clamp(static_cast<float>(grid_.w * 0.9 / span), 6.0f, 600.0f);
    zoomAtBeat_ = start;
    zoomAtX_ = grid_.x + grid_.w * 0.05f;
    targetX_ = static_cast<float>(start) * targetZoom_ - grid_.w * 0.05f;
    zooming_ = true;
    if (b.count > 0) {
        const float middle = (static_cast<float>(rowOf_[b.high] + rowOf_[b.low]) + 1.0f) * 0.5f * rowHeight_;
        setScrollY(middle - grid_.h * 0.5f, true);
    }
}

void PianoRollView::scrollToNotes()
{
    const Clip *c = clip();
    zooming_ = false;
    setScrollX(0.0f, false);
    if (c && !c->notes.empty()) {
        int low = 127, high = 0;
        for (const Note &n : c->notes) {
            low = std::min(low, n.key);
            high = std::max(high, n.key);
        }
        const float middle = (static_cast<float>(rowOf_[high] + rowOf_[low]) + 1.0f) * 0.5f * rowHeight_;
        setScrollY(middle - grid_.h * 0.5f, false);
    }
}

Vec2 PianoRollView::measure(const Theme &) const
{
    return {kKeysWidth + 300.0f, kRulerHeight + 8.0f * rowHeight_ + kMiniHeight + kGap + (velocityLane_ ? 50.0f + kGap : 0.0f)};
}

void PianoRollView::arrange(const Rect &rect, const Theme &)
{
    rect_ = rect;
    // The lane keeps at least 80 pixels of grid above it
    const float laneHeight = velocityLane_ ? std::clamp(laneHeight_, 30.0f, std::max(30.0f, rect.h - kRulerHeight - kMiniHeight - 2.0f * kGap - 80.0f)) : 0.0f;
    const float lane = velocityLane_ ? laneHeight + kGap : 0.0f;
    const float gridHeight = std::max(0.0f, rect.h - kRulerHeight - kMiniHeight - kGap - lane);
    corner_ = {rect.x, rect.y, kKeysWidth, kRulerHeight - 4.0f};
    keys_ = {rect.x, rect.y + kRulerHeight, kKeysWidth, gridHeight};
    ruler_ = {rect.x + kKeysWidth + kGap, rect.y, std::max(0.0f, rect.w - kKeysWidth - kGap), kRulerHeight};
    loopBand_ = {ruler_.x, ruler_.y + 1.0f, ruler_.w, kLoopBand};
    grid_ = {ruler_.x, keys_.y, ruler_.w, gridHeight};
    divider_ = velocityLane_ ? Rect{rect.x, grid_.y + grid_.h, rect.w, kGap} : Rect{};
    lane_ = velocityLane_ ? Rect{grid_.x, grid_.y + grid_.h + kGap, grid_.w, laneHeight} : Rect{};
    laneKeys_ = velocityLane_ ? Rect{keys_.x, lane_.y, keys_.w, lane_.h} : Rect{};
    mini_ = {grid_.x, rect.y + rect.h - kMiniHeight, grid_.w, kMiniHeight};
    gridLabel_ = {rect.x, mini_.y, kKeysWidth, kMiniHeight};
    if (scrollY_ < 0.0f && grid_.h > 0.0f) { // first layout: around E4, before any notes
        scrollY_ = (static_cast<float>(rowOf_[64]) + 0.5f) * rowHeight_ - grid_.h * 0.5f;
        targetY_ = scrollY_;
    }
    if (revealPending_ && grid_.h > 0.0f) {
        scrollToNotes();
        revealPending_ = false;
    }
    clampScroll();
}

float PianoRollView::beatX(double beat) const
{
    return grid_.x + static_cast<float>(beat) * pixelsPerBeat_ - scrollX_;
}

double PianoRollView::xBeat(float x) const
{
    return static_cast<double>(x - grid_.x + scrollX_) / pixelsPerBeat_;
}

float PianoRollView::keyY(int key) const
{
    return grid_.y + static_cast<float>(rowOf_[std::clamp(key, 0, 127)]) * rowHeight_ - scrollY_;
}

int PianoRollView::yKey(float y) const
{
    if (rows_.empty())
        return 60;
    const int row = static_cast<int>(std::floor((y - grid_.y + scrollY_) / rowHeight_));
    return rows_[static_cast<size_t>(std::clamp(row, 0, static_cast<int>(rows_.size()) - 1))];
}

float PianoRollView::rowsHeight() const { return static_cast<float>(rows_.size()) * rowHeight_; }

float PianoRollView::velocityY(float velocity) const
{
    return lane_.y + lane_.h - 7.0f - std::clamp(velocity, 0.0f, 1.0f) * (lane_.h - 14.0f);
}

float PianoRollView::yVelocity(float y) const
{
    return std::clamp((lane_.y + lane_.h - 7.0f - y) / std::max(1.0f, lane_.h - 14.0f), 1.0f / 127.0f, 1.0f);
}

Rect PianoRollView::noteRect(const Note &note) const
{
    const float x = beatX(note.start);
    const float w = std::max(4.0f, static_cast<float>(note.length) * pixelsPerBeat_ - 1.0f);
    return {x, keyY(note.key) + 1.0f, w, rowHeight_ - 2.0f};
}

int PianoRollView::noteAt(Vec2 p, int &edge) const
{
    edge = 0;
    const Clip *c = clip();
    if (!c)
        return -1;
    for (int i = static_cast<int>(c->notes.size()) - 1; i >= 0; --i) {
        const Rect r = noteRect(c->notes[static_cast<size_t>(i)]);
        if (r.contains(p)) {
            const float grab = std::min(kEndGrab, r.w * 0.3f);
            if (p.x >= r.x + r.w - grab)
                edge = 1;
            else if (p.x <= r.x + grab && r.w > 14.0f)
                edge = -1;
            return i;
        }
    }
    return -1;
}

int PianoRollView::handleAt(Vec2 p) const
{
    const Clip *c = clip();
    if (!c || !lane_.contains(p))
        return -1;
    int best = -1;
    float distance = 9.0f;
    for (size_t i = 0; i < c->notes.size(); ++i) {
        const Note &n = c->notes[i];
        const float x = beatX(n.start) + 4.0f;
        const float d = std::max(std::fabs(p.x - x), std::fabs(p.y - velocityY(n.velocity)) * 0.6f);
        // Selected notes win ties, so a chord's selected note can be picked out
        if (d < distance || (d <= distance + 0.01f && best >= 0 && n.selected && !c->notes[static_cast<size_t>(best)].selected)) {
            distance = d;
            best = static_cast<int>(i);
        }
    }
    return best;
}

int PianoRollView::ghostAt(Vec2 p) const
{
    if (ghosts_ == Ghosts::Off || !grid_.contains(p))
        return -1;
    for (int i = static_cast<int>(ghostNotes_.size()) - 1; i >= 0; --i) {
        const Ghost &g = ghostNotes_[static_cast<size_t>(i)];
        const Rect r{beatX(g.start), keyY(g.key) + 1.0f, std::max(4.0f, static_cast<float>(g.length) * pixelsPerBeat_ - 1.0f),
                     rowHeight_ - 2.0f};
        if (r.contains(p))
            return i;
    }
    return -1;
}

double PianoRollView::snapDown(double beat, bool free) const
{
    const double s = snap();
    return free || s <= 0.0 ? beat : std::floor(beat / s + 1e-9) * s;
}

double PianoRollView::snapRound(double beat, bool free) const
{
    const double s = snap();
    return free || s <= 0.0 ? beat : std::round(beat / s) * s;
}

bool PianoRollView::freeOf(const Input &input) const
{
    const bool ctrl = (input.mods & engine::ModCtrl) != 0;
    return snapping_ ? ctrl : !ctrl;
}

float PianoRollView::contentWidth() const
{
    const Clip *c = clip();
    return static_cast<float>((c ? c->length : 16.0) + kTailBeats) * pixelsPerBeat_;
}

Rect PianoRollView::miniThumb() const
{
    const Clip *c = clip();
    const double total = (c ? c->length : 16.0) + kTailBeats + kLeadBeats;
    const double from = xBeat(grid_.x) + kLeadBeats, to = xBeat(grid_.x + grid_.w) + kLeadBeats;
    const float x0 = mini_.x + static_cast<float>(std::clamp(from / total, 0.0, 1.0)) * mini_.w;
    const float x1 = mini_.x + static_cast<float>(std::clamp(to / total, 0.0, 1.0)) * mini_.w;
    const float w = std::max(18.0f, x1 - x0);
    return {std::min(x0, mini_.x + mini_.w - w), mini_.y + 3.0f, w, mini_.h - 6.0f};
}

Rect PianoRollView::boundsRect() const
{
    const Clip *c = clip();
    if (!c || daw::selectedCount(c->notes) < 2)
        return {};
    const NoteBounds b = noteBounds(c->notes);
    const float x0 = beatX(b.start), x1 = beatX(b.end);
    const float y0 = keyY(b.high), y1 = keyY(b.low) + rowHeight_;
    return {x0 - 3.0f, std::min(y0, y1) - 3.0f, x1 - x0 + 6.0f, std::fabs(y1 - y0) + 6.0f};
}

Rect PianoRollView::stretchHandle(int side) const
{
    const Rect b = boundsRect();
    if (b.w <= 0.0f || (tool_ != Tool::Draw && tool_ != Tool::Select))
        return {};
    const float h = std::clamp(b.h * 0.5f, 14.0f, 28.0f);
    return {side < 0 ? b.x - 8.0f : b.x + b.w + 1.0f, b.y + (b.h - h) * 0.5f, 7.0f, h};
}

Rect PianoRollView::clipHandle(int side) const
{
    const Clip *c = clip();
    if (!c)
        return {};
    const float x = side < 0 ? beatX(0.0) : beatX(c->length);
    const float y = loopBand_.y + loopBand_.h + 2.0f;
    return {side < 0 ? x : x - 9.0f, y, 9.0f, ruler_.y + ruler_.h - y - 2.0f};
}

Rect PianoRollView::loopRect() const
{
    const Clip *c = clip();
    if (!c || !hasLoop())
        return {};
    const float x0 = beatX(project().loopStart - c->start), x1 = beatX(project().loopEnd - c->start);
    return {x0, loopBand_.y + 1.0f, std::max(4.0f, x1 - x0), loopBand_.h - 2.0f};
}

void PianoRollView::clampScroll()
{
    const Clip *c = clip();
    const double length = (c ? c->length : 16.0) + kTailBeats;
    auto maxX = [this, length](float zoom) { return std::max(-static_cast<float>(kLeadBeats) * zoom, static_cast<float>(length) * zoom - grid_.w); };
    const float maxY = std::max(0.0f, rowsHeight() - grid_.h);
    scrollX_ = std::clamp(scrollX_, -static_cast<float>(kLeadBeats) * pixelsPerBeat_, maxX(pixelsPerBeat_));
    targetX_ = std::clamp(targetX_, -static_cast<float>(kLeadBeats) * targetZoom_, maxX(targetZoom_));
    scrollY_ = std::clamp(scrollY_, 0.0f, maxY);
    targetY_ = std::clamp(targetY_, 0.0f, maxY);
}

void PianoRollView::setScrollX(float x, bool glide)
{
    targetX_ = x;
    if (!glide) {
        scrollX_ = x;
        if (zooming_) {
            pixelsPerBeat_ = targetZoom_;
            zooming_ = false;
            scrollX_ = targetX_;
        }
    }
    clampScroll();
}

void PianoRollView::setScrollY(float y, bool glide)
{
    targetY_ = y;
    if (!glide)
        scrollY_ = y;
    clampScroll();
}

void PianoRollView::glide(const Input &input)
{
    const float k = input.ease(16.0f);
    if (zooming_) {
        pixelsPerBeat_ += (targetZoom_ - pixelsPerBeat_) * k;
        if (std::fabs(pixelsPerBeat_ - targetZoom_) < 0.02f) {
            pixelsPerBeat_ = targetZoom_;
            zooming_ = false;
        }
        // The beat under the mouse stays there as the zoom glides
        scrollX_ = static_cast<float>(zoomAtBeat_) * pixelsPerBeat_ - (zoomAtX_ - grid_.x);
        if (!zooming_)
            scrollX_ = targetX_;
    } else {
        scrollX_ += (targetX_ - scrollX_) * k;
        if (std::fabs(targetX_ - scrollX_) < 0.3f)
            scrollX_ = targetX_;
    }
    scrollY_ += (targetY_ - scrollY_) * k;
    if (std::fabs(targetY_ - scrollY_) < 0.3f)
        scrollY_ = targetY_;
    clampScroll();
}

void PianoRollView::audition(int key, float velocity)
{
    if (!auditionOn_ && drag_ != Drag::Keys)
        return;
    soundUntil_ = -1.0;
    if (sounding_ == key)
        return;
    stopAudition();
    sounding_ = key;
    if (hooks_.auditionOn)
        hooks_.auditionOn(key, velocity);
}

void PianoRollView::stopAudition()
{
    if (sounding_ >= 0 && hooks_.auditionOff)
        hooks_.auditionOff(sounding_);
    sounding_ = -1;
    soundUntil_ = -1.0;
}

void PianoRollView::checkSelection()
{
    if (project().selectedTrack != clipTrack_ || project().selectedClip != clipIndex_) {
        clipTrack_ = project().selectedTrack;
        clipIndex_ = project().selectedClip;
        hovered_ = -1;
        drag_ = Drag::None;
        stopAudition();
        reveal();
        message_.clear();
    }
}

void PianoRollView::rebuildRows()
{
    // The key in the middle of the view stays there when the rows change (folding)
    const size_t before = rows_.size();
    const int middle = before > 0 && grid_.h > 0.0f ? yKey(grid_.y + grid_.h * 0.5f) : -1;
    rows_.clear();
    const Clip *c = clip();
    if (fold_) {
        if (const Scale *s = scale()) {
            for (int k = 127; k >= 0; --k) {
                if (inScale(k, root_, *s))
                    rows_.push_back(k);
            }
        } else if (c) {
            bool used[128] = {};
            for (const Note &n : c->notes)
                used[std::clamp(n.key, 0, 127)] = true;
            for (int k = 127; k >= 0; --k) {
                if (used[k])
                    rows_.push_back(k);
            }
        }
    }
    if (rows_.empty()) {
        for (int k = 127; k >= 0; --k)
            rows_.push_back(k);
    }
    // Each key's row: its own, or the row of the next key down that is shown
    size_t j = rows_.size() - 1;
    for (int key = 0; key < 128; ++key) {
        while (j > 0 && rows_[j - 1] <= key)
            --j;
        rowOf_[key] = static_cast<int>(j);
    }
    if (middle >= 0 && rows_.size() != before)
        setScrollY((static_cast<float>(rowOf_[middle]) + 0.5f) * rowHeight_ - grid_.h * 0.5f, false);
}

void PianoRollView::rebuildGhosts()
{
    ghostNotes_.clear();
    const Clip *c = clip();
    if (!c || ghosts_ == Ghosts::Off)
        return;
    const double from = -kLeadBeats, to = c->length + kTailBeats;
    for (size_t t = 0; t < project().tracks.size(); ++t) {
        const Track &track = project().tracks[t];
        const bool same = static_cast<int>(t) == project().selectedTrack;
        if (track.kind != TrackKind::Instrument || (!same && ghosts_ != Ghosts::All))
            continue;
        for (size_t ci = 0; ci < track.clips.size(); ++ci) {
            const Clip &other = track.clips[ci];
            if (same && static_cast<int>(ci) == project().selectedClip)
                continue;
            const double offset = other.start - c->start;
            if (offset > to || offset + other.length < from)
                continue;
            for (const Note &n : other.notes) {
                if (n.start >= other.length)
                    continue;
                const double start = offset + n.start;
                const double length = std::min(n.length, other.length - n.start);
                if (start > to || start + length < from)
                    continue;
                ghostNotes_.push_back({start, length, n.key, static_cast<int>(t), static_cast<int>(ci), same});
            }
        }
    }
}

double PianoRollView::insertionPoint() const
{
    const Clip *c = clip();
    if (!c)
        return 0.0;
    if (hooks_.position) {
        const double at = hooks_.position() - c->start;
        if (at >= 0.0 && at < c->length)
            return snapDown(at);
    }
    double end = -1.0;
    for (const Note &n : c->notes) {
        if (n.selected)
            end = std::max(end, n.start + n.length);
    }
    const double s = snap();
    return end >= 0.0 && end < c->length ? std::ceil(end / s - 1e-9) * s : 0.0;
}

int PianoRollView::insertionKey() const
{
    const Clip *c = clip();
    int low = 128;
    if (c) {
        for (const Note &n : c->notes) {
            if (n.selected)
                low = std::min(low, n.key);
        }
    }
    return low < 128 ? low : 60 + root_;
}

void PianoRollView::addNotes(std::vector<Note> made, const std::string &what)
{
    if (!clip() || made.empty())
        return;
    Clip *c = editClip();
    daw::selectAll(c->notes, false);
    for (Note &n : made)
        c->notes.push_back(n);
    clampToClip(c->notes, c->length, kMinLength);
    message_ = what + ": " + plural(static_cast<int>(made.size()), "note");
}

void PianoRollView::insertScaleRun(int direction, int octaves)
{
    const Scale *s = scale();
    const Scale chromatic = scales().back();
    addNotes(scaleRun(insertionKey(), octaves, direction, insertionPoint(), snap(), lastVelocity_, root_, s ? *s : chromatic),
             std::string(s ? s->name : "Chromatic") + " run");
}

void PianoRollView::insertChord(const ChordShape &shape)
{
    const Scale *s = scale();
    const Scale major = scales().front();
    const std::vector<int> keys = chordKeys(shape, insertionKey(), root_, s ? *s : major);
    addNotes(chord(keys, insertionPoint(), std::max(snap(), lastLength_), lastVelocity_), shape.name + " chord");
}

void PianoRollView::arpeggiate(const ArpSettings &settings)
{
    edit([settings](std::vector<Note> &notes) { return daw::arpeggiate(notes, settings); },
         std::string("Arpeggiated (") + arpPatternName(settings.pattern) + ")");
}

void PianoRollView::copy()
{
    if (const Clip *c = clip()) {
        clipboard_ = copySelection(c->notes);
        message_ = "Copied " + plural(static_cast<int>(clipboard_.size()), "note");
    }
}

void PianoRollView::cut()
{
    if (!clip())
        return;
    Clip *c = editClip();
    copy();
    if (daw::selectedCount(c->notes) == 0)
        daw::selectAll(c->notes, true);
    removeSelected(c->notes);
    message_ = "Cut " + plural(static_cast<int>(clipboard_.size()), "note");
}

void PianoRollView::paste()
{
    if (!clip() || clipboard_.empty())
        return;
    Clip *c = editClip();
    daw::paste(c->notes, clipboard_, insertionPoint());
    clampToClip(c->notes, c->length, kMinLength);
    message_ = "Pasted " + plural(static_cast<int>(clipboard_.size()), "note");
}

void PianoRollView::selectAll()
{
    if (clip())
        daw::selectAll(editClip()->notes, true);
}

void PianoRollView::duplicate()
{
    edit([this](std::vector<Note> &notes) { return daw::duplicate(notes, snap()); }, "Duplicated");
}

std::string PianoRollView::hint() const
{
    const Clip *c = clip();
    if (!c)
        return "Double-click an instrument clip to edit its notes";
    switch (drag_) {
    case Drag::Move: return "Alt copies  /  Ctrl ignores the grid";
    case Drag::Draw: return "Drag right for the length, up or down for the key";
    case Drag::Resize:
    case Drag::ResizeStart:
    case Drag::Stretch: return "Ctrl ignores the grid";
    case Drag::Box: return "Shift adds to the selection";
    default: break;
    }
    if (hoverStretch_ != 0)
        return "Drag to stretch the selection's timing  /  Ctrl ignores the grid";
    if (hovered_ >= 0 && overGrid_) {
        switch (tool_) {
        case Tool::Slice: return "Click to cut the note here  /  Ctrl cuts off the grid";
        case Tool::Erase: return "Click to delete it, or drag across notes";
        default:
            return hoverEdge_ != 0 ? "Drag to change the length  /  Ctrl ignores the grid"
                                   : "Drag to move  /  Alt-drag copies  /  double-click deletes  /  M mutes";
        }
    }
    if (hoverGhost_ >= 0 && hoverGhost_ < static_cast<int>(ghostNotes_.size())) {
        const Ghost &g = ghostNotes_[static_cast<size_t>(hoverGhost_)];
        const Track *t = project().track(g.track);
        const std::string name = t && g.clip < static_cast<int>(t->clips.size()) ? t->clips[static_cast<size_t>(g.clip)].name : "";
        return "From \"" + name + "\"" + (g.sameTrack ? "" : " on " + (t ? t->name : std::string())) +
               ": double-click to edit that clip";
    }
    if (lane_.contains(mouse_) && laneController_ != kVelocityLane)
        return tool_ == Tool::Erase ? "Drag across points to erase them"
                                    : "Drag to draw  /  drag a point to move it, double-click to delete it  /  Alt-drag draws a line";
    if (lane_.contains(mouse_))
        return "Drag a handle for its velocity  /  drag across to paint  /  Alt-drag draws a ramp";
    if (laneKeys_.contains(mouse_))
        return "Choose what the lane shows: velocity or a controller";
    if (loopBand_.contains(mouse_))
        return "Drag to set the loop  /  click it to loop or not  /  L loops the selection";
    if (ruler_.contains(mouse_))
        return "Click to move the playhead  /  drag the clip's ends to change its length";
    if (keys_.contains(mouse_))
        return "Press a key to hear it";
    if (mini_.contains(mouse_))
        return "The whole clip: drag the window to scroll";
    switch (tool_) {
    case Tool::Draw: return "Click to add a note, drag for its length  /  Ctrl-drag selects  /  B V C E change tools";
    case Tool::Select: return "Drag to select  /  Shift adds  /  Alt draws a lasso  /  B V C E change tools";
    case Tool::Slice: return "Click a note to cut it on the grid  /  Ctrl cuts anywhere";
    case Tool::Erase: return "Click or drag across notes to delete them";
    }
    return {};
}

void PianoRollView::makeHooks()
{
    // Each is a Keymap action, so the keys can be changed. Without a clip (or, for the ones that
    // act on it, a selection) they go on: Delete then removes the clip, Ctrl+D duplicates it.
    auto withClip = [this](std::function<void()> run) {
        return [this, run](const ActionEvent &) {
            if (!clip())
                return false;
            run();
            return true;
        };
    };
    auto withSelection = [this](std::function<void()> run) {
        return [this, run](const ActionEvent &) {
            if (!hasSelectedNote())
                return false;
            run();
            return true;
        };
    };
    noteKeys_.action("edit.delete", Priority::Component, withSelection([this] { deleteSelectedNote(); }));
    noteKeys_.action("edit.selectAll", Priority::Component, withClip([this] { selectAll(); }));
    noteKeys_.action("edit.copy", Priority::Component, withSelection([this] { copy(); }));
    noteKeys_.action("edit.cut", Priority::Component, withSelection([this] { cut(); }));
    noteKeys_.action("edit.paste", Priority::Component, [this](const ActionEvent &) {
        if (!clip() || clipboard_.empty())
            return false;
        paste();
        return true;
    });
    noteKeys_.action("clip.duplicate", Priority::Component, withSelection([this] { duplicate(); }));
    noteKeys_.action("notes.up", Priority::Component, withSelection([this] { transposeSelection(1); }));
    noteKeys_.action("notes.down", Priority::Component, withSelection([this] { transposeSelection(-1); }));
    noteKeys_.action("notes.octaveUp", Priority::Component, withSelection([this] { transposeSelection(12); }));
    noteKeys_.action("notes.octaveDown", Priority::Component, withSelection([this] { transposeSelection(-12); }));
    noteKeys_.action("notes.earlier", Priority::Component, withSelection([this] { nudgeSelection(-snap()); }));
    noteKeys_.action("notes.later", Priority::Component, withSelection([this] { nudgeSelection(snap()); }));
    noteKeys_.action("notes.nudgeEarlier", Priority::Component, withSelection([this] { nudgeSelection(-snap() / 4.0); }));
    noteKeys_.action("notes.nudgeLater", Priority::Component, withSelection([this] { nudgeSelection(snap() / 4.0); }));
    noteKeys_.action("notes.shorter", Priority::Component, withSelection([this] { resizeSelection(-snap()); }));
    noteKeys_.action("notes.longer", Priority::Component, withSelection([this] { resizeSelection(snap()); }));
    auto step = [this](int direction) {
        return [this, direction](const ActionEvent &) {
            if (!clip() || clip()->notes.empty())
                return false;
            Clip *c = editClip();
            const int i = selectNext(c->notes, direction);
            if (i >= 0) {
                const Note &n = c->notes[static_cast<size_t>(i)];
                audition(n.key, n.velocity);
                soundUntil_ = now_ + 0.25;
                // Into view
                const float x = beatX(n.start);
                if (x < grid_.x || x > grid_.x + grid_.w - 20.0f)
                    setScrollX(static_cast<float>(n.start) * pixelsPerBeat_ - grid_.w * 0.3f, true);
                const float y = keyY(n.key);
                if (y < grid_.y || y > grid_.y + grid_.h - rowHeight_)
                    setScrollY(static_cast<float>(rowOf_[n.key]) * rowHeight_ - grid_.h * 0.5f, true);
            }
            return true;
        };
    };
    noteKeys_.action("notes.previous", Priority::Component, step(-1));
    noteKeys_.action("notes.next", Priority::Component, step(1));
    noteKeys_.action("notes.louder", Priority::Component, withSelection([this] { velocitySelection(8.0f / 127.0f); }));
    noteKeys_.action("notes.softer", Priority::Component, withSelection([this] { velocitySelection(-8.0f / 127.0f); }));
    noteKeys_.action("notes.quantize", Priority::Component, withClip([this] {
        edit([this](std::vector<Note> &n) { return quantize(n, snap()); }, "Quantized");
    }));
    noteKeys_.action("notes.deselect", Priority::Component, withSelection([this] { daw::selectAll(editClip()->notes, false); }));
    noteKeys_.action("notes.mute", Priority::Component, withClip([this] { toggleMute(); }));
    noteKeys_.action("notes.loop", Priority::Component, withClip([this] { loopSelection(); }));
    noteKeys_.action("notes.fit", Priority::Component, withClip([this] { fit(); }));
    noteKeys_.action("notes.drawTool", Priority::Component, withClip([this] { tool_ = Tool::Draw; }));
    noteKeys_.action("notes.selectTool", Priority::Component, withClip([this] { tool_ = Tool::Select; }));
    noteKeys_.action("notes.sliceTool", Priority::Component, withClip([this] { tool_ = Tool::Slice; }));
    noteKeys_.action("notes.eraseTool", Priority::Component, withClip([this] { tool_ = Tool::Erase; }));
}

// --- Input ---------------------------------------------------------------------------------

void PianoRollView::startDrag(Drag drag, const Input &input, int anchor)
{
    drag_ = drag;
    anchor_ = anchor;
    dragMoved_ = false;
    pressMouse_ = input.mouse;
    pressBeat_ = xBeat(input.mouse.x);
    pressKey_ = yKey(input.mouse.y);
    pressVelocity_ = lane_.h > 0.0f ? yVelocity(input.mouse.y) : 0.0f;
    pressLane_ = laneHeight_;
    pressLoopStart_ = project().loopStart;
    pressLoopEnd_ = project().loopEnd;
    panX_ = scrollX_;
    panY_ = scrollY_;
    paintX_ = input.mouse.x;
    dragMouse_ = input.mouse;
    lasso_.clear();
    if (const Clip *c = clip()) {
        before_ = c->notes;
        pressBounds_ = noteBounds(c->notes);
        pressClipStart_ = c->start;
        pressClipLength_ = c->length;
    }
}

Rect PianoRollView::boxRect() const
{
    const Vec2 from{pressMouse_.x - (scrollX_ - panX_), pressMouse_.y - (scrollY_ - panY_)};
    const float x0 = std::min(from.x, dragMouse_.x), y0 = std::min(from.y, dragMouse_.y);
    return {x0, y0, std::max(from.x, dragMouse_.x) - x0, std::max(from.y, dragMouse_.y) - y0};
}

void PianoRollView::eraseAt(Vec2 p)
{
    int edge = 0;
    const int i = clip() ? noteAt(p, edge) : -1;
    if (i < 0)
        return;
    Clip *c = editClip();
    c->notes.erase(c->notes.begin() + i);
    ++anchor_; // counts what was erased
    hovered_ = -1;
}

void PianoRollView::pressGrid(const Input &input)
{
    // A press on the grid edits the clip (its selection at least), so it is counted as a change now
    Clip *c = editClip();
    std::vector<Note> &notes = c->notes;
    const bool ctrl = (input.mods & engine::ModCtrl) != 0;
    const bool alt = (input.mods & engine::ModAlt) != 0;
    const bool quick = lastPress_ >= 0.0 && input.time - lastPress_ < 0.3;
    const bool doubleNote = quick && hovered_ >= 0 && hovered_ == lastPressNote_;
    const bool doubleGhost = quick && hovered_ < 0 && hoverGhost_ >= 0 && hoverGhost_ == lastPressGhost_;
    lastPress_ = input.time;
    lastPressNote_ = hovered_;
    lastPressGhost_ = hovered_ < 0 ? hoverGhost_ : -1;

    if (hoverStretch_ != 0) {
        startDrag(Drag::Stretch, input, hoverStretch_);
        return;
    }
    if (doubleGhost) {
        // Edit the ghost's clip, keeping the view where it is on the timeline
        const Ghost g = ghostNotes_[static_cast<size_t>(hoverGhost_)];
        const Track &t = project().tracks[static_cast<size_t>(g.track)];
        const Clip &other = t.clips[static_cast<size_t>(g.clip)];
        const double moved = other.start - c->start;
        const double noteStart = g.start - moved;
        store_.select(g.track, g.clip);
        clipTrack_ = g.track;
        clipIndex_ = g.clip;
        for (Note &n : store_.editClip(g.track, g.clip).notes)
            n.selected = n.key == g.key && std::fabs(n.start - noteStart) < 1e-6;
        setScrollX(scrollX_ - static_cast<float>(moved) * pixelsPerBeat_, false);
        hovered_ = hoverGhost_ = -1;
        lastPressGhost_ = -1;
        message_ = "Editing " + other.name + (g.sameTrack ? "" : " on " + t.name);
        return;
    }
    if (tool_ == Tool::Erase) {
        startDrag(Drag::Erase, input, 0);
        anchor_ = 0;
        eraseAt(input.mouse);
        return;
    }
    if (tool_ == Tool::Slice) {
        if (hovered_ < 0)
            return;
        const double at = snapRound(xBeat(input.mouse.x), freeOf(input));
        const bool group = notes[static_cast<size_t>(hovered_)].selected;
        int cuts = 0;
        const size_t count = notes.size();
        for (size_t i = 0; i < count; ++i) {
            if ((group && notes[i].selected) || static_cast<int>(i) == hovered_)
                cuts += slice(notes, i, at, kMinLength) ? 1 : 0;
        }
        message_ = cuts ? "Cut " + plural(cuts, "note") : std::string("Nothing to cut there");
        hovered_ = -1;
        return;
    }

    if (doubleNote) {
        daw::selectAll(notes, false);
        notes[static_cast<size_t>(hovered_)].selected = true;
        deleteSelectedNote();
        lastPressNote_ = -1;
    } else if (hovered_ >= 0) {
        Note &n = notes[static_cast<size_t>(hovered_)];
        if (input.shift) {
            n.selected = !n.selected; // Shift-click: add or remove, no drag
            return;
        }
        if (!n.selected) {
            daw::selectAll(notes, false);
            n.selected = true;
        }
        int anchor = hovered_;
        if (alt && hoverEdge_ == 0) {
            // Alt-drag: copies of the selection are what move
            const size_t count = notes.size();
            for (size_t i = 0; i < count; ++i) {
                if (notes[i].selected) {
                    notes.push_back(notes[i]);
                    notes[i].selected = false;
                    if (static_cast<int>(i) == hovered_)
                        anchor = static_cast<int>(notes.size()) - 1;
                }
            }
        }
        const Note &held = notes[static_cast<size_t>(anchor)];
        lastLength_ = held.length;
        lastVelocity_ = held.velocity;
        audition(held.key, held.velocity);
        startDrag(hoverEdge_ > 0 ? Drag::Resize : hoverEdge_ < 0 ? Drag::ResizeStart : Drag::Move, input, anchor);
    } else if (tool_ == Tool::Select || ctrl || input.shift) {
        additive_ = input.shift;
        if (!additive_)
            daw::selectAll(notes, false);
        startDrag(alt && tool_ == Tool::Select ? Drag::Lasso : Drag::Box, input, -1);
        lasso_.push_back({xBeat(input.mouse.x), -static_cast<double>((input.mouse.y - grid_.y + scrollY_) / rowHeight_)});
    } else {
        const double start = snapDown(xBeat(input.mouse.x), freeOf(input));
        if (start >= 0.0 && start < c->length) {
            Note n;
            n.start = start;
            n.length = std::min(lastLength_, c->length - start);
            n.key = yKey(input.mouse.y);
            if (inKey_ && scale())
                n.key = snapToScale(n.key, root_, *scale());
            n.velocity = lastVelocity_;
            daw::selectAll(notes, false);
            n.selected = true;
            notes.push_back(n);
            lastPressNote_ = -1;
            audition(n.key, n.velocity);
            startDrag(Drag::Draw, input, static_cast<int>(notes.size()) - 1);
        }
    }
}

void PianoRollView::updateDrag(const Input &input)
{
    const Clip *c = clip();
    dragMouse_ = input.mouse;
    if (std::fabs(input.mouse.x - pressMouse_.x) + std::fabs(input.mouse.y - pressMouse_.y) > kDragStart)
        dragMoved_ = true;
    const bool free = freeOf(input);
    // Dragging notes or a box past an edge scrolls toward it
    const bool edits = drag_ == Drag::Move || drag_ == Drag::Resize || drag_ == Drag::ResizeStart || drag_ == Drag::Box ||
                       drag_ == Drag::Lasso || drag_ == Drag::Draw || drag_ == Drag::Stretch || drag_ == Drag::Erase;
    if (edits && grid_.w > 0.0f) {
        const float speed = 14.0f * std::min(input.dt, 0.05f);
        if (input.mouse.x < grid_.x + kEdge)
            setScrollX(scrollX_ - (grid_.x + kEdge - input.mouse.x) * speed, false);
        else if (input.mouse.x > grid_.x + grid_.w - kEdge)
            setScrollX(scrollX_ + (input.mouse.x - (grid_.x + grid_.w - kEdge)) * speed, false);
        if (drag_ != Drag::Resize && drag_ != Drag::ResizeStart && drag_ != Drag::Stretch) {
            if (input.mouse.y < grid_.y + kEdge)
                setScrollY(scrollY_ - (grid_.y + kEdge - input.mouse.y) * speed, false);
            else if (input.mouse.y > grid_.y + grid_.h - kEdge && input.mouse.y < grid_.y + grid_.h + kEdge * 2.0f)
                setScrollY(scrollY_ + (input.mouse.y - (grid_.y + grid_.h - kEdge)) * speed, false);
        }
    }

    const bool sameCount = c && before_.size() == c->notes.size();
    switch (drag_) {
    case Drag::Keys:
        audition(yKey(std::clamp(input.mouse.y, keys_.y, keys_.y + keys_.h - 1.0f)), 0.8f);
        break;
    case Drag::Ruler:
        if (c && hooks_.seek)
            hooks_.seek(c->start + std::clamp(xBeat(input.mouse.x), 0.0, c->length));
        break;
    case Drag::Loop:
        if (c && dragMoved_) {
            const double a = snapRound(pressBeat_, free) + c->start, b = snapRound(xBeat(input.mouse.x), free) + c->start;
            setLoop(std::min(a, b), std::max(a, b));
        }
        break;
    case Drag::LoopStart:
    case Drag::LoopEnd:
        if (c) {
            const double at = snapRound(xBeat(input.mouse.x), free) + c->start;
            if (drag_ == Drag::LoopStart)
                setLoop(std::min(at, pressLoopEnd_ - kMinLength), pressLoopEnd_);
            else
                setLoop(pressLoopStart_, std::max(at, pressLoopStart_ + kMinLength));
            store_.setLoop(project().loopStart, project().loopEnd, project().looping || hasLoop());
        }
        break;
    case Drag::ClipStart:
        if (c) {
            Clip *w = editClip();
            // From the clip as it was, so dragging back brings notes back
            w->start = pressClipStart_;
            w->length = pressClipLength_;
            w->notes = before_;
            const double at = pressClipStart_ + snapRound(xBeat(input.mouse.x) + (scrollX_ - panX_) / pixelsPerBeat_, free);
            project().trimStart(*w, at, std::max(snap(), kMinLength));
            const float moved = static_cast<float>(w->start - pressClipStart_) * pixelsPerBeat_;
            scrollX_ = targetX_ = panX_ - moved;
        }
        break;
    case Drag::ClipEnd:
        if (c)
            editClip()->length = std::max(std::max(snap(), 0.25), snapRound(xBeat(input.mouse.x), free));
        break;
    case Drag::Mini: {
        const double total = (c ? c->length : 16.0) + kTailBeats + kLeadBeats;
        const double left = (input.mouse.x - grabX_ - mini_.x) / std::max(1.0f, mini_.w) * total - kLeadBeats;
        setScrollX(static_cast<float>(left) * pixelsPerBeat_, false);
        break;
    }
    case Drag::Pan:
        setScrollX(panX_ - (input.mouse.x - pressMouse_.x), false);
        setScrollY(panY_ - (input.mouse.y - pressMouse_.y), false);
        break;
    case Drag::Divider:
        laneHeight_ = std::clamp(pressLane_ - (input.mouse.y - pressMouse_.y), 30.0f, std::max(30.0f, rect_.h * 0.6f));
        break;
    case Drag::Draw: {
        if (!sameCount || anchor_ < 0)
            break;
        Clip *w = editClip();
        Note &n = w->notes[static_cast<size_t>(anchor_)];
        const double step = free ? kMinLength : std::max(snap(), kMinLength);
        if (dragMoved_ && std::fabs(input.mouse.x - pressMouse_.x) > kDragStart) {
            const double end = snapRound(xBeat(input.mouse.x), free);
            n.length = std::clamp(end - n.start, step, w->length - n.start);
        }
        int key = yKey(input.mouse.y);
        if (inKey_ && scale())
            key = snapToScale(key, root_, *scale());
        if (key != n.key) {
            n.key = key;
            audition(n.key, n.velocity);
        }
        break;
    }
    case Drag::Move: {
        if (!sameCount || anchor_ < 0 || anchor_ >= static_cast<int>(before_.size()))
            break;
        const Note &a = before_[static_cast<size_t>(anchor_)];
        // The pressed note lands on the grid (Ctrl: anywhere); the others keep their places
        const double raw = a.start + (xBeat(input.mouse.x) - pressBeat_);
        const double start = snapRound(raw, free);
        double dBeat = start - a.start;
        int key = a.key + (yKey(input.mouse.y) - pressKey_);
        if (inKey_ && scale())
            key = snapToScale(std::clamp(key, 0, 127), root_, *scale());
        int dKey = key - a.key;
        // Keep every moved note inside the clip and the keyboard
        for (const Note &n : before_) {
            if (!n.selected)
                continue;
            dBeat = std::max(dBeat, -n.start);
            dBeat = std::min(dBeat, c->length - kMinLength - n.start);
            dKey = std::clamp(dKey, -n.key, 127 - n.key);
        }
        Clip *w = editClip();
        for (size_t i = 0; i < w->notes.size(); ++i) {
            if (!before_[i].selected)
                continue;
            Note &n = w->notes[i];
            n.start = before_[i].start + dBeat;
            n.key = before_[i].key + dKey;
            if (inKey_ && scale() && static_cast<int>(i) != anchor_)
                n.key = snapToScale(n.key, root_, *scale());
        }
        const int now = w->notes[static_cast<size_t>(anchor_)].key;
        if (now != sounding_)
            audition(now, a.velocity);
        break;
    }
    case Drag::Resize:
    case Drag::ResizeStart: {
        if (!sameCount || anchor_ < 0)
            break;
        const Note &a = before_[static_cast<size_t>(anchor_)];
        const double minimum = free ? kMinLength : std::max(kMinLength, snap());
        Clip *w = editClip();
        if (drag_ == Drag::Resize) {
            const double end = snapRound(xBeat(input.mouse.x), free);
            const double delta = end - (a.start + a.length);
            for (size_t i = 0; i < w->notes.size(); ++i) {
                if (before_[i].selected)
                    w->notes[i].length = std::clamp(before_[i].length + delta, std::min(minimum, before_[i].length), w->length - w->notes[i].start);
            }
        } else {
            const double delta = snapRound(xBeat(input.mouse.x), free) - a.start;
            for (size_t i = 0; i < w->notes.size(); ++i) {
                if (!before_[i].selected)
                    continue;
                const double end = before_[i].start + before_[i].length;
                const double start = std::clamp(before_[i].start + delta, 0.0, end - std::min(minimum, before_[i].length));
                w->notes[i].start = start;
                w->notes[i].length = end - start;
            }
        }
        lastLength_ = w->notes[static_cast<size_t>(anchor_)].length;
        break;
    }
    case Drag::Stretch: {
        if (!sameCount || pressBounds_.count < 2)
            break;
        const double span = std::max(kMinLength, pressBounds_.end - pressBounds_.start);
        const double at = snapRound(xBeat(input.mouse.x), free);
        double factor;
        double anchor;
        if (anchor_ > 0) {
            anchor = pressBounds_.start;
            factor = (at - anchor) / span;
        } else {
            anchor = pressBounds_.end;
            factor = (anchor - at) / span;
        }
        factor = std::clamp(factor, 1.0 / 16.0, 16.0);
        Clip *w = editClip();
        w->notes = before_;
        stretchAround(w->notes, anchor, factor, kMinLength);
        break;
    }
    case Drag::Box: {
        if (!sameCount)
            break;
        Clip *w = editClip();
        const Rect box = boxRect();
        for (size_t i = 0; i < w->notes.size(); ++i)
            w->notes[i].selected = overlaps(noteRect(w->notes[i]), box) || (additive_ && before_[i].selected);
        break;
    }
    case Drag::Lasso: {
        if (!sameCount)
            break;
        // In beats and rows, negated so up is up (folded rows lasso as they look)
        const double row = (input.mouse.y - grid_.y + scrollY_) / rowHeight_;
        const NotePoint point{xBeat(input.mouse.x), -row};
        if (lasso_.empty() || std::fabs(point.beat - lasso_.back().beat) * pixelsPerBeat_ + std::fabs(point.key - lasso_.back().key) * rowHeight_ > 3.0f)
            lasso_.push_back(point);
        // Each note's centre line in the same space
        Clip *w = editClip();
        for (size_t i = 0; i < before_.size(); ++i) {
            bool hit = false;
            const double y = -(static_cast<double>(rowOf_[std::clamp(before_[i].key, 0, 127)]) + 0.5);
            for (double t : {0.0, 0.25, 0.5, 0.75, 1.0})
                hit = hit || (lasso_.size() >= 3 && insidePolygon(lasso_, {before_[i].start + before_[i].length * t, y}));
            w->notes[i].selected = hit || (additive_ && before_[i].selected);
        }
        break;
    }
    case Drag::Velocity: {
        if (!sameCount || anchor_ < 0)
            break;
        const float v = yVelocity(input.mouse.y);
        const float delta = v - before_[static_cast<size_t>(anchor_)].velocity;
        const bool group = before_[static_cast<size_t>(anchor_)].selected;
        Clip *w = editClip();
        for (size_t i = 0; i < w->notes.size(); ++i) {
            if (static_cast<int>(i) == anchor_)
                w->notes[i].velocity = v;
            else if (group && before_[i].selected)
                w->notes[i].velocity = std::clamp(before_[i].velocity + delta, 1.0f / 127.0f, 1.0f);
        }
        lastVelocity_ = v;
        shownVelocity_ = v;
        break;
    }
    case Drag::Paint: {
        if (!c)
            break;
        // Notes whose starts the mouse crossed since last frame take its height; with a
        // selection, only selected ones
        const float v = yVelocity(input.mouse.y);
        const float a = std::min(paintX_, input.mouse.x) - 4.0f, b = std::max(paintX_, input.mouse.x) + 4.0f;
        Clip *w = editClip();
        const bool onlySelected = daw::selectedCount(w->notes) > 0;
        for (Note &n : w->notes) {
            const float x = beatX(n.start) + 4.0f;
            if (x >= a && x <= b && (!onlySelected || n.selected))
                n.velocity = v;
        }
        paintX_ = input.mouse.x;
        shownVelocity_ = v;
        break;
    }
    case Drag::Ramp: {
        if (!sameCount)
            break;
        // A straight line from the press to the mouse: the notes it spans take its height there
        const float v = yVelocity(input.mouse.y);
        const float x0 = std::min(pressMouse_.x, input.mouse.x), x1 = std::max(pressMouse_.x, input.mouse.x);
        const bool onlySelected = daw::selectedCount(before_) > 0;
        Clip *w = editClip();
        for (size_t i = 0; i < w->notes.size(); ++i) {
            w->notes[i].velocity = before_[i].velocity;
            const float x = beatX(w->notes[i].start) + 4.0f;
            if (x < x0 || x > x1 || (onlySelected && !before_[i].selected) || x1 - x0 < 1.0f)
                continue;
            const float t = (x - pressMouse_.x) / (input.mouse.x - pressMouse_.x);
            w->notes[i].velocity = std::clamp(pressVelocity_ + (v - pressVelocity_) * t, 1.0f / 127.0f, 1.0f);
        }
        shownVelocity_ = v;
        break;
    }
    case Drag::Erase:
        eraseAt(input.mouse);
        break;
    case Drag::Point: {
        Clip *w = c ? editClip() : nullptr;
        ControlLane *lane = w ? findLane(w->controls, laneController_) : nullptr;
        if (!lane || anchor_ < 0 || anchor_ >= static_cast<int>(lane->points.size()))
            break;
        // Between its neighbours, so the points keep their order
        const size_t i = static_cast<size_t>(anchor_);
        const double low = i > 0 ? lane->points[i - 1].beat : 0.0;
        const double high = i + 1 < lane->points.size() ? lane->points[i + 1].beat : c->length;
        ControlPoint &p = lane->points[i];
        p.beat = std::clamp(snapRound(xBeat(input.mouse.x), free), low, high);
        p.value = yValue(input.mouse.y);
        shownVelocity_ = p.value;
        break;
    }
    case Drag::CcPaint: {
        if (!c)
            break;
        // Points on the grid from where the mouse was to where it is, along the line between
        Clip *w = editClip();
        ControlLane &lane = laneFor(w->controls, laneController_);
        const double beat = std::clamp(xBeat(input.mouse.x), 0.0, c->length);
        const float value = yValue(input.mouse.y);
        const double step = free || !snapping_ ? 1.0 / 32.0 : std::max(snap(), 1.0 / 64.0);
        const double a = std::min(paintBeat_, beat), b = std::max(paintBeat_, beat);
        for (double g = std::ceil(a / step - 1e-9) * step; g <= b + 1e-9; g += step) {
            const double t = b > a ? (g - paintBeat_) / (beat - paintBeat_) : 1.0;
            setControlPoint(lane, g, paintValue_ + (value - paintValue_) * static_cast<float>(std::clamp(t, 0.0, 1.0)), step * 0.49);
        }
        paintBeat_ = beat;
        paintValue_ = value;
        shownVelocity_ = value;
        break;
    }
    case Drag::CcLine: {
        if (!c)
            break;
        ControlLane &lane = laneFor(editClip()->controls, laneController_);
        lane.points = beforeLane_.points;
        const double from = std::clamp(snapRound(pressBeat_, free), 0.0, c->length);
        const double to = std::clamp(snapRound(xBeat(input.mouse.x), free), 0.0, c->length);
        const float value = yValue(input.mouse.y);
        controlLine(lane, from, pressVelocity_, to, value);
        shownVelocity_ = value;
        break;
    }
    case Drag::CcErase: {
        Clip *w = c ? editClip() : nullptr;
        ControlLane *lane = w ? findLane(w->controls, laneController_) : nullptr;
        const double beat = xBeat(input.mouse.x);
        const double pad = 4.0 / pixelsPerBeat_;
        if (lane)
            anchor_ += eraseControlPoints(*lane, std::min(paintBeat_, beat) - pad, std::max(paintBeat_, beat) + pad);
        paintBeat_ = beat;
        break;
    }
    case Drag::None:
        break;
    }
}

void PianoRollView::finishDrag()
{
    const Clip *c = clip();
    switch (drag_) {
    case Drag::Move:
    case Drag::Resize:
    case Drag::ResizeStart:
    case Drag::Stretch:
        if (c) {
            Clip *w = editClip();
            clampToClip(w->notes, w->length, kMinLength);
            if (drag_ == Drag::Stretch)
                message_ = "Stretched " + plural(daw::selectedCount(w->notes), "note");
        }
        break;
    case Drag::Draw:
        if (c && anchor_ >= 0 && anchor_ < static_cast<int>(c->notes.size())) {
            const Note &n = c->notes[static_cast<size_t>(anchor_)];
            lastLength_ = n.length;
            message_ = "Added " + audio::keyName(n.key);
        }
        break;
    case Drag::Box:
    case Drag::Lasso:
        if (c) {
            const int n = daw::selectedCount(c->notes);
            message_ = n ? plural(n, "note") + " selected" : std::string();
        }
        break;
    case Drag::Erase:
        if (anchor_ > 0)
            message_ = "Erased " + plural(anchor_, "note");
        break;
    case Drag::Loop:
        if (!dragMoved_ && c) {
            // A click: in the loop turns looping on or off; elsewhere the loop goes
            const double at = pressBeat_ + c->start;
            if (hasLoop() && at >= project().loopStart && at <= project().loopEnd)
                store_.setLoop(project().loopStart, project().loopEnd, !project().looping);
            else
                setLoop(0.0, 0.0);
        } else if (hasLoop()) {
            setLooping(true);
        }
        break;
    case Drag::ClipStart:
    case Drag::ClipEnd:
        if (c)
            message_ = "Clip length " + format("%.3g beats", c->length);
        break;
    case Drag::CcPaint:
    case Drag::CcLine:
    case Drag::Point:
    case Drag::CcErase:
        if (c) {
            Clip *w = editClip();
            if (ControlLane *lane = findLane(w->controls, laneController_)) {
                if (drag_ == Drag::CcPaint)
                    thinControlPoints(*lane, 1.0f / 254.0f);
                message_ = (drag_ == Drag::CcErase ? "Erased " : "Edited ") + laneName();
            }
            dropEmptyLanes(w->controls);
        }
        break;
    default:
        break;
    }
    if (drag_ == Drag::Move || drag_ == Drag::Resize || drag_ == Drag::ResizeStart || drag_ == Drag::Draw ||
        drag_ == Drag::Keys)
        stopAudition();
    drag_ = Drag::None;
    before_.clear();
    lasso_.clear();
}

void PianoRollView::hover(const Input &input)
{
    mouse_ = input.mouse;
    overGrid_ = grid_.contains(input.mouse);
    const Clip *c = clip();
    hoverKey_ = (overGrid_ || keys_.contains(input.mouse)) ? yKey(input.mouse.y) : -1;
    hoverBeat_ = overGrid_ ? xBeat(input.mouse.x) : -1.0;
    if (drag_ != Drag::None)
        return;
    hovered_ = -1;
    hoverEdge_ = 0;
    hoverHandle_ = -1;
    hoverGhost_ = -1;
    hoverStretch_ = 0;
    if (!c)
        return;
    if (stretchHandle(-1).contains(input.mouse))
        hoverStretch_ = -1;
    else if (stretchHandle(1).contains(input.mouse))
        hoverStretch_ = 1;
    if (overGrid_ && hoverStretch_ == 0) {
        hovered_ = noteAt(input.mouse, hoverEdge_);
        if (hovered_ < 0)
            hoverGhost_ = ghostAt(input.mouse);
    }
    hoverPoint_ = -1;
    if (velocityLane_ && laneController_ == kVelocityLane)
        hoverHandle_ = handleAt(input.mouse);
    else if (velocityLane_)
        hoverPoint_ = pointAt(input.mouse);
}

void PianoRollView::handleInput(const Input &input)
{
    if (noteKeys_.attach(input.signals))
        makeHooks();
    checkSelection();
    now_ = input.time;
    ctrlHeld_ = (input.mods & engine::ModCtrl) != 0;
    const Clip *c = clip();
    const bool ctrl = ctrlHeld_;
    const bool alt = (input.mods & engine::ModAlt) != 0;
    if (drag_ == Drag::None) {
        rebuildRows();
        clampScroll();
    }
    rebuildGhosts();
    glide(input);

    if (soundUntil_ >= 0.0 && input.time > soundUntil_ && drag_ == Drag::None)
        stopAudition();

    // Follow: page along with the playhead while playing
    if (c && follow_ && drag_ == Drag::None && hooks_.playing && hooks_.playing() && hooks_.position) {
        const double at = hooks_.position() - c->start;
        const float x = beatX(at);
        if (at >= 0.0 && at <= c->length && (x > grid_.x + grid_.w - 16.0f || x < grid_.x))
            setScrollX(static_cast<float>(at) * pixelsPerBeat_ - grid_.w * 0.06f, true);
    }

    // The wheel: keys up and down, time with Shift (or sideways), zoom in time with Ctrl, row
    // height with Alt
    if (rect_.contains(input.mouse)) {
        float across = input.scrollX;
        float down = input.scroll;
        if (ctrl && down != 0.0f) {
            zoomAround(std::pow(1.2f, down), std::clamp(input.mouse.x, grid_.x, grid_.x + grid_.w));
            down = 0.0f;
        } else if (alt && down != 0.0f) {
            zoomRows(std::pow(1.12f, down), std::clamp(input.mouse.y, grid_.y, grid_.y + grid_.h));
            down = 0.0f;
        }
        if (input.shift && across == 0.0f)
            std::swap(across, down);
        if (down != 0.0f)
            setScrollY(targetY_ - down * rowHeight_ * 3.0f, true);
        if (across != 0.0f) {
            if (zooming_)
                setScrollX(scrollX_, false);
            setScrollX(targetX_ - across * 80.0f, true);
        }
        input.scroll = input.scrollX = 0.0f;

        // Middle-drag pans, anywhere on the editor
        if (input.middlePressed && drag_ == Drag::None) {
            input.middlePressed = false;
            if (zooming_)
                setScrollX(scrollX_, false);
            startDrag(Drag::Pan, input, -1);
        }
    }
    if (drag_ == Drag::Pan && !input.middleDown)
        drag_ = Drag::None;

    hover(input);
    if (laneController_ != kVelocityLane) {
        // A controller's value: at the point under the mouse, or where the mouse is in time
        const ControlLane *lane = shownLane();
        if (drag_ == Drag::None) {
            shownVelocity_ = -1.0f;
            if (lane && hoverPoint_ >= 0 && hoverPoint_ < static_cast<int>(lane->points.size()))
                shownVelocity_ = lane->points[static_cast<size_t>(hoverPoint_)].value;
            else if (c && lane_.contains(input.mouse))
                shownVelocity_ = lane ? lane->valueAt(xBeat(input.mouse.x)) : controllerDefault(laneController_);
        }
    } else if (drag_ != Drag::Velocity && drag_ != Drag::Paint && drag_ != Drag::Ramp) {
        shownVelocity_ = -1.0f;
        if (c && hovered_ >= 0 && hovered_ < static_cast<int>(c->notes.size()))
            shownVelocity_ = c->notes[static_cast<size_t>(hovered_)].velocity;
        else if (c && hoverHandle_ >= 0 && hoverHandle_ < static_cast<int>(c->notes.size()))
            shownVelocity_ = c->notes[static_cast<size_t>(hoverHandle_)].velocity;
    }

    // Right-click: the note tools, on the note under the mouse (selecting it if it wasn't)
    if (c && input.secondaryPressed && (grid_.contains(input.mouse) || lane_.contains(input.mouse))) {
        input.secondaryPressed = false;
        if (hovered_ >= 0 && !c->notes[static_cast<size_t>(hovered_)].selected) {
            Clip *w = editClip();
            daw::selectAll(w->notes, false);
            w->notes[static_cast<size_t>(hovered_)].selected = true;
        }
        if (hooks_.contextMenu)
            hooks_.contextMenu(input.mouse);
    }

    if (input.pressed && drag_ == Drag::None) {
        const Rect loop = loopRect();
        if (gridLabel_.contains(input.mouse)) {
            snapping_ = !snapping_;
            message_ = snapping_ ? "Snapping to the grid" : "Not snapping (Ctrl snaps)";
        } else if (velocityLane_ && divider_.contains(input.mouse) && !lane_.contains(input.mouse)) {
            startDrag(Drag::Divider, input, -1);
        } else if (keys_.contains(input.mouse)) {
            startDrag(Drag::Keys, input, -1);
            audition(yKey(input.mouse.y), 0.8f);
        } else if (c && loopBand_.contains(input.mouse)) {
            if (loop.w > 0.0f && std::fabs(input.mouse.x - loop.x) < 6.0f)
                startDrag(Drag::LoopStart, input, -1);
            else if (loop.w > 0.0f && std::fabs(input.mouse.x - (loop.x + loop.w)) < 6.0f)
                startDrag(Drag::LoopEnd, input, -1);
            else
                startDrag(Drag::Loop, input, -1);
        } else if (c && clipHandle(-1).inset(-3.0f).contains(input.mouse)) {
            startDrag(Drag::ClipStart, input, -1);
        } else if (c && clipHandle(1).inset(-3.0f).contains(input.mouse)) {
            startDrag(Drag::ClipEnd, input, -1);
        } else if (c && ruler_.contains(input.mouse)) {
            startDrag(Drag::Ruler, input, -1);
        } else if (mini_.contains(input.mouse)) {
            const Rect t = miniThumb();
            grabX_ = t.contains(input.mouse) ? input.mouse.x - t.x : t.w * 0.5f;
            startDrag(Drag::Mini, input, -1);
        } else if (c && velocityLane_ && laneKeys_.contains(input.mouse)) {
            laneMenuPressed_ = true; // opens on the release, so the release can't pick from it
        } else if (c && lane_.contains(input.mouse) && laneController_ != kVelocityLane) {
            pressLane(input);
        } else if (c && lane_.contains(input.mouse)) {
            if (alt)
                startDrag(Drag::Ramp, input, -1);
            else if (hoverHandle_ >= 0)
                startDrag(Drag::Velocity, input, hoverHandle_);
            else
                startDrag(Drag::Paint, input, -1);
        } else if (c && grid_.contains(input.mouse)) {
            pressGrid(input);
        }
    }

    if (laneMenuPressed_ && !input.down) {
        laneMenuPressed_ = false;
        if (c && laneKeys_.contains(input.mouse) && hooks_.laneMenu)
            hooks_.laneMenu({laneKeys_.x, laneKeys_.y + 26.0f});
    }
    if (!input.down && drag_ != Drag::Pan && drag_ != Drag::None)
        finishDrag();
    if (drag_ != Drag::None)
        updateDrag(input);

    // The pointer says what a press would do
    Cursor cursor = Cursor::Arrow;
    switch (drag_) {
    case Drag::Move:
    case Drag::Pan: cursor = Cursor::Move; break;
    case Drag::Draw:
    case Drag::Resize:
    case Drag::ResizeStart:
    case Drag::Stretch:
    case Drag::Loop:
    case Drag::LoopStart:
    case Drag::LoopEnd:
    case Drag::ClipStart:
    case Drag::ClipEnd: cursor = Cursor::ResizeX; break;
    case Drag::Velocity:
    case Drag::Divider: cursor = Cursor::ResizeY; break;
    case Drag::Point: cursor = Cursor::Move; break;
    case Drag::CcPaint:
    case Drag::CcLine:
    case Drag::CcErase: cursor = Cursor::Crosshair; break;
    case Drag::Keys:
    case Drag::Ruler:
    case Drag::Mini: cursor = Cursor::Hand; break;
    case Drag::Erase:
    case Drag::Paint:
    case Drag::Ramp:
    case Drag::Box:
    case Drag::Lasso: cursor = Cursor::Crosshair; break;
    case Drag::None: {
        const Rect loop = loopRect();
        if (!rect_.contains(input.mouse))
            break;
        if (hoverStretch_ != 0)
            cursor = Cursor::ResizeX;
        else if (overGrid_ && hovered_ >= 0)
            cursor = tool_ == Tool::Slice ? Cursor::IBeam : tool_ == Tool::Erase ? Cursor::Crosshair
                     : hoverEdge_ != 0 ? Cursor::ResizeX : Cursor::Hand;
        else if (overGrid_ && hoverGhost_ >= 0)
            cursor = Cursor::Hand;
        else if (overGrid_)
            cursor = tool_ == Tool::Select ? Cursor::Arrow : tool_ == Tool::Slice ? Cursor::IBeam : Cursor::Crosshair;
        else if (c && loopBand_.contains(input.mouse))
            cursor = loop.w > 0.0f && (std::fabs(input.mouse.x - loop.x) < 6.0f || std::fabs(input.mouse.x - loop.x - loop.w) < 6.0f)
                         ? Cursor::ResizeX : Cursor::Hand;
        else if (c && (clipHandle(-1).inset(-3.0f).contains(input.mouse) || clipHandle(1).inset(-3.0f).contains(input.mouse)))
            cursor = Cursor::ResizeX;
        else if (keys_.contains(input.mouse) || ruler_.contains(input.mouse) || mini_.contains(input.mouse) ||
                 gridLabel_.contains(input.mouse))
            cursor = Cursor::Hand;
        else if (lane_.contains(input.mouse) && laneController_ != kVelocityLane)
            cursor = hoverPoint_ >= 0 && !(input.mods & engine::ModAlt) && tool_ != Tool::Erase ? Cursor::Move : Cursor::Crosshair;
        else if (lane_.contains(input.mouse))
            cursor = hoverHandle_ >= 0 && !(input.mods & engine::ModAlt) ? Cursor::ResizeY : Cursor::Crosshair;
        else if (velocityLane_ && laneKeys_.contains(input.mouse))
            cursor = Cursor::Hand;
        else if (velocityLane_ && divider_.contains(input.mouse))
            cursor = Cursor::ResizeY;
        break;
    }
    }
    if (rect_.contains(input.mouse) || drag_ != Drag::None)
        input.setCursor(cursor);
}

// --- Controller lanes ----------------------------------------------------------------------

std::string PianoRollView::laneName() const
{
    return laneController_ == kVelocityLane ? std::string("Velocity") : controllerName(laneController_);
}

const ControlLane *PianoRollView::shownLane() const
{
    const Clip *c = clip();
    if (!c || laneController_ == kVelocityLane)
        return nullptr;
    for (const ControlLane &l : c->controls) {
        if (l.controller == laneController_)
            return &l;
    }
    return nullptr;
}

void PianoRollView::clearLane()
{
    if (!clip() || laneController_ == kVelocityLane)
        return;
    Clip *c = editClip();
    c->controls.erase(std::remove_if(c->controls.begin(), c->controls.end(),
                                     [this](const ControlLane &l) { return l.controller == laneController_; }),
                      c->controls.end());
    message_ = "Cleared " + laneName();
}

float PianoRollView::yValue(float y) const
{
    return std::clamp((lane_.y + lane_.h - 7.0f - y) / std::max(1.0f, lane_.h - 14.0f), 0.0f, 1.0f);
}

std::string PianoRollView::valueText(float value) const
{
    if (laneController_ == kVelocityLane)
        return format("%.0f", std::round(value * 127.0f));
    if (laneController_ == kPitchBend)
        return format("%+.0f", std::round((value - 0.5f) * 16383.0f));
    if (laneController_ == 64)
        return value >= 0.5f ? "On" : "Off";
    return format("%.0f", std::round(value * 127.0f));
}

int PianoRollView::pointAt(Vec2 p) const
{
    const ControlLane *lane = shownLane();
    if (!lane || !lane_.contains(p))
        return -1;
    int best = -1;
    float distance = 7.0f;
    for (size_t i = 0; i < lane->points.size(); ++i) {
        const float d = std::max(std::fabs(p.x - beatX(lane->points[i].beat)), std::fabs(p.y - velocityY(lane->points[i].value)));
        if (d < distance) {
            distance = d;
            best = static_cast<int>(i);
        }
    }
    return best;
}

void PianoRollView::pressLane(const Input &input)
{
    Clip *c = editClip();
    const bool alt = (input.mods & engine::ModAlt) != 0;
    const bool quick = lastPress_ >= 0.0 && input.time - lastPress_ < 0.3;
    lastPress_ = input.time;
    lastPressNote_ = -1;
    ControlLane *lane = findLane(c->controls, laneController_);
    if (tool_ == Tool::Erase) {
        startDrag(Drag::CcErase, input, 0);
        anchor_ = 0;
        paintBeat_ = xBeat(input.mouse.x);
        return;
    }
    if (hoverPoint_ >= 0 && lane && !alt) {
        if (quick && hoverPoint_ == lastPressGhost_) {
            // A double-click deletes the point
            lane->points.erase(lane->points.begin() + hoverPoint_);
            dropEmptyLanes(c->controls);
            hoverPoint_ = lastPressGhost_ = -1;
            message_ = "Deleted a " + laneName() + " point";
            return;
        }
        lastPressGhost_ = hoverPoint_;
        startDrag(Drag::Point, input, hoverPoint_);
        return;
    }
    lastPressGhost_ = -1;
    if (alt) {
        // A straight line from here, over the lane as it was
        startDrag(Drag::CcLine, input, -1);
        beforeLane_ = lane ? *lane : ControlLane{laneController_, {}};
        pressVelocity_ = yValue(input.mouse.y);
        return;
    }
    startDrag(Drag::CcPaint, input, -1);
    paintBeat_ = std::clamp(xBeat(input.mouse.x), 0.0, c->length);
    paintValue_ = yValue(input.mouse.y);
    setControlPoint(laneFor(c->controls, laneController_), snapRound(paintBeat_, freeOf(input) || !snapping_), paintValue_,
                    std::max(snap(), 1.0 / 64.0) * 0.49);
    shownVelocity_ = paintValue_;
}

void PianoRollView::drawControls(DrawList &list, const Theme &theme, const Clip &c) const
{
    const ControlLane *lane = nullptr;
    for (const ControlLane &l : c.controls) {
        if (l.controller == laneController_)
            lane = &l;
    }
    // The bend's middle, where it rests
    if (laneController_ == kPitchBend || laneController_ == 10)
        list.add(theme.ink({lane_.x, velocityY(0.5f), lane_.w, 1.0f}, Shape::Rounded, TextTone::Secondary, true, 0.35f));
    if (!lane || lane->points.empty()) {
        list.add(theme.text("Drag to draw " + laneName() + "  /  Alt-drag for a line", lane_, TextStyle::Caption, TextTone::Muted,
                            TextAlign::Center));
        return;
    }
    // The curve: one value a pixel, so a stepped controller's steps stay near-vertical. A plot maps
    // 0-1 over the lane's height the way velocityY does, and its fill runs from the resting place.
    const float base = laneController_ == kPitchBend || laneController_ == 10 ? velocityY(0.5f) : lane_.y + lane_.h - 4.0f;
    const float from = std::max(lane_.x, beatX(0.0)), to = std::min(lane_.x + lane_.w, beatX(c.length));
    std::vector<float> values;
    for (float x = from; x < to; x += 1.0f)
        values.push_back(lane->valueAt(xBeat(x)));
    if (values.size() >= 2) {
        const float span = lane_.h - 14.0f;
        const float lastX = from + static_cast<float>(values.size() - 1);
        const float baseline = (lane_.y + lane_.h - 7.0f - base) / span;
        const Rect area{from, velocityY(1.0f), lastX - from, velocityY(0.0f) - velocityY(1.0f)};
        ui::Plot fill = theme.accentPlot(area, ui::PlotMode::Fill, 0.13f);
        fill.values = values.data();
        fill.count = static_cast<uint32_t>(values.size());
        fill.low = 0.0f;
        fill.high = 1.0f;
        fill.baseline = baseline;
        list.add(fill);
        ui::Plot line = theme.accentPlot(area, ui::PlotMode::Line, 0.9f);
        line.values = values.data();
        line.count = static_cast<uint32_t>(values.size());
        line.low = 0.0f;
        line.high = 1.0f;
        line.width = 2.0f;
        list.add(line);
    }
    // Its points: small raised knobs with accent cores
    for (size_t i = 0; i < lane->points.size(); ++i) {
        const float x = beatX(lane->points[i].beat), y = velocityY(lane->points[i].value);
        if (x < lane_.x - 8.0f || x > lane_.x + lane_.w + 8.0f)
            continue;
        const bool hot = static_cast<int>(i) == hoverPoint_ || (drag_ == Drag::Point && static_cast<int>(i) == anchor_);
        const float d = hot ? 12.0f : 8.0f;
        list.add(theme.surface({x - d * 0.5f, y - d * 0.5f, d, d}, Shape::Circle, Elevation::Raised, hot ? 0.7f : 0.4f, Finish::Convex));
        list.add(theme.accent({x - d * 0.5f + 2.0f, y - d * 0.5f + 2.0f, d - 4.0f, d - 4.0f}, Shape::Circle, hot ? 1.0f : 0.8f));
    }
}

// --- View settings -------------------------------------------------------------------------

bool PianoRollView::ViewSettings::operator==(const ViewSettings &o) const
{
    return grid == o.grid && triplet == o.triplet && snapping == o.snapping && ghosts == o.ghosts && fold == o.fold &&
           laneShown == o.laneShown && lane == o.lane && laneHeight == o.laneHeight && audition == o.audition &&
           follow == o.follow && inKey == o.inKey && scale == o.scale && root == o.root && zoom == o.zoom &&
           rowHeight == o.rowHeight;
}

PianoRollView::ViewSettings PianoRollView::view() const
{
    ViewSettings v;
    v.grid = gridChoice_;
    v.triplet = triplet_;
    v.snapping = snapping_;
    v.ghosts = ghosts_;
    v.fold = fold_;
    v.laneShown = velocityLane_;
    v.lane = laneController_;
    v.laneHeight = laneHeight_;
    v.audition = auditionOn_;
    v.follow = follow_;
    v.inKey = inKey_;
    v.scale = scale_;
    v.root = root_;
    v.zoom = zooming_ ? targetZoom_ : pixelsPerBeat_;
    v.rowHeight = rowHeight_;
    return v;
}

void PianoRollView::setView(const ViewSettings &v)
{
    gridChoice_ = std::clamp(v.grid, 0, static_cast<int>(gridChoices().size()) - 1);
    triplet_ = v.triplet;
    snapping_ = v.snapping;
    ghosts_ = v.ghosts;
    fold_ = v.fold;
    velocityLane_ = v.laneShown;
    laneController_ = v.lane >= kVelocityLane && v.lane <= kPitchBend ? v.lane : kVelocityLane;
    laneHeight_ = std::clamp(v.laneHeight, 30.0f, 400.0f);
    auditionOn_ = v.audition;
    follow_ = v.follow;
    inKey_ = v.inKey;
    scale_ = v.scale >= -1 && v.scale < static_cast<int>(scales().size()) ? v.scale : -1;
    setRoot(v.root);
    pixelsPerBeat_ = targetZoom_ = std::clamp(v.zoom, 6.0f, 600.0f);
    zooming_ = false;
    rowHeight_ = std::clamp(v.rowHeight, 7.0f, 36.0f);
    rebuildRows();
}

std::string PianoRollView::writeView(const ViewSettings &v)
{
    std::string out;
    auto put = [&out](const char *key, double value) {
        out += (out.empty() ? "" : " ") + std::string(key) + "=" + format("%g", value);
    };
    put("grid", v.grid);
    put("triplet", v.triplet);
    put("snap", v.snapping);
    put("ghosts", static_cast<int>(v.ghosts));
    put("fold", v.fold);
    put("laneShown", v.laneShown);
    put("lane", v.lane);
    put("laneHeight", v.laneHeight);
    put("hear", v.audition);
    put("follow", v.follow);
    put("inKey", v.inKey);
    put("scale", v.scale);
    put("root", v.root);
    put("zoom", v.zoom);
    put("rowHeight", v.rowHeight);
    return out;
}

PianoRollView::ViewSettings PianoRollView::readView(const std::string &text)
{
    ViewSettings v;
    size_t i = 0;
    while (i < text.size()) {
        const size_t end = std::min(text.find(' ', i), text.size());
        const std::string pair = text.substr(i, end - i);
        i = end + 1;
        const size_t eq = pair.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = pair.substr(0, eq);
        char *stop = nullptr;
        const double value = std::strtod(pair.c_str() + eq + 1, &stop);
        if (stop == pair.c_str() + eq + 1 || !std::isfinite(value))
            continue;
        const int whole = static_cast<int>(std::lround(std::clamp(value, -1e6, 1e6)));
        if (key == "grid") v.grid = whole;
        else if (key == "triplet") v.triplet = whole != 0;
        else if (key == "snap") v.snapping = whole != 0;
        else if (key == "ghosts") v.ghosts = static_cast<Ghosts>(std::clamp(whole, 0, 2));
        else if (key == "fold") v.fold = whole != 0;
        else if (key == "laneShown") v.laneShown = whole != 0;
        else if (key == "lane") v.lane = whole;
        else if (key == "laneHeight") v.laneHeight = static_cast<float>(value);
        else if (key == "hear") v.audition = whole != 0;
        else if (key == "follow") v.follow = whole != 0;
        else if (key == "inKey") v.inKey = whole != 0;
        else if (key == "scale") v.scale = whole;
        else if (key == "root") v.root = whole;
        else if (key == "zoom") v.zoom = static_cast<float>(value);
        else if (key == "rowHeight") v.rowHeight = static_cast<float>(value);
    }
    return v;
}

// --- Drawing -------------------------------------------------------------------------------

std::string PianoRollView::readout() const
{
    const Clip *c = clip();
    if (!c || !dragMoved_)
        return {};
    const int bpb = std::max(1, project().beatsPerBar);
    const Note *a = anchor_ >= 0 && anchor_ < static_cast<int>(c->notes.size()) ? &c->notes[static_cast<size_t>(anchor_)] : nullptr;
    switch (drag_) {
    case Drag::Move:
        return a ? audio::keyName(a->key) + "   " + barsBeats(c->start + a->start, bpb) : std::string();
    case Drag::Draw:
        return a ? audio::keyName(a->key) + "   " + lengthText(a->length, bpb) : std::string();
    case Drag::Resize:
    case Drag::ResizeStart:
        return a ? "Length " + lengthText(a->length, bpb) : std::string();
    case Drag::Stretch: {
        const NoteBounds b = noteBounds(c->notes);
        const double span = pressBounds_.end - pressBounds_.start;
        return span > 0.0 ? format("%.0f%%", (b.end - b.start) / span * 100.0) : std::string();
    }
    case Drag::Velocity:
    case Drag::Paint:
    case Drag::Ramp: return format("Velocity %.0f", std::round(shownVelocity_ * 127.0f));
    case Drag::Point:
    case Drag::CcPaint:
    case Drag::CcLine: return laneName() + "  " + valueText(shownVelocity_);
    case Drag::Loop:
    case Drag::LoopStart:
    case Drag::LoopEnd: return hasLoop() ? "Loop " + barsBeats(project().loopStart, bpb) + " - " + barsBeats(project().loopEnd, bpb) : std::string();
    case Drag::ClipStart:
    case Drag::ClipEnd: return "Clip " + barsBeats(c->start, bpb) + ",  " + format("%.3g beats", c->length);
    default: return {};
    }
}

void PianoRollView::drawKeyboard(DrawList &list, const Theme &theme, const Clip *c) const
{
    const Scale *s = scale();
    // Keys sounding under the playhead light up while it plays
    bool playing[128] = {};
    if (c && hooks_.playing && hooks_.playing() && hooks_.position) {
        const double at = hooks_.position() - c->start;
        for (const Note &n : c->notes) {
            if (!n.muted && at >= n.start && at < n.start + n.length && n.start < c->length)
                playing[std::clamp(n.key, 0, 127)] = true;
        }
    }

    // A raised, domed strip; black keys pressed into it
    list.add(theme.surface(keys_, Shape::Soft, Elevation::Raised, 0.55f, Finish::Convex));
    list.pushClip(keys_);
    const int firstRow = std::max(0, static_cast<int>(std::floor(scrollY_ / rowHeight_)));
    const int lastRow = std::min(static_cast<int>(rows_.size()) - 1, static_cast<int>((scrollY_ + grid_.h) / rowHeight_));
    for (int row = firstRow; row <= lastRow; ++row) {
        const int key = rows_[static_cast<size_t>(row)];
        const float y = grid_.y + static_cast<float>(row) * rowHeight_ - scrollY_;
        const Rect pad{keys_.x + 4.0f, y + 1.5f, keys_.w - 8.0f, rowHeight_ - 3.0f};
        const bool out = s && !inScale(key, root_, *s);
        const bool lit = key == sounding_ || playing[key];
        if (isBlack(key))
            list.add(theme.surface({pad.x, pad.y, keys_.w * 0.6f, pad.h}, Shape::Soft, Elevation::Inset, 0.55f, Finish::Concave));
        else if (key % 12 == 0 || key % 12 == 5)
            list.add(theme.ink({keys_.x + 6.0f, y + rowHeight_ - 0.5f, keys_.w - 12.0f, 1.0f}, Shape::Rounded, TextTone::Muted, true,
                               key % 12 == 0 ? 0.35f : 0.15f));
        if (lit)
            list.add(theme.accent(pad, Shape::Soft, 0.85f));
        else if (key == hoverKey_)
            list.add(theme.accent(pad, Shape::Soft, 0.2f));
        if (out)
            list.add(theme.ink({keys_.x + 2.0f, y, keys_.w - 4.0f, rowHeight_}, Shape::Rounded, TextTone::Muted, true, 0.12f));
        if (s && key % 12 == root_ && !lit)
            list.add(theme.accent({keys_.x + keys_.w - 7.0f, y + 2.5f, 3.0f, rowHeight_ - 5.0f}, Shape::Pill, 0.9f));
        const bool named = key % 12 == 0 || (s && key % 12 == root_) || (rowHeight_ >= 15.0f && !isBlack(key)) || key == hoverKey_ || lit;
        if (named && rowHeight_ >= 10.0f) {
            const TextTone tone = lit ? TextTone::OnAccent : key == hoverKey_ ? TextTone::Accent
                                  : s && key % 12 == root_ ? TextTone::Accent : key % 12 == 0 ? TextTone::Secondary : TextTone::Muted;
            list.add(theme.text(audio::keyName(key), {keys_.x, y, keys_.w - 13.0f, rowHeight_}, TextStyle::Caption, tone, TextAlign::End));
        }
    }
    list.popClip();
}

void PianoRollView::drawGrid(DrawList &list, const Theme &theme, const Clip &c) const
{
    const Scale *s = scale();
    const int beatsPerBar = std::max(1, project().beatsPerBar);
    const int firstRow = std::max(0, static_cast<int>(std::floor(scrollY_ / rowHeight_)));
    const int lastRow = std::min(static_cast<int>(rows_.size()) - 1, static_cast<int>((scrollY_ + grid_.h) / rowHeight_));
    for (int row = firstRow; row <= lastRow; ++row) {
        const int key = rows_[static_cast<size_t>(row)];
        const float y = grid_.y + static_cast<float>(row) * rowHeight_ - scrollY_;
        const Rect band{grid_.x, y, grid_.w, rowHeight_};
        if (s) {
            // Outside the scale shaded; the root's rows tinted
            if (!inScale(key, root_, *s))
                list.add(theme.ink(band, Shape::Rounded, TextTone::Muted, true, 0.10f));
            else if (key % 12 == root_)
                list.add(theme.accent(band, Shape::Rounded, 0.07f));
        } else if (isBlack(key)) {
            list.add(theme.ink(band, Shape::Rounded, TextTone::Muted, true, 0.07f));
        }
        if (key == hoverKey_ && (drag_ == Drag::None || drag_ == Drag::Move || drag_ == Drag::Draw))
            list.add(theme.accent(band, Shape::Rounded, 0.06f));
        if (key % 12 == 0)
            list.add(theme.ink({grid_.x, y + rowHeight_ - 1.0f, grid_.w, 1.0f}, Shape::Rounded, TextTone::Muted, true, 0.3f));
    }

    // The loop's range, faintly across the grid
    if (hasLoop()) {
        const float x0 = beatX(project().loopStart - c.start), x1 = beatX(project().loopEnd - c.start);
        list.add(theme.accent({x0, grid_.y, x1 - x0, grid_.h}, Shape::Rounded, project().looping ? 0.035f : 0.018f));
    }

    // Lines: bars, beats and the grid (doubled until they stay apart)
    double step = snap();
    while (pixelsPerBeat_ * step < 9.0f)
        step *= 2.0;
    const double first = std::floor(xBeat(grid_.x) / step) * step;
    const double last = xBeat(grid_.x + grid_.w) + step;
    for (double b = first; b <= last; b += step) {
        const float x = beatX(b);
        const double absolute = c.start + b;
        const double inBar = std::fmod(std::fmod(absolute, static_cast<double>(beatsPerBar)) + beatsPerBar, beatsPerBar);
        const bool bar = inBar < 1e-6 || beatsPerBar - inBar < 1e-6;
        const bool beat = std::fabs(absolute - std::round(absolute)) < 1e-6;
        list.add(theme.ink({x, grid_.y, 1.0f, grid_.h}, Shape::Rounded, TextTone::Muted, true, bar ? 0.4f : beat ? 0.16f : 0.07f));
    }

    // Outside the clip, notes don't play
    const float start = beatX(0.0), end = beatX(c.length);
    if (start > grid_.x)
        list.add(theme.scrim({grid_.x, grid_.y, start - grid_.x, grid_.h}, 0.55f));
    if (end < grid_.x + grid_.w)
        list.add(theme.scrim({end, grid_.y, grid_.x + grid_.w - end, grid_.h}, 0.55f));
    for (float x : {start, end}) {
        if (x >= grid_.x && x <= grid_.x + grid_.w)
            list.add(theme.accent({x - 1.0f, grid_.y, 2.0f, grid_.h}, Shape::Rounded, 0.35f));
    }
}

void PianoRollView::drawNotes(DrawList &list, const Theme &theme, const Clip &c) const
{
    const Rect view{grid_.x - 2.0f, grid_.y - 2.0f, grid_.w + 4.0f, grid_.h + 4.0f};
    // Ghosts: other clips' notes, faint and hollow
    if (ghosts_ != Ghosts::Off) {
        for (size_t i = 0; i < ghostNotes_.size(); ++i) {
            const Ghost &g = ghostNotes_[i];
            const Rect r{beatX(g.start), keyY(g.key) + 1.0f, std::max(4.0f, static_cast<float>(g.length) * pixelsPerBeat_ - 1.0f),
                         rowHeight_ - 2.0f};
            if (!overlaps(r, view))
                continue;
            const bool hot = static_cast<int>(i) == hoverGhost_;
            list.add(theme.ink(r, Shape::Soft, TextTone::Secondary, true, hot ? 0.28f : g.sameTrack ? 0.13f : 0.08f));
            list.add(theme.ink({r.x + 1.0f, r.y, r.w - 2.0f, 1.0f}, Shape::Rounded, TextTone::Secondary, true, hot ? 0.6f : 0.3f));
        }
    }

    const float px = theme.fontSize(TextStyle::Caption);
    const double sliceAt = tool_ == Tool::Slice && hovered_ >= 0 ? snapRound(hoverBeat_, snapping_ ? ctrlHeld_ : !ctrlHeld_) : -1.0;
    for (size_t i = 0; i < c.notes.size(); ++i) {
        const Note &n = c.notes[i];
        const Rect r = noteRect(n);
        if (!overlaps(r, view))
            continue;
        const bool hovered = static_cast<int>(i) == hovered_ || static_cast<int>(i) == hoverHandle_;
        const bool past = n.start >= c.length;
        if (n.muted) {
            // Muted: a hollow well with an accent outline
            list.add(theme.surface(r, Shape::Soft, Elevation::Inset, 0.6f));
            outline(list, theme, r, n.selected ? 0.9f : hovered ? 0.6f : 0.35f, n.selected ? 1.5f : 1.0f);
        } else {
            // A raised, domed pill, its fill stronger the louder it is
            list.add(theme.surface(r, Shape::Soft, Elevation::Raised, n.selected ? 0.8f : 0.4f, Finish::Convex));
            const float fill = n.selected ? 1.0f : 0.3f + 0.5f * n.velocity + (hovered ? 0.15f : 0.0f);
            list.add(theme.accent(r, Shape::Soft, past ? fill * 0.4f : fill));
            // A sheen along the top, so the fill reads as domed
            if (r.h >= 6.0f)
                list.add(theme.ink({r.x + 2.0f, r.y + 1.0f, std::max(1.0f, r.w - 4.0f), std::max(1.0f, r.h * 0.3f)}, Shape::Pill,
                                   TextTone::OnAccent, true, n.selected ? 0.22f : 0.12f));
            if (n.selected) {
                list.add(theme.ink({r.x + 1.0f, r.y, r.w - 2.0f, 1.5f}, Shape::Pill, TextTone::OnAccent, true, 0.9f));
                list.add(theme.ink({r.x + 1.0f, r.y + r.h - 1.5f, r.w - 2.0f, 1.5f}, Shape::Pill, TextTone::OnAccent, true, 0.45f));
            }
            // Its velocity as a thin bar inside
            if (r.w > 8.0f && r.h >= 16.0f)
                list.add(theme.ink({r.x + 2.0f, r.y + r.h - 3.0f, std::max(2.0f, (r.w - 4.0f) * n.velocity), 1.5f}, Shape::Pill,
                                   TextTone::OnAccent, true, n.selected ? 0.55f : 0.35f));
        }
        // Grips at the edge the mouse is on
        if (static_cast<int>(i) == hovered_ && hoverEdge_ != 0 && (tool_ == Tool::Draw || tool_ == Tool::Select)) {
            const float x = hoverEdge_ > 0 ? r.x + r.w - 4.0f : r.x + 2.0f;
            list.add(theme.ink({x, r.y + 2.0f, 2.0f, r.h - 4.0f}, Shape::Pill, n.muted ? TextTone::Accent : TextTone::OnAccent));
        }
        if (static_cast<int>(i) == hovered_ && tool_ == Tool::Erase)
            list.add(theme.scrim(r, 0.6f));
        if (static_cast<int>(i) == hovered_ && sliceAt > n.start && sliceAt < n.start + n.length) {
            const float x = beatX(sliceAt);
            list.add(theme.ink({x - 1.0f, r.y - 3.0f, 2.0f, r.h + 6.0f}, Shape::Pill, TextTone::Primary, true, 0.9f));
        }
        if (r.w > 26.0f && rowHeight_ >= px - 1.0f)
            list.add(theme.text(audio::keyName(n.key), {r.x + 5.0f, r.y - 1.0f, r.w - 8.0f, r.h}, TextStyle::Caption,
                                n.muted ? TextTone::Muted : TextTone::OnAccent, TextAlign::Start));
    }

    // Draw tool: where a press would put a note
    if (tool_ == Tool::Draw && drag_ == Drag::None && overGrid_ && hovered_ < 0 && hoverGhost_ < 0 && hoverStretch_ == 0 &&
        !ctrlHeld_ && hoverBeat_ >= 0.0) {
        const double start = snapDown(hoverBeat_);
        if (start < c.length) {
            const Rect r{beatX(start), keyY(inKey_ && scale() ? snapToScale(hoverKey_, root_, *scale()) : hoverKey_) + 1.0f,
                         std::max(4.0f, static_cast<float>(std::min(lastLength_, c.length - start)) * pixelsPerBeat_ - 1.0f),
                         rowHeight_ - 2.0f};
            list.add(theme.accent(r, Shape::Soft, 0.16f));
            outline(list, theme, r, 0.45f);
        }
    }

    // The selection's bounds and its stretch handles
    const Rect bounds = boundsRect();
    if (bounds.w > 0.0f && (tool_ == Tool::Draw || tool_ == Tool::Select) && drag_ != Drag::Box && drag_ != Drag::Lasso) {
        outline(list, theme, bounds, drag_ == Drag::Stretch ? 0.5f : 0.22f);
        for (int side : {-1, 1}) {
            const Rect h = stretchHandle(side);
            const bool hot = hoverStretch_ == side || (drag_ == Drag::Stretch && anchor_ == side);
            list.add(theme.surface(h, Shape::Pill, Elevation::Raised, hot ? 0.8f : 0.5f, Finish::Convex));
            list.add(theme.accent(h.inset(2.0f), Shape::Pill, hot ? 0.95f : 0.45f));
        }
    }

    // The box or lasso being dragged
    if (drag_ == Drag::Box) {
        const Rect box = boxRect();
        list.add(theme.accent(box, Shape::Soft, 0.12f));
        outline(list, theme, box, 0.7f);
    } else if (drag_ == Drag::Lasso && !lasso_.empty()) {
        auto toScreen = [this](const NotePoint &p) {
            return Vec2{beatX(p.beat), grid_.y + static_cast<float>(-p.key) * rowHeight_ - scrollY_};
        };
        for (size_t i = 0; i < lasso_.size(); ++i) {
            const Vec2 a = toScreen(lasso_[i]);
            const Vec2 b = toScreen(lasso_[(i + 1) % lasso_.size()]);
            const float length = std::max(1.0f, std::hypot(b.x - a.x, b.y - a.y));
            const bool closing = i + 1 == lasso_.size();
            for (float t = 0.0f; t < length; t += closing ? 7.0f : 2.0f) {
                const float x = a.x + (b.x - a.x) * t / length, y = a.y + (b.y - a.y) * t / length;
                list.add(theme.accent({x - 1.0f, y - 1.0f, 2.0f, 2.0f}, Shape::Circle, closing ? 0.4f : 0.85f));
            }
        }
    }
}

void PianoRollView::drawRuler(DrawList &list, const Theme &theme, const Clip &c) const
{
    const int beatsPerBar = std::max(1, project().beatsPerBar);
    // The loop strip: an inset groove with the range as an accent band
    list.add(theme.surface(loopBand_, Shape::Pill, Elevation::Inset, 0.4f));
    list.pushClip(ruler_);
    const Rect loop = loopRect();
    if (loop.w > 0.0f) {
        list.add(theme.surface(loop, Shape::Pill, Elevation::Raised, 0.45f, Finish::Convex));
        list.add(theme.accent(loop, Shape::Pill, project().looping ? 0.85f : 0.3f));
        for (float x : {loop.x + 3.0f, loop.x + loop.w - 5.0f})
            list.add(theme.ink({x, loop.y + 2.0f, 2.0f, loop.h - 4.0f}, Shape::Pill, TextTone::OnAccent, true, project().looping ? 0.8f : 0.5f));
    }

    const float rulerBottom = ruler_.y + ruler_.h;
    const double first = std::floor(xBeat(ruler_.x));
    const double last = xBeat(ruler_.x + ruler_.w) + 1.0;
    for (double b = first; b <= last; b += 1.0) {
        const float x = beatX(b);
        const double absolute = c.start + b;
        if (absolute < -1e-9)
            continue;
        const bool bar = std::fmod(absolute + 1e-9, static_cast<double>(beatsPerBar)) < 1e-6;
        if (!bar && pixelsPerBeat_ < 14.0f)
            continue;
        const bool inside = b >= -1e-9 && b <= c.length + 1e-9;
        list.add(theme.ink({x, rulerBottom - (bar ? 11.0f : 5.0f), 1.0f, bar ? 9.0f : 3.0f}, Shape::Rounded, TextTone::Muted, true,
                           (bar ? 0.8f : 0.6f) * (inside ? 1.0f : 0.5f)));
        // Bar numbers (every other one when they crowd)
        const int barNumber = static_cast<int>(std::floor(absolute / beatsPerBar + 1e-9)) + 1;
        if (bar && (pixelsPerBeat_ * beatsPerBar >= 28.0f || barNumber % 2 == 1))
            list.add(theme.text(std::to_string(barNumber), {x + (std::fabs(b) < 1e-9 ? 13.0f : 4.0f), loopBand_.y + loopBand_.h, 40.0f,
                                                             rulerBottom - loopBand_.y - loopBand_.h - 3.0f},
                                TextStyle::Caption, inside ? TextTone::Secondary : TextTone::Muted, TextAlign::Start));
    }
    // The clip's ends: raised flags to drag
    for (int side : {-1, 1}) {
        const Rect h = clipHandle(side);
        const bool hot = drag_ == (side < 0 ? Drag::ClipStart : Drag::ClipEnd) ||
                         (drag_ == Drag::None && h.inset(-3.0f).contains(mouse_));
        list.add(theme.surface(h, Shape::Soft, Elevation::Raised, hot ? 0.75f : 0.45f, Finish::Convex));
        list.add(theme.accent({side < 0 ? h.x + 2.0f : h.x + h.w - 4.0f, h.y + 2.0f, 2.0f, h.h - 4.0f}, Shape::Pill, hot ? 1.0f : 0.6f));
    }
    list.popClip();
}

void PianoRollView::drawLane(DrawList &list, const Theme &theme, const Clip &c) const
{
    // The divider's grip
    if (drag_ == Drag::Divider || (drag_ == Drag::None && divider_.contains(mouse_) && !lane_.contains(mouse_)))
        list.add(theme.accent({grid_.x + grid_.w * 0.5f - 24.0f, divider_.y + divider_.h * 0.5f - 1.5f, 48.0f, 3.0f}, Shape::Pill, 0.7f));
    else
        list.add(theme.ink({grid_.x + grid_.w * 0.5f - 16.0f, divider_.y + divider_.h * 0.5f - 1.0f, 32.0f, 2.0f}, Shape::Pill,
                           TextTone::Muted, true, 0.35f));

    // The header: the lane's name over a chevron (it opens the menu of lanes), and the value
    const bool menuHot = drag_ == Drag::None && laneKeys_.contains(mouse_);
    list.add(theme.surface(laneKeys_, Shape::Soft, Elevation::Raised, menuHot ? 0.75f : 0.55f, Finish::Convex));
    const Rect name{laneKeys_.x, laneKeys_.y + 4.0f, laneKeys_.w, 18.0f};
    list.add(theme.text(laneName(), name, TextStyle::Caption, menuHot ? TextTone::Accent : TextTone::Secondary, TextAlign::Center));
    list.add(theme.ink({laneKeys_.x + laneKeys_.w - 15.0f, laneKeys_.y + laneKeys_.h - 12.0f, 8.0f, 5.0f}, Shape::TriangleDown,
                       menuHot ? TextTone::Accent : TextTone::Muted));
    const std::string value = shownVelocity_ >= 0.0f ? valueText(shownVelocity_) : std::string("");
    if (lane_.h > 46.0f)
        list.add(theme.text(value, {laneKeys_.x, laneKeys_.y + 22.0f, laneKeys_.w, laneKeys_.h - 26.0f}, TextStyle::Title,
                            TextTone::Accent, TextAlign::Center));
    list.add(theme.surface(lane_, Shape::Soft, Elevation::Inset, 0.6f));
    list.pushClip(lane_.inset(2.0f));
    for (float g : {0.25f, 0.5f, 0.75f, 1.0f})
        list.add(theme.ink({lane_.x, velocityY(g), lane_.w, 1.0f}, Shape::Rounded, TextTone::Muted, true, g == 1.0f ? 0.2f : 0.1f));
    const float start = beatX(0.0), end = beatX(c.length);
    if (start > lane_.x)
        list.add(theme.scrim({lane_.x, lane_.y, start - lane_.x, lane_.h}, 0.55f));
    if (end < lane_.x + lane_.w)
        list.add(theme.scrim({end, lane_.y, lane_.x + lane_.w - end, lane_.h}, 0.55f));
    if (laneController_ != kVelocityLane) {
        drawControls(list, theme, c);
        list.popClip();
        return;
    }
    const bool small = lane_.h < 50.0f;
    for (size_t i = 0; i < c.notes.size(); ++i) {
        const Note &n = c.notes[i];
        const float x = beatX(n.start) + 4.0f;
        if (x < lane_.x - 8.0f || x > lane_.x + lane_.w + 8.0f)
            continue;
        const float y = velocityY(n.velocity);
        const bool hot = static_cast<int>(i) == hoverHandle_ || static_cast<int>(i) == hovered_ ||
                         (drag_ == Drag::Velocity && static_cast<int>(i) == anchor_);
        const float alpha = n.muted ? 0.3f : n.selected || hot ? 1.0f : 0.5f;
        list.add(theme.accent({x - 1.0f, y, 2.0f, lane_.y + lane_.h - 4.0f - y}, Shape::Pill, alpha * 0.6f));
        // A raised knob with an accent core
        const float d = hot ? 13.0f : small ? 9.0f : 11.0f;
        list.add(theme.surface({x - d * 0.5f, y - d * 0.5f, d, d}, Shape::Circle, Elevation::Raised, hot ? 0.7f : 0.4f, Finish::Convex));
        list.add(theme.accent({x - d * 0.5f + 2.5f, y - d * 0.5f + 2.5f, d - 5.0f, d - 5.0f}, Shape::Circle, alpha));
    }
    // The ramp being drawn
    if (drag_ == Drag::Ramp && dragMoved_) {
        const float x0 = pressMouse_.x, y0 = velocityY(pressVelocity_);
        const float x1 = dragMouse_.x, y1 = velocityY(shownVelocity_);
        const float length = std::max(1.0f, std::hypot(x1 - x0, y1 - y0));
        for (float t = 0.0f; t <= length; t += 5.0f)
            list.add(theme.accent({x0 + (x1 - x0) * t / length - 1.5f, y0 + (y1 - y0) * t / length - 1.5f, 3.0f, 3.0f}, Shape::Circle, 0.8f));
    }
    list.popClip();
}

void PianoRollView::drawOverview(DrawList &list, const Theme &theme, const Clip &c) const
{
    // The grid in use, under the keys: accent while snapping (a click turns it off)
    const bool hot = gridLabel_.contains(mouse_) && drag_ == Drag::None;
    list.add(theme.surface(gridLabel_.inset(1.0f), Shape::Pill, snapping_ ? Elevation::Raised : Elevation::Inset, hot ? 0.7f : 0.45f,
                           snapping_ ? Finish::Convex : Finish::Flat));
    list.add(theme.text((snapping_ ? "Snap " : "Free ") + gridText(), gridLabel_, TextStyle::Caption,
                        snapping_ ? TextTone::Accent : TextTone::Muted, TextAlign::Center));

    // The whole clip in a groove, the part in view as a raised window over it
    list.add(theme.surface(mini_, Shape::Pill, Elevation::Inset, 0.45f));
    const Rect thumb = miniThumb();
    list.add(theme.surface(thumb, Shape::Pill, Elevation::Raised, drag_ == Drag::Mini ? 0.75f : 0.5f, Finish::Convex));
    const double total = c.length + kTailBeats + kLeadBeats;
    auto beatToX = [this, total](double beat) {
        return mini_.x + 6.0f + static_cast<float>((beat + kLeadBeats) / total) * (mini_.w - 12.0f);
    };
    list.pushClip(mini_.inset(2.0f));
    int low = 127, high = 0;
    for (const Note &n : c.notes) {
        low = std::min(low, n.key);
        high = std::max(high, n.key);
    }
    if (high < low + 12) {
        low = std::max(0, low - 6);
        high = std::min(127, low + 12);
    }
    const float span = mini_.h - 8.0f;
    for (const Note &n : c.notes) {
        const float x = beatToX(n.start);
        const float w = std::max(1.5f, beatToX(n.start + n.length) - x - 0.5f);
        const float y = mini_.y + 4.0f + static_cast<float>(high - n.key) / static_cast<float>(std::max(1, high - low)) * (span - 2.0f);
        const bool in = x + w > thumb.x && x < thumb.x + thumb.w;
        list.add(theme.accent({x, y, w, 2.0f}, Shape::Rounded, n.muted ? 0.2f : n.selected ? 1.0f : in ? 0.75f : 0.4f));
    }
    // Outside the window dimmed, so the part in view stands out
    if (thumb.x > mini_.x)
        list.add(theme.scrim({mini_.x, mini_.y, thumb.x - mini_.x, mini_.h}, 0.55f));
    if (thumb.x + thumb.w < mini_.x + mini_.w)
        list.add(theme.scrim({thumb.x + thumb.w, mini_.y, mini_.x + mini_.w - thumb.x - thumb.w, mini_.h}, 0.55f));
    // The loop and the playhead
    if (hasLoop())
        list.add(theme.accent({beatToX(project().loopStart - c.start), mini_.y + mini_.h - 4.0f,
                               std::max(2.0f, beatToX(project().loopEnd - c.start) - beatToX(project().loopStart - c.start)), 2.0f},
                              Shape::Pill, project().looping ? 0.8f : 0.35f));
    if (hooks_.position) {
        const double at = hooks_.position() - c.start;
        if (at >= -kLeadBeats && at <= c.length + kTailBeats)
            list.add(theme.accent({beatToX(at) - 0.5f, mini_.y + 3.0f, 1.5f, mini_.h - 6.0f}, Shape::Rounded, 0.9f));
    }
    list.popClip();
}

void PianoRollView::draw(DrawList &list, const Theme &theme) const
{
    const Clip *c = clip();

    // The corner: the key under the mouse (or the one sounding)
    list.add(theme.surface(corner_, Shape::Soft, Elevation::Inset, 0.45f));
    const int shown = sounding_ >= 0 ? sounding_ : hoverKey_;
    if (shown >= 0)
        list.add(theme.text(audio::keyName(shown), corner_, TextStyle::Subtitle, TextTone::Accent, TextAlign::Center));
    else if (c)
        list.add(theme.text(gridText(), corner_, TextStyle::Caption, TextTone::Muted, TextAlign::Center));

    drawKeyboard(list, theme, c);

    list.add(theme.surface(grid_, Shape::Soft, Elevation::Inset, 0.6f));
    if (!c) {
        const Rect middle{grid_.x, grid_.y + grid_.h * 0.5f - 30.0f, grid_.w, 30.0f};
        list.add(theme.text("No instrument clip selected", middle, TextStyle::Subtitle, TextTone::Secondary, TextAlign::Center));
        list.add(theme.text("Double-click a clip in the arrangement, or an empty instrument lane to make one",
                            {middle.x, middle.y + 30.0f, middle.w, 24.0f}, TextStyle::Caption, TextTone::Muted, TextAlign::Center));
        list.add(theme.surface(loopBand_, Shape::Pill, Elevation::Inset, 0.4f));
        list.add(theme.surface(mini_, Shape::Pill, Elevation::Inset, 0.45f));
        return;
    }

    list.pushClip(grid_.inset(2.0f));
    drawGrid(list, theme, *c);
    drawNotes(list, theme, *c);
    list.popClip();

    drawRuler(list, theme, *c);
    if (velocityLane_ && lane_.h > 0.0f)
        drawLane(list, theme, *c);
    drawOverview(list, theme, *c);

    // Playhead, from the ruler down through the lane
    if (hooks_.position) {
        const double at = hooks_.position() - c->start;
        const float x = beatX(at);
        if (x >= grid_.x && x <= grid_.x + grid_.w) {
            const float top = loopBand_.y + loopBand_.h + 2.0f;
            const float bottom = velocityLane_ ? lane_.y + lane_.h : grid_.y + grid_.h;
            list.add(theme.accent({x - 1.0f, top + 6.0f, 2.0f, bottom - top - 6.0f}, Shape::Rounded, 0.9f));
            list.add(theme.accent({x - 6.0f, top, 12.0f, 9.0f}, Shape::TriangleDown));
        }
    }

    // What the drag is doing, in a raised pill beside the mouse
    const std::string text = readout();
    if (!text.empty()) {
        const Vec2 size = theme.measureText(text, TextStyle::Caption);
        const float w = size.x + 20.0f, h = 22.0f;
        const float x = std::clamp(dragMouse_.x + 16.0f, rect_.x, rect_.x + rect_.w - w);
        const float y = std::clamp(dragMouse_.y - h - 12.0f, rect_.y, rect_.y + rect_.h - h);
        list.add(theme.surface({x, y, w, h}, Shape::Pill, Elevation::Raised, 0.8f, Finish::Convex));
        list.add(theme.text(text, {x, y, w, h}, TextStyle::Caption, TextTone::Primary, TextAlign::Center));
    }
}

}
