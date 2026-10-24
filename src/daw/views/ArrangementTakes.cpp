// The arrangement's take lanes: drawing them and comping in them (docs/design/takes-plan.md). The
// rules are in daw/model/Takes; this is ArrangementView's part, kept apart from Arrangement.cpp.
#include "daw/views/Arrangement.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "daw/model/Takes.h"
#include "daw/model/Warp.h"
#include "daw/widgets/Format.h"

namespace daw {

using namespace ui;

namespace {

constexpr double kDoubleClick = 0.3; // seconds
constexpr float kBoundaryReach = 5.0f; // pixels either side of a comp boundary that grab it

// A lane's name, short, for a comp section's badge: "T3" for "Take 3"
std::string shortName(const std::string &name)
{
    if (name.rfind("Take ", 0) == 0)
        return "T" + name.substr(5);
    return name.size() > 8 ? name.substr(0, 7) + "." : name;
}

}

void ArrangementView::drawTakeBadge(DrawList &list, const Theme &theme, int track, const Clip &clip, const Rect &r) const
{
    const Track *t = project().track(track);
    const int lane = t ? laneOfSection(*t, clip) : -1;
    if (lane < 0 || r.w < 40.0f)
        return;
    const std::string text = shortName(t->takeLanes[static_cast<size_t>(lane)].name);
    const float w = theme.measureText(text, TextStyle::Caption).x + 12.0f;
    const Rect badge{r.x + r.w - w - 6.0f, r.y + 5.0f, w, 16.0f};
    if (badge.x < r.x + 40.0f)
        return;
    list.add(theme.surface(badge, Shape::Pill, Elevation::Inset, 0.4f));
    list.add(theme.text(text, badge, TextStyle::Caption, TextTone::Accent, TextAlign::Center));
}

void ArrangementView::drawTakeContent(DrawList &list, const Theme &theme, int track, const Clip &take, const Rect &r,
                                      float opacity) const
{
    const Track &t = project().tracks[static_cast<size_t>(track)];
    const Rect body = r.inset(4.0f);
    const float visibleStart = std::max(body.x, well_.x), visibleEnd = std::min(body.x + body.w, well_.x + well_.w);
    if (t.kind == TrackKind::Audio) {
        const audio::AudioData *data = transport_.takeAudioFor ? transport_.takeAudioFor(track, take) : nullptr;
        if (!data)
            return;
        const audio::TempoMap &timing = project().timing;
        std::vector<float> pairs;
        uint32_t columns = 0;
        const float firstX = visibleStart;
        for (float x = firstX; x < visibleEnd; x += 2.0f) {
            const double b0 = take.start + (x - r.x) / pixelsPerBeat_, b1 = take.start + (x + 2.0f - r.x) / pixelsPerBeat_;
            if (b0 >= take.end())
                break;
            float low = 0.0f, high = 0.0f;
            data->range(clipSourceSeconds(take, timing, b0), clipSourceSeconds(take, timing, b1), low, high);
            pairs.push_back(std::max(-1.0f, low * take.gain));
            pairs.push_back(std::min(1.0f, high * take.gain));
            ++columns;
        }
        if (columns > 0) {
            Plot wave = theme.inkPlot({firstX, body.y, 2.0f * static_cast<float>(columns), body.h}, PlotMode::Range, TextTone::Secondary, opacity);
            wave.low = -1.0f;
            wave.high = 1.0f;
            wave.barFraction = 0.75f;
            wave.values = pairs.data();
            wave.count = columns;
            list.add(wave);
        }
        return;
    }
    if (take.notes.empty())
        return;
    // The notes' pitch range fills the lane (at least an octave)
    int low = 127, high = 0;
    for (const Note &n : take.notes) {
        low = std::min(low, n.key);
        high = std::max(high, n.key);
    }
    if (high - low < 12) {
        const int pad = (12 - (high - low)) / 2;
        low = std::max(0, low - pad);
        high = std::min(127, low + 12);
    }
    const float rowHeight = body.h / static_cast<float>(high - low + 1);
    for (const Note &n : take.notes) {
        if (n.start >= take.length)
            continue;
        const float x = r.x + static_cast<float>(n.start) * pixelsPerBeat_;
        const float w = std::max(2.0f, static_cast<float>(std::min(n.length, take.length - n.start)) * pixelsPerBeat_ - 1.0f);
        if (x + w < visibleStart || x > visibleEnd)
            continue;
        list.add(theme.ink({x, body.y + static_cast<float>(high - n.key) * rowHeight, w, std::max(2.0f, rowHeight - 1.0f)}, Shape::Pill,
                           TextTone::Secondary, true, n.muted ? opacity * 0.4f : opacity));
    }
}

void ArrangementView::drawTakeLanes(DrawList &list, const Theme &theme) const
{
    const size_t count = std::min(rows_.size(), project().tracks.size());
    for (size_t i = 0; i < count; ++i) {
        const int track = static_cast<int>(i);
        if (rows_[i].takes == 0 || hidden(track))
            continue;
        const Track &t = project().tracks[i];
        const ObjectId heard = transport_.auditioned ? transport_.auditioned(track) : kNoId;
        for (size_t l = 0; l < rows_[i].takes && l < t.takeLanes.size(); ++l) {
            const Rect lane = takeLaneRect(track, l);
            if (lane.y > well_.y + well_.h || lane.y + lane.h < well_.y)
                continue;
            const TakeLane &takes = t.takeLanes[l];
            const bool hot = track == takeTrack_ && static_cast<int>(l) == takeLane_;
            const bool listening = heard == takes.id;
            list.add(theme.surface({lane.x + 4.0f, lane.y + 2.0f, lane.w - 8.0f, lane.h - 4.0f}, Shape::Soft, Elevation::Inset,
                                   hot ? 0.42f : 0.28f));
            if (listening)
                list.add(theme.accent({lane.x + 4.0f, lane.y + 2.0f, lane.w - 8.0f, lane.h - 4.0f}, Shape::Soft, 0.06f));
            list.pushClip(lane);
            // Every take faint
            for (const Clip &take : takes.clips) {
                const Rect r{beatX(take.start) + 1.0f, lane.y + 5.0f, static_cast<float>(take.length) * pixelsPerBeat_ - 2.0f, lane.h - 10.0f};
                if (!clipShown(r))
                    continue;
                list.add(theme.surface(r, Shape::Soft, Elevation::Raised, 0.25f));
                drawTakeContent(list, theme, track, take, r, listening ? 0.6f : 0.25f);
            }
            // The parts the comp plays from it, bright and outlined
            for (const auto &[from, to] : laneUse(t, takes.id)) {
                const Rect used{beatX(from), lane.y + 3.0f, static_cast<float>(to - from) * pixelsPerBeat_, lane.h - 6.0f};
                if (!clipShown(used))
                    continue;
                list.pushClip(used);
                for (const Clip &take : takes.clips) {
                    const Rect r{beatX(take.start) + 1.0f, lane.y + 5.0f, static_cast<float>(take.length) * pixelsPerBeat_ - 2.0f, lane.h - 10.0f};
                    if (!clipShown(r))
                        continue;
                    list.add(theme.surface(r, Shape::Soft, Elevation::Raised, 0.65f, Finish::Convex));
                    drawTakeContent(list, theme, track, take, r, 0.75f);
                }
                list.popClip();
                list.add(theme.accent({used.x, used.y, used.w, 2.0f}, Shape::Rounded, 0.85f));
                list.add(theme.accent({used.x, used.y + used.h - 2.0f, used.w, 2.0f}, Shape::Rounded, 0.85f));
            }
            // What a click would choose (or the swipe in progress)
            if (hot && hoverTo_ > hoverFrom_ && takeDrag_ != TakeDrag::Boundary) {
                const Rect what{beatX(hoverFrom_), lane.y + 3.0f, static_cast<float>(hoverTo_ - hoverFrom_) * pixelsPerBeat_, lane.h - 6.0f};
                list.add(theme.accent(what, Shape::Soft, takeDrag_ == TakeDrag::Swipe ? 0.16f : 0.09f));
            }
            list.popClip();
        }
        // The comp's boundaries, down through its lanes
        const Rect first = takeLaneRect(track, 0);
        const float height = static_cast<float>(rows_[i].takes) * first.h;
        for (double edge : compBoundaries(t)) {
            const float x = beatX(edge);
            if (x < well_.x || x > well_.x + well_.w)
                continue;
            const bool dragged = takeDrag_ == TakeDrag::Boundary && track == takeTrack_ && std::fabs(edge - boundaryAt_) < 1e-6;
            list.add(theme.accent({x - 1.0f, first.y + 2.0f, 2.0f, height - 4.0f}, Shape::Pill, dragged ? 1.0f : 0.45f));
        }
    }
}

void ArrangementView::drawTakeHeaders(DrawList &list, const Theme &theme) const
{
    const size_t count = std::min(rows_.size(), project().tracks.size());
    for (size_t i = 0; i < count; ++i) {
        const int track = static_cast<int>(i);
        if (rows_[i].takes == 0 || hidden(track))
            continue;
        const Track &t = project().tracks[i];
        const ObjectId heard = transport_.auditioned ? transport_.auditioned(track) : kNoId;
        for (size_t l = 0; l < rows_[i].takes && l < t.takeLanes.size(); ++l) {
            const Rect h = takeHeaderRect(track, l);
            if (h.y > headerArea_.y + headerArea_.h || h.y + h.h < headerArea_.y)
                continue;
            const TakeLane &takes = t.takeLanes[l];
            const bool listening = heard == takes.id;
            // How much of the lane the comp plays
            double total = 0.0, used = 0.0;
            for (const Clip &take : takes.clips)
                total += take.length;
            for (const auto &[from, to] : laneUse(t, takes.id))
                used += to - from;
            const int share = total > 0.0 ? static_cast<int>(std::lround(100.0 * used / total)) : 0;
            list.add(theme.ink({h.x + 20.0f, h.y + 4.0f, 2.0f, h.h - 8.0f}, Shape::Pill, used > 0.0 ? TextTone::Accent : TextTone::Muted,
                               true, used > 0.0 ? 0.8f : 0.3f));
            const float textW = h.w - 120.0f;
            list.add(theme.text(takes.name, {h.x + 32.0f, h.y + 7.0f, textW, 18.0f}, TextStyle::Caption,
                                listening ? TextTone::Accent : TextTone::Secondary, TextAlign::Start));
            // How much the comp uses it; over a button, what the button does
            const bool overLane = track == takeTrack_ && static_cast<int>(l) == takeLane_;
            const int button = overLane ? takeButton_ : -1;
            const char *says[] = {listening ? "Back to the comp" : "Listen to this take", "Use all of this take", "Delete this take lane"};
            const std::string caption = button >= 0 ? std::string(says[button])
                                        : listening ? std::string("Listening")
                                        : used <= 0.0 ? std::string("Not in the comp")
                                                      : std::to_string(share) + "% in the comp";
            list.add(theme.text(caption, {h.x + 32.0f, h.y + 26.0f, textW + 40.0f, 18.0f}, TextStyle::Caption,
                                listening || button >= 0 ? TextTone::Accent : TextTone::Muted, TextAlign::Start));
            // Listen, use it all, delete
            const Icon icons[] = {Icon::Play, Icon::Up, Icon::Close}; // up: into the comp
            for (int b = 0; b < 3; ++b) {
                const Rect r = takeButtonRect(track, l, b);
                const bool hover = track == takeTrack_ && static_cast<int>(l) == takeLane_ && takeButton_ == b;
                const bool on = b == 0 && listening;
                list.add(theme.surface(r, Shape::Circle, hover || on ? Elevation::Raised : Elevation::Inset, hover || on ? 0.7f : 0.35f));
                drawIcon(list, theme, icons[b], r.inset(4.0f), on || hover ? TextTone::Accent : TextTone::Secondary);
            }
        }
    }
}

bool ArrangementView::handleTakeInput(const Input &input)
{
    takePress_ = false;
    const audio::TempoMap &timing = project().timing;
    // Alt+Up and Alt+Down: the selected comp section from the take above or below
    if (takeKeys_.attach(input.signals)) {
        auto step = [this](int direction) {
            return [this, direction](const ActionEvent &) {
                const int index = project().selectedTrack;
                const Clip *c = project().clip();
                if (!c || c->take == kNoId)
                    return false;
                Track copy = project().tracks[static_cast<size_t>(index)];
                const double at = c->start + c->length * 0.5;
                if (stepTake(copy, static_cast<size_t>(project().selectedClip), direction, project().timing)) {
                    store_.editTrack(index).clips = copy.clips;
                    for (size_t k = 0; k < copy.clips.size(); ++k) {
                        if (copy.clips[k].start <= at && copy.clips[k].end() > at)
                            store_.select(index, static_cast<int>(k));
                    }
                }
                return true;
            };
        };
        takeKeys_.action("takes.up", Priority::Component, step(-1));
        takeKeys_.action("takes.down", Priority::Component, step(1));
    }
    // The comp as `edited`, when it differs from the track's
    auto apply = [this](int track, const Track &edited) {
        Track &t = store_.editTrack(track);
        t.clips = edited.clips;
        t.takeLanes = edited.takeLanes;
    };

    // A press being dragged: a swipe across a take, or a click on release
    if (takeDrag_ == TakeDrag::Pending || takeDrag_ == TakeDrag::Swipe) {
        const Track *t = project().track(takeTrack_);
        if (!t) {
            takeDrag_ = TakeDrag::None;
            return true;
        }
        if (!input.down) {
            if (takeDrag_ == TakeDrag::Pending) {
                // A click: the section under it from this take
                Track copy = *t;
                const auto [from, to] = sectionAround(copy, pressRaw_);
                if (compRange(copy, from, to, dragTake_, timing))
                    apply(takeTrack_, copy);
            }
            takeDrag_ = TakeDrag::None;
            return true;
        }
        const double beat = laneSnap(xBeat(input.mouse.x), input.shift);
        if (takeDrag_ == TakeDrag::Pending && std::fabs(input.mouse.x - pressX_) > 4.0f)
            takeDrag_ = TakeDrag::Swipe;
        if (takeDrag_ == TakeDrag::Swipe) {
            hoverFrom_ = std::min(pressBeat_, beat);
            hoverTo_ = std::max(pressBeat_, beat);
            if (beat != swipeTo_) {
                // From the comp as it was at the press, so swiping back gives back what was there
                swipeTo_ = beat;
                Track copy = *t;
                copy.clips = compBefore_;
                compRange(copy, hoverFrom_, hoverTo_, dragTake_, timing);
                apply(takeTrack_, copy);
            }
        }
        input.setCursor(engine::Cursor::Hand);
        return true;
    }
    // A boundary being dragged
    if (takeDrag_ == TakeDrag::Boundary) {
        const Track *t = project().track(takeTrack_);
        if (!input.down || !t) {
            takeDrag_ = TakeDrag::None;
            return true;
        }
        const double to = laneSnap(xBeat(input.mouse.x), input.shift);
        if (std::fabs(to - boundaryAt_) > 1e-9) {
            Track copy = *t;
            if (moveBoundary(copy, boundaryAt_, to, timing)) {
                apply(takeTrack_, copy);
                // Where the boundary landed (moving it is clamped): the nearest one to where it went
                double best = boundaryAt_;
                for (double edge : compBoundaries(copy)) {
                    if (std::fabs(edge - to) < std::fabs(best - to))
                        best = edge;
                }
                boundaryAt_ = best;
            }
        }
        input.setCursor(engine::Cursor::ResizeX);
        return true;
    }

    takeTrack_ = takeLane_ = takeButton_ = -1;
    hoverFrom_ = hoverTo_ = 0.0;
    int track = -1;
    size_t lane = 0;
    if (!takeLaneAt(input.mouse, track, lane))
        return false;
    const Track &t = project().tracks[static_cast<size_t>(track)];
    if (lane >= t.takeLanes.size())
        return true;
    takeTrack_ = track;
    takeLane_ = static_cast<int>(lane);
    const TakeLane &takes = t.takeLanes[lane];
    const ObjectId heard = transport_.auditioned ? transport_.auditioned(track) : kNoId;

    // A lane's header: its buttons, and its menu
    if (headerArea_.contains(input.mouse)) {
        for (int b = 0; b < 3; ++b) {
            if (takeButtonRect(track, lane, b).contains(input.mouse))
                takeButton_ = b;
        }
        if (input.secondaryPressed && transport_.takeMenu) {
            input.secondaryPressed = false;
            transport_.takeMenu(track, takes.id, kNoId, input.mouse);
        }
        if (!input.pressed)
            return true;
        takePress_ = true;
        if (takeButton_ == 0 && transport_.audition) {
            transport_.audition(track, heard == takes.id ? kNoId : takes.id);
        } else if (takeButton_ == 1) {
            Track copy = t;
            if (useLane(copy, takes.id, timing))
                apply(track, copy);
        } else if (takeButton_ == 2) {
            const ObjectId gone = takes.id;
            Track copy = t;
            if (removeLane(copy, gone)) {
                if (heard == gone && transport_.audition)
                    transport_.audition(track, kNoId);
                apply(track, copy);
                takeTrack_ = takeLane_ = takeButton_ = -1;
            }
        }
        return true;
    }

    // In the lane: a comp boundary near the mouse, else the take under it
    const double raw = std::clamp(xBeat(input.mouse.x), 0.0, project().lengthBeats());
    for (double edge : compBoundaries(t)) {
        if (std::fabs(beatX(edge) - input.mouse.x) <= kBoundaryReach) {
            input.setCursor(engine::Cursor::ResizeX);
            if (input.pressed) {
                takePress_ = true;
                takeDrag_ = TakeDrag::Boundary;
                boundaryAt_ = edge;
            }
            return true;
        }
    }
    const Clip *take = nullptr;
    for (const Clip &c : takes.clips) {
        if (c.start <= raw && c.end() > raw)
            take = &c;
    }
    if (take) {
        const auto [from, to] = sectionAround(t, raw);
        hoverFrom_ = std::max(from, take->start);
        hoverTo_ = std::min(to, take->end());
        input.setCursor(engine::Cursor::Hand);
    }
    if (input.secondaryPressed) {
        input.secondaryPressed = false;
        if (transport_.takeMenu)
            transport_.takeMenu(track, takes.id, take ? take->id : kNoId, input.mouse);
        return true;
    }
    if (!input.pressed)
        return true;
    takePress_ = true;
    if (!take)
        return true;
    // A second press on the same take soon after: all of it
    const bool doubleClick = lastTakePress_ >= 0.0 && input.time - lastTakePress_ < kDoubleClick && lastTakePressed_ == take->id;
    lastTakePress_ = doubleClick ? -1.0 : input.time;
    lastTakePressed_ = take->id;
    if (doubleClick) {
        Track copy = t;
        if (compRange(copy, take->start, take->end(), take->id, timing))
            apply(track, copy);
        return true;
    }
    takeDrag_ = TakeDrag::Pending;
    dragTake_ = take->id;
    pressRaw_ = raw;
    pressBeat_ = laneSnap(raw, input.shift);
    pressX_ = input.mouse.x;
    swipeTo_ = -1.0;
    compBefore_ = t.clips;
    return true;
}

}
