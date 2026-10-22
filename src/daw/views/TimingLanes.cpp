#include "daw/views/TimingLanes.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "daw/widgets/Format.h"

namespace daw {

using namespace ui;

namespace {

constexpr float kPad = 8.0f;          // between the tempo lane's edge and its curve's top and bottom
constexpr float kPointReach = 8.0f;   // how near the mouse must be to a point to take it
constexpr double kDoubleClick = 0.3;  // seconds

std::string signatureText(const audio::Signature &s)
{
    return std::to_string(s.numerator) + "/" + std::to_string(s.denominator);
}

// "3.2" for a beat: its bar and the unit in it, from 1 (the ticks only when it's off the unit)
std::string barText(const audio::TempoMap &timing, double beat)
{
    const audio::BarPosition p = timing.position(beat);
    std::string text = std::to_string(p.bar + 1) + "." + std::to_string(p.unit + 1);
    if (p.fraction > 1e-6)
        text += "." + format("%03.0f", std::floor(p.fraction * 960.0));
    return text;
}

}

TimingLanes::TimingLanes(ProjectStore &store, TimingHooks hooks) : store_(store), hooks_(std::move(hooks)) {}

void TimingLanes::arrange(const Rect &headers, const Rect &lanes)
{
    signatureHeader_ = {headers.x, headers.y, headers.w, kSignatureHeight};
    tempoHeader_ = {headers.x, headers.y + kSignatureHeight + kGap, headers.w, kTempoHeight};
    signatureLane_ = {lanes.x, lanes.y, lanes.w, kSignatureHeight};
    tempoLane_ = {lanes.x, lanes.y + kSignatureHeight + kGap, lanes.w, kTempoHeight};
}

void TimingLanes::tempoRange(double &low, double &high) const
{
    if (dragPoint_ >= 0) {
        low = dragLow_;
        high = dragHigh_;
        return;
    }
    low = audio::TempoMap::kMaxBpm;
    high = audio::TempoMap::kMinBpm;
    for (const audio::TempoPoint &p : project().timing.points()) {
        low = std::min(low, p.bpm);
        high = std::max(high, p.bpm);
    }
    low = std::floor((low - 5.0) / 10.0) * 10.0;
    high = std::ceil((high + 5.0) / 10.0) * 10.0;
    if (high - low < 40.0) {
        const double middle = std::round((low + high) / 20.0) * 10.0;
        low = middle - 20.0;
        high = middle + 20.0;
    }
    low = std::max(0.0, low);
}

float TimingLanes::bpmY(double bpm) const
{
    double low = 0.0, high = 0.0;
    tempoRange(low, high);
    const double f = (bpm - low) / (high - low);
    return tempoLane_.y + tempoLane_.h - kPad - static_cast<float>(f) * (tempoLane_.h - 2.0f * kPad);
}

double TimingLanes::yBpm(float y, bool fine) const
{
    double low = 0.0, high = 0.0;
    tempoRange(low, high);
    const double f = (tempoLane_.y + tempoLane_.h - kPad - y) / (tempoLane_.h - 2.0f * kPad);
    const double bpm = low + f * (high - low);
    const double step = fine ? 0.1 : 1.0;
    return std::clamp(std::round(bpm / step) * step, audio::TempoMap::kMinBpm, audio::TempoMap::kMaxBpm);
}

int TimingLanes::pointAt(Vec2 p) const
{
    if (!tempoLane_.contains(p))
        return -1;
    const auto &points = project().timing.points();
    int found = -1;
    float best = kPointReach;
    for (size_t i = 0; i < points.size(); ++i) {
        const float dx = std::fabs(beatX(points[i].beat) - p.x);
        if (dx < best && std::fabs(bpmY(points[i].bpm) - p.y) < 10.0f) {
            best = dx;
            found = static_cast<int>(i);
        }
    }
    return found;
}

Rect TimingLanes::markerRect(const audio::Signature &s) const
{
    const float x = beatX(project().timing.barStart(s.bar));
    return {x, signatureLane_.y + 3.0f, 36.0f, signatureLane_.h - 6.0f};
}

int TimingLanes::markerAt(Vec2 p) const
{
    if (!signatureLane_.contains(p))
        return -1;
    const auto &signatures = project().timing.signatures();
    // The last drawn is on top
    for (int i = static_cast<int>(signatures.size()) - 1; i >= 0; --i) {
        if (markerRect(signatures[static_cast<size_t>(i)]).inset(-2.0f).contains(p))
            return i;
    }
    return -1;
}

int TimingLanes::selectedPoint() const
{
    if (selectedBeat_ < 0.0)
        return -1;
    const audio::TempoMap &timing = project().timing;
    const size_t i = timing.pointAt(selectedBeat_);
    return timing.points()[i].beat == selectedBeat_ ? static_cast<int>(i) : -1;
}

int TimingLanes::selectedMarker() const
{
    if (selectedBar_ < 0)
        return -1;
    const audio::TempoMap &timing = project().timing;
    const size_t i = timing.signatureIndexAt(selectedBar_);
    return timing.signatures()[i].bar == selectedBar_ ? static_cast<int>(i) : -1;
}

bool TimingLanes::deleteSelected()
{
    const audio::TempoMap &timing = project().timing;
    const int point = selectedPoint();
    if (point > 0) {
        std::vector<audio::TempoPoint> points = timing.points();
        points.erase(points.begin() + point);
        audio::TempoMap edited = timing;
        edited.setPoints(std::move(points));
        edit(edited);
        selectedBeat_ = -1.0;
        return true;
    }
    const int marker = selectedMarker();
    if (marker > 0) {
        std::vector<audio::Signature> signatures = timing.signatures();
        signatures.erase(signatures.begin() + marker);
        audio::TempoMap edited = timing;
        edited.setSignatures(std::move(signatures));
        edit(edited);
        selectedBar_ = -1;
        return true;
    }
    return false;
}

bool TimingLanes::handleInput(const Input &input)
{
    // Delete removes the selected point or change; with nothing removable selected it goes on to
    // whatever else takes it (the selected clip)
    if (keys_.attach(input.signals))
        keys_.action("edit.delete", Priority::Component, [this](const ActionEvent &) { return deleteSelected(); });

    const audio::TempoMap &timing = project().timing;
    const double end = project().lengthBeats();

    // A tempo point being dragged follows the mouse between its neighbours (the first stays at 0)
    if (dragPoint_ >= 0) {
        const auto &points = timing.points();
        if (!input.down || static_cast<size_t>(dragPoint_) >= points.size()) {
            dragPoint_ = -1;
            return true;
        }
        const size_t i = static_cast<size_t>(dragPoint_);
        std::vector<audio::TempoPoint> moved = points;
        if (i > 0) {
            const double low = points[i - 1].beat + 1.0 / 64.0;
            const double high = i + 1 < points.size() ? points[i + 1].beat - 1.0 / 64.0 : end;
            moved[i].beat = std::clamp(hooks_.snap(xBeat(input.mouse.x), input.shift), low, std::max(low, high));
        }
        moved[i].bpm = yBpm(input.mouse.y, input.shift);
        if (moved[i] != points[i]) {
            audio::TempoMap edited = timing;
            edited.setPoints(std::move(moved));
            edit(edited);
            selectedBeat_ = project().timing.points()[i].beat;
        }
        input.setCursor(engine::Cursor::Move);
        return true;
    }
    // A signature change being dragged moves to the bar under the mouse, between its neighbours
    if (dragMarker_ >= 0) {
        const auto &signatures = timing.signatures();
        if (!input.down || static_cast<size_t>(dragMarker_) >= signatures.size()) {
            dragMarker_ = -1;
            return true;
        }
        const size_t i = static_cast<size_t>(dragMarker_);
        const int low = signatures[i - 1].bar + 1;
        const int high = i + 1 < signatures.size() ? signatures[i + 1].bar - 1 : std::max(low, timing.barAt(end));
        const int bar = std::clamp(timing.barAt(timing.nearestBar(xBeat(input.mouse.x))), low, std::max(low, high));
        if (bar != signatures[i].bar) {
            std::vector<audio::Signature> moved = signatures;
            moved[i].bar = bar;
            audio::TempoMap edited = timing;
            edited.setSignatures(std::move(moved));
            edit(edited);
            selectedBar_ = bar;
        }
        input.setCursor(engine::Cursor::ResizeX);
        return true;
    }

    hoverPoint_ = pointAt(input.mouse);
    hoverMarker_ = markerAt(input.mouse);
    hoverTempo_ = tempoLane_.contains(input.mouse);
    hoverSignature_ = signatureLane_.contains(input.mouse);
    const bool inHeaders = signatureHeader_.contains(input.mouse) || tempoHeader_.contains(input.mouse);
    // Where a click would add something: a point at the snapped beat, a change at the bar
    ghostBeat_ = -1.0;
    ghostBar_ = -1;
    const double mouseBeat = std::clamp(xBeat(input.mouse.x), 0.0, end);
    if (hoverTempo_ && hoverPoint_ < 0) {
        const double beat = std::clamp(hooks_.snap(mouseBeat, input.shift), 0.0, end);
        const auto &points = timing.points();
        if (beat > 0.0 && std::none_of(points.begin(), points.end(), [&](const audio::TempoPoint &p) { return std::fabs(p.beat - beat) < 1e-9; })) {
            ghostBeat_ = beat;
            ghostBpm_ = yBpm(input.mouse.y, input.shift);
        }
    }
    if (hoverSignature_ && hoverMarker_ < 0) {
        const int bar = timing.barAt(mouseBeat);
        if (bar > 0 && timing.signatureAtBar(bar).bar != bar)
            ghostBar_ = bar;
    }

    if (!hoverTempo_ && !hoverSignature_ && !inHeaders) {
        // A press anywhere else lets go of the selection
        if (input.pressed || input.secondaryPressed) {
            selectedBeat_ = -1.0;
            selectedBar_ = -1;
        }
        return false;
    }
    if (hoverPoint_ >= 0)
        input.setCursor(engine::Cursor::Move);
    else if (hoverMarker_ > 0)
        input.setCursor(engine::Cursor::ResizeX);
    else if (ghostBeat_ >= 0.0)
        input.setCursor(engine::Cursor::Crosshair);
    else if (hoverMarker_ == 0 || ghostBar_ >= 0)
        input.setCursor(engine::Cursor::Hand);

    // Right-click: a point's or a marker's menu (selecting it), or the lane's
    if (input.secondaryPressed) {
        input.secondaryPressed = false;
        if (hoverPoint_ >= 0) {
            selectedBeat_ = timing.points()[static_cast<size_t>(hoverPoint_)].beat;
            selectedBar_ = -1;
            if (hooks_.tempoMenu)
                hooks_.tempoMenu(hoverPoint_, input.mouse);
        } else if (hoverMarker_ >= 0) {
            selectedBar_ = timing.signatures()[static_cast<size_t>(hoverMarker_)].bar;
            selectedBeat_ = -1.0;
            if (hooks_.signatureMenu)
                hooks_.signatureMenu(hoverMarker_, input.mouse);
        } else if ((hoverTempo_ || hoverSignature_) && hooks_.laneMenu) {
            hooks_.laneMenu(input.mouse, hoverTempo_ ? hooks_.snap(mouseBeat, input.shift) : timing.barStart(timing.barAt(mouseBeat)), hoverTempo_);
        }
        return true;
    }
    if (!input.pressed)
        return true;

    // A second press on the same thing soon after the first is a double-click
    const int pressed = hoverTempo_ ? hoverPoint_ : hoverMarker_;
    const bool doubleClick = lastPress_ >= 0.0 && input.time - lastPress_ < kDoubleClick && pressed == lastPressed_ &&
                             lastPressTempo_ == hoverTempo_;
    lastPress_ = doubleClick ? -1.0 : input.time;
    lastPressed_ = pressed;
    lastPressTempo_ = hoverTempo_;

    if (hoverTempo_) {
        selectedBar_ = -1;
        if (doubleClick && pressed >= 0) {
            if (hooks_.editTempo)
                hooks_.editTempo(pressed);
            return true;
        }
        int point = pressed;
        if (point < 0 && ghostBeat_ > 0.0) {
            // A new point where the ghost is, holding until the next
            std::vector<audio::TempoPoint> points = timing.points();
            points.push_back({ghostBeat_, ghostBpm_, false});
            audio::TempoMap edited = timing;
            edited.setPoints(std::move(points));
            edit(edited);
            point = static_cast<int>(edited.pointAt(ghostBeat_));
            lastPressed_ = point; // a second click on it opens its tempo
        }
        if (point >= 0) {
            selectedBeat_ = project().timing.points()[static_cast<size_t>(point)].beat;
            tempoRange(dragLow_, dragHigh_); // before dragPoint_ is set, so it's the range now
            dragPoint_ = point;
        } else {
            selectedBeat_ = -1.0;
        }
        return true;
    }
    if (hoverSignature_) {
        selectedBeat_ = -1.0;
        if (doubleClick) {
            if (pressed >= 0) {
                if (hooks_.editSignature)
                    hooks_.editSignature(pressed);
            } else if (ghostBar_ > 0) {
                // A change at the bar, as the signature in force there, then its editor
                std::vector<audio::Signature> signatures = timing.signatures();
                audio::Signature added = timing.signatureAtBar(ghostBar_);
                added.bar = ghostBar_;
                signatures.push_back(added);
                audio::TempoMap edited = timing;
                edited.setSignatures(std::move(signatures));
                edit(edited);
                selectedBar_ = ghostBar_;
                if (hooks_.editSignature)
                    hooks_.editSignature(static_cast<int>(edited.signatureIndexAt(ghostBar_)));
            }
            return true;
        }
        selectedBar_ = pressed >= 0 ? timing.signatures()[static_cast<size_t>(pressed)].bar : -1;
        if (pressed > 0)
            dragMarker_ = pressed;
        return true;
    }
    return true; // the headers do nothing on a press
}

void TimingLanes::drawLanes(DrawList &list, const Theme &theme) const
{
    const audio::TempoMap &timing = project().timing;
    const double end = project().lengthBeats();
    const float endX = std::min(signatureLane_.x + signatureLane_.w, beatX(end));
    const int markerSelected = selectedMarker();
    const int pointSelected = selectedPoint();

    // Signatures: a groove with a raised marker at each change
    list.add(theme.surface(signatureLane_.inset(1.0f), Shape::Soft, Elevation::Inset, hoverSignature_ ? 0.42f : 0.3f));
    list.pushClip(signatureLane_);
    // Where a double-click would add a change: a faint marker and what it would do
    if (ghostBar_ > 0) {
        const float x = beatX(timing.barStart(ghostBar_));
        const Rect r{x, signatureLane_.y + 3.0f, 36.0f, signatureLane_.h - 6.0f};
        list.add(theme.accent({x, r.y + 2.0f, 2.0f, r.h - 4.0f}, Shape::Pill, 0.35f));
        list.add(theme.text("Double-click for a new time signature at bar " + std::to_string(ghostBar_ + 1),
                            {x + 8.0f, signatureLane_.y, 360.0f, signatureLane_.h}, TextStyle::Caption, TextTone::Muted, TextAlign::Start));
    }
    const auto &signatures = timing.signatures();
    for (size_t i = 0; i < signatures.size(); ++i) {
        const Rect r = markerRect(signatures[i]);
        if (r.x > signatureLane_.x + signatureLane_.w || r.x + r.w < signatureLane_.x)
            continue;
        const bool hot = static_cast<int>(i) == hoverMarker_ || static_cast<int>(i) == dragMarker_;
        const bool selected = static_cast<int>(i) == markerSelected;
        if (selected)
            list.add(theme.accent(r.inset(-2.0f), Shape::Soft, 0.3f));
        list.add(theme.surface(r, Shape::Soft, Elevation::Raised, hot || selected ? 0.75f : 0.45f, Finish::Convex));
        list.add(theme.accent({r.x, r.y + 2.0f, 2.0f, r.h - 4.0f}, Shape::Pill, hot || selected ? 1.0f : 0.7f));
        list.add(theme.text(signatureText(signatures[i]), {r.x + 4.0f, r.y, r.w - 4.0f, r.h}, TextStyle::Caption,
                            hot || selected ? TextTone::Accent : TextTone::Secondary, TextAlign::Center));
    }
    if (signatures.size() == 1 && ghostBar_ < 0 && endX > signatureLane_.x + 360.0f)
        list.add(theme.text("Double-click a bar to change the time signature there",
                            {markerRect(signatures[0]).x + 48.0f, signatureLane_.y, 360.0f, signatureLane_.h}, TextStyle::Caption,
                            TextTone::Muted, TextAlign::Start));
    list.popClip();

    // Tempo: a groove, a guide at each step of the scale, the area under the curve, the curve and
    // its points
    list.add(theme.surface(tempoLane_.inset(1.0f), Shape::Soft, Elevation::Inset, hoverTempo_ || dragPoint_ >= 0 ? 0.45f : 0.3f));
    list.pushClip(tempoLane_);
    double low = 0.0, high = 0.0;
    tempoRange(low, high);
    const double guide = high - low > 120.0 ? 40.0 : high - low > 60.0 ? 20.0 : 10.0;
    for (double bpm = std::ceil(low / guide) * guide; bpm <= high; bpm += guide)
        list.add(theme.ink({tempoLane_.x, bpmY(bpm), tempoLane_.w, 1.0f}, Shape::Rounded, TextTone::Muted, true, 0.1f));
    // The fill, sampled every 3 pixels over the song in view
    const float fillFrom = std::max(tempoLane_.x, beatX(0.0)), fillTo = std::min(tempoLane_.x + tempoLane_.w, endX);
    if (fillTo - fillFrom > 3.0f) {
        std::vector<float> values;
        values.reserve(static_cast<size_t>((fillTo - fillFrom) / 3.0f) + 2);
        for (float x = fillFrom; x < fillTo; x += 3.0f)
            values.push_back(static_cast<float>(timing.bpmAt(xBeat(x))));
        values.push_back(static_cast<float>(timing.bpmAt(xBeat(fillTo))));
        Plot fill = theme.accentPlot({fillFrom, tempoLane_.y + kPad, fillTo - fillFrom, tempoLane_.h - 2.0f * kPad}, PlotMode::Fill, 0.06f);
        fill.values = values.data();
        fill.count = static_cast<uint32_t>(values.size());
        fill.low = static_cast<float>(low);
        fill.high = static_cast<float>(high);
        fill.baseline = static_cast<float>(low);
        list.add(fill);
    }
    const auto &points = timing.points();
    std::vector<Vec2> line;
    line.reserve(points.size() * 2 + 1);
    for (size_t i = 0; i < points.size(); ++i) {
        const Vec2 p{beatX(points[i].beat), bpmY(points[i].bpm)};
        if (i > 0 && !points[i - 1].ramp)
            line.push_back({p.x, line.back().y}); // held until here, then a step
        line.push_back(p);
    }
    // After the last point it holds to the timeline's end
    line.push_back({std::max(line.back().x, endX), line.back().y});
    for (Vec2 &p : line)
        p.x = std::clamp(p.x, tempoLane_.x - 2.0f, tempoLane_.x + tempoLane_.w + 2.0f);
    list.addPolyline(line.data(), line.size(), theme.accentLine({}, {}, 2.0f, 0.85f));

    // A caption beside a point, kept inside the lane
    auto caption = [&](float x, float y, const std::string &text, TextTone tone) {
        const float w = theme.measureText(text, TextStyle::Caption).x + 4.0f;
        float tx = x + 10.0f;
        if (tx + w > tempoLane_.x + tempoLane_.w - 4.0f)
            tx = x - 10.0f - w;
        // Above the point, or below it when that would leave the lane
        float ty = y - 24.0f;
        if (ty < tempoLane_.y + 3.0f)
            ty = std::min(y + 8.0f, tempoLane_.y + tempoLane_.h - 19.0f);
        list.add(theme.text(text, {tx, ty, w, 16.0f}, TextStyle::Caption, tone, TextAlign::Start));
    };
    for (size_t i = 0; i < points.size(); ++i) {
        const float x = beatX(points[i].beat);
        if (x < tempoLane_.x - 8.0f || x > tempoLane_.x + tempoLane_.w + 8.0f)
            continue;
        const float y = bpmY(points[i].bpm);
        const bool on = static_cast<int>(i) == hoverPoint_ || static_cast<int>(i) == dragPoint_;
        const bool selected = static_cast<int>(i) == pointSelected;
        const float d = on || selected ? 13.0f : 10.0f;
        if (selected)
            list.add(theme.accent({x - d * 0.5f - 3.0f, y - d * 0.5f - 3.0f, d + 6.0f, d + 6.0f}, Shape::Circle, 0.3f));
        list.add(theme.surface({x - d * 0.5f, y - d * 0.5f, d, d}, Shape::Circle, Elevation::Raised, on ? 0.75f : 0.45f, Finish::Convex));
        list.add(theme.accent({x - d * 0.5f + 2.5f, y - d * 0.5f + 2.5f, d - 5.0f, d - 5.0f}, Shape::Circle, on || selected ? 1.0f : 0.8f));
        // The point under the mouse or being dragged says where it is and what it does
        if (on)
            caption(x, y, "Bar " + barText(timing, points[i].beat) + "   " + format("%.1f BPM", points[i].bpm) + (points[i].ramp ? "   ramp" : ""),
                    TextTone::Accent);
    }
    // Where a click would add a point: a faint knob, a guide down the lane, and its place and tempo
    if (ghostBeat_ > 0.0 && dragPoint_ < 0) {
        const float x = beatX(ghostBeat_), y = bpmY(ghostBpm_);
        list.add(theme.ink({x, tempoLane_.y + 2.0f, 1.0f, tempoLane_.h - 4.0f}, Shape::Rounded, TextTone::Muted, true, 0.25f));
        list.add(theme.accent({x - 5.0f, y - 5.0f, 10.0f, 10.0f}, Shape::Circle, 0.35f));
        caption(x, y, "Bar " + barText(timing, ghostBeat_) + "   " + format("%.1f BPM", ghostBpm_), TextTone::Secondary);
    }
    if (points.size() == 1 && !hoverTempo_ && endX > tempoLane_.x + 420.0f)
        list.add(theme.text("Click to add a tempo change; right-click a point to ramp to the next",
                            {tempoLane_.x + 14.0f, tempoLane_.y + tempoLane_.h - 20.0f, 420.0f, 16.0f}, TextStyle::Caption,
                            TextTone::Muted, TextAlign::Start));
    list.popClip();
}

void TimingLanes::drawHeaders(DrawList &list, const Theme &theme) const
{
    const audio::TempoMap &timing = project().timing;
    const double playhead = hooks_.position ? std::max(0.0, hooks_.position()) : 0.0;
    list.add(theme.text("Signature", {signatureHeader_.x + 12.0f, signatureHeader_.y, 120.0f, signatureHeader_.h}, TextStyle::Caption,
                        TextTone::Secondary, TextAlign::Start));
    list.add(theme.text(signatureText(timing.signatureAt(playhead)),
                        {signatureHeader_.x + 120.0f, signatureHeader_.y, signatureHeader_.w - 132.0f, signatureHeader_.h},
                        TextStyle::Caption, TextTone::Accent, TextAlign::End));
    list.add(theme.text("Tempo", {tempoHeader_.x + 12.0f, tempoHeader_.y + 6.0f, 120.0f, 18.0f}, TextStyle::Caption,
                        TextTone::Secondary, TextAlign::Start));
    const double bpm = timing.bpmAt(playhead);
    const bool ramping = timing.slopeAt(playhead) != 0.0;
    list.add(theme.text(format("%.1f BPM", bpm) + (ramping ? (timing.slopeAt(playhead) > 0.0 ? "  rising" : "  falling") : ""),
                        {tempoHeader_.x + 12.0f, tempoHeader_.y + 26.0f, 160.0f, 18.0f}, TextStyle::Caption, TextTone::Accent,
                        TextAlign::Start));
    // The lane's scale, beside it
    double low = 0.0, high = 0.0;
    tempoRange(low, high);
    const float right = tempoHeader_.x + tempoHeader_.w - 12.0f;
    for (double value : {high, (low + high) / 2.0, low})
        list.add(theme.text(format("%.0f", value), {right - 60.0f, bpmY(value) - 9.0f, 60.0f, 18.0f}, TextStyle::Caption,
                            TextTone::Muted, TextAlign::End));
}

uint64_t TimingLanes::key() const
{
    Fingerprint fp;
    fp.add(signatureLane_);
    fp.add(tempoLane_);
    fp.add(hoverPoint_);
    fp.add(hoverMarker_);
    fp.add(hoverTempo_);
    fp.add(hoverSignature_);
    fp.add(ghostBeat_);
    fp.add(ghostBpm_);
    fp.add(ghostBar_);
    fp.add(dragPoint_);
    fp.add(dragMarker_);
    fp.add(dragLow_);
    fp.add(dragHigh_);
    fp.add(selectedBeat_);
    fp.add(selectedBar_);
    return fp.value();
}

}
