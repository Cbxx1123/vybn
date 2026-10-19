#include "StepSequencer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <string>
#include <utility>

namespace daw {

using namespace ui;

namespace {

constexpr float kHeaderGap = 4.0f;   // between the step numbers and the grid
constexpr float kLaneGap = 6.0f;     // between the grid and the velocity lane
constexpr float kCellGap = 2.0f;     // a step's cell is this far in from its column
constexpr float kCellInset = 3.0f;   // and this far in from its row
constexpr float kRing = 1.5f;        // the outline of a muted step
constexpr float kMessageSeconds = 3.0f;
constexpr double kAuditionSeconds = 0.15;
constexpr double kForever = std::numeric_limits<double>::infinity();

// The row under `y` (a column of rows starting at `top`, scrolled by `scroll`), or -1 outside 0..count-1
int rowUnder(float y, float top, float scroll, int count)
{
    const float r = std::floor((y - top + scroll) / StepSequencerView::kRowHeight);
    return r < 0.0f || r >= static_cast<float>(count) ? -1 : static_cast<int>(r);
}

// The step under `x` (columns of `width` starting at `left`, scrolled by `scroll`), or -1 outside 0..count-1
int stepUnder(float x, float left, float scroll, float width, int count)
{
    const float s = std::floor((x - left + scroll) / width);
    return s < 0.0f || s >= static_cast<float>(count) ? -1 : static_cast<int>(s);
}

float maxScroll(float content, float view)
{
    return std::max(0.0f, content - view);
}

// Keeps every field in the range the view works in
void clampView(StepSequencerView::ViewSettings &v)
{
    v.step = std::clamp(v.step, 0, static_cast<int>(stepChoices().size()) - 1);
    v.swing = std::clamp(v.swing, 0.0, 1.0);
    v.gate = std::clamp(v.gate, 0.05, 1.0);
    v.velocity = std::clamp(v.velocity, 0.01f, 1.0f);
}

// A plain number, whole token; false for anything else
bool parseNumber(const std::string &text, double &out)
{
    if (text.empty())
        return false;
    char *end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0' || !std::isfinite(value) || std::fabs(value) > 1e9)
        return false;
    out = value;
    return true;
}

}

bool StepSequencerView::ViewSettings::operator==(const ViewSettings &o) const
{
    return step == o.step && swing == o.swing && gate == o.gate && velocity == o.velocity && audition == o.audition;
}

StepSequencerView::StepSequencerView(ProjectStore &store, StepSequencerHooks hooks) : store_(store), hooks_(std::move(hooks))
{
}

const Clip *StepSequencerView::clip() const
{
    const Track *t = project().track(project().selectedTrack);
    if (!t || t->kind != TrackKind::Instrument)
        return nullptr;
    return project().clip();
}

StepGrid StepSequencerView::grid() const
{
    const double length = clip() ? clip()->length : 4.0;
    return stepGrid(length, stepChoices()[static_cast<size_t>(view_.step)].beats, view_.swing);
}

std::vector<Note> *StepSequencerView::editNotes()
{
    if (!clip())
        return nullptr;
    return &store_.editClip(project().selectedTrack, project().selectedClip).notes;
}

void StepSequencerView::say(std::string text)
{
    message_ = std::move(text);
    messageTime_ = now_;
}

std::string StepSequencerView::message() const
{
    return now_ - messageTime_ < kMessageSeconds ? message_ : std::string();
}

void StepSequencerView::setView(const ViewSettings &view)
{
    view_ = view;
    clampView(view_);
}

std::string StepSequencerView::writeView(const ViewSettings &view)
{
    std::ostringstream out;
    out.precision(9);
    out << "step=" << view.step << " swing=" << view.swing << " gate=" << view.gate << " velocity=" << view.velocity
        << " audition=" << (view.audition ? 1 : 0);
    return out.str();
}

StepSequencerView::ViewSettings StepSequencerView::readView(const std::string &text)
{
    ViewSettings v;
    std::istringstream in(text);
    std::string token;
    while (in >> token) {
        const size_t eq = token.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = token.substr(0, eq);
        double value = 0.0;
        if (!parseNumber(token.substr(eq + 1), value))
            continue;
        if (key == "step")
            v.step = static_cast<int>(std::lround(value));
        else if (key == "swing")
            v.swing = value;
        else if (key == "gate")
            v.gate = value;
        else if (key == "velocity")
            v.velocity = static_cast<float>(value);
        else if (key == "audition")
            v.audition = value != 0.0;
    }
    clampView(v);
    return v;
}

void StepSequencerView::setStepChoice(int index)
{
    view_.step = std::clamp(index, 0, static_cast<int>(stepChoices().size()) - 1);
}

void StepSequencerView::setSwing(double swing)
{
    swing = std::clamp(swing, 0.0, 1.0);
    if (swing == view_.swing)
        return;
    if (std::vector<Note> *notes = editNotes())
        applySwing(*notes, grid(), swing); // the grid still has the old swing here
    view_.swing = swing;
}

void StepSequencerView::setGate(double gate)
{
    view_.gate = std::clamp(gate, 0.05, 1.0);
}

void StepSequencerView::setVelocity(float velocity)
{
    view_.velocity = std::clamp(velocity, 0.01f, 1.0f);
}

void StepSequencerView::setAudition(bool on)
{
    view_.audition = on;
}

std::vector<int> StepSequencerView::rows() const
{
    static const std::vector<Note> none;
    const Clip *c = clip();
    std::vector<int> kit;
    for (int key : defaultKit())
        if (std::find(hidden_.begin(), hidden_.end(), key) == hidden_.end())
            kit.push_back(key);
    kit.insert(kit.end(), added_.begin(), added_.end());
    return stepRows(c ? c->notes : none, kit);
}

void StepSequencerView::addRow(int key)
{
    key = std::clamp(key, 1, 127);
    hidden_.erase(std::remove(hidden_.begin(), hidden_.end(), key), hidden_.end());
    const std::vector<int> &kit = defaultKit();
    if (std::find(kit.begin(), kit.end(), key) == kit.end() && std::find(added_.begin(), added_.end(), key) == added_.end())
        added_.push_back(key);
    selectRow(key);
    say("Added " + drumName(key) + " row");
}

void StepSequencerView::removeRow(int key)
{
    clearRow(key);
    added_.erase(std::remove(added_.begin(), added_.end(), key), added_.end());
    const std::vector<int> &kit = defaultKit();
    if (std::find(kit.begin(), kit.end(), key) != kit.end() && std::find(hidden_.begin(), hidden_.end(), key) == hidden_.end())
        hidden_.push_back(key);
    if (selectedRow_ == key)
        selectedRow_ = -1;
    say("Removed " + drumName(key));
}

void StepSequencerView::selectRow(int key)
{
    selectedRow_ = key;
}

void StepSequencerView::clearRow(int key)
{
    std::vector<Note> *notes = editNotes();
    if (!notes)
        return;
    daw::clearRow(*notes, key, grid());
    say("Cleared " + drumName(key));
}

void StepSequencerView::shiftRow(int key, int steps)
{
    if (steps == 0)
        return;
    std::vector<Note> *notes = editNotes();
    if (!notes)
        return;
    daw::shiftRow(*notes, key, grid(), steps);
    say("Shifted " + drumName(key) + (steps > 0 ? " right" : " left"));
}

void StepSequencerView::fillRow(int key, int every, int offset)
{
    std::vector<Note> *notes = editNotes();
    if (!notes)
        return;
    const StepGrid g = grid();
    daw::setRow(*notes, key, g, everyPattern(g.count, every, offset, view_.velocity), view_.gate);
    const int steps = std::max(1, every);
    say("Filled " + drumName(key) + " every " + std::to_string(steps) + (steps == 1 ? " step" : " steps"));
}

void StepSequencerView::euclidRow(int key, int pulses, int rotation)
{
    std::vector<Note> *notes = editNotes();
    if (!notes)
        return;
    const StepGrid g = grid();
    const std::vector<bool> hits = euclidean(pulses, g.count, rotation);
    std::vector<float> pattern(static_cast<size_t>(g.count), 0.0f);
    for (size_t i = 0; i < pattern.size() && i < hits.size(); ++i)
        pattern[i] = hits[i] ? view_.velocity : 0.0f;
    daw::setRow(*notes, key, g, pattern, view_.gate);
    say("Euclidean " + std::to_string(pulses) + " of " + std::to_string(g.count) + " on " + drumName(key));
}

void StepSequencerView::randomRow(int key, double density, uint32_t seed)
{
    std::vector<Note> *notes = editNotes();
    if (!notes)
        return;
    const StepGrid g = grid();
    daw::setRow(*notes, key, g, randomPattern(g.count, density, view_.velocity, seed), view_.gate);
    say("Random " + std::to_string(std::lround(density * 100.0)) + "% on " + drumName(key));
}

bool StepSequencerView::rowMuted(int key) const
{
    const Clip *c = clip();
    return c && daw::rowMuted(c->notes, key, grid());
}

void StepSequencerView::toggleRowMute(int key)
{
    const Clip *c = clip();
    if (!c)
        return;
    bool hasNotes = false;
    for (const Note &n : c->notes)
        hasNotes = hasNotes || n.key == key;
    if (!hasNotes)
        return;
    std::vector<Note> *notes = editNotes();
    if (!notes)
        return;
    const StepGrid g = grid();
    const bool muted = daw::rowMuted(*notes, key, g);
    daw::setRowMuted(*notes, key, g, !muted);
    say(std::string(muted ? "Unmuted " : "Muted ") + drumName(key));
}

void StepSequencerView::copyRow(int key)
{
    const Clip *c = clip();
    if (!c)
        return;
    clipboard_ = rowPattern(c->notes, key, grid());
    say("Copied " + drumName(key));
}

void StepSequencerView::pasteRow(int key)
{
    if (clipboard_.empty())
        return;
    std::vector<Note> *notes = editNotes();
    if (!notes)
        return;
    const StepGrid g = grid();
    std::vector<float> pattern = clipboard_;
    pattern.resize(static_cast<size_t>(g.count), 0.0f); // cut or padded to this grid
    daw::setRow(*notes, key, g, pattern, view_.gate);
    say("Pasted " + drumName(key));
}

ui::Vec2 StepSequencerView::measure(const Theme &) const
{
    return {400.0f, 160.0f};
}

void StepSequencerView::arrange(const Rect &rect, const Theme &)
{
    rect_ = rect;
    const bool laneShown = selectedRow_ >= 0;
    const float top = rect.y + kHeaderHeight + kHeaderGap;
    const float gridX = rect.x + kNameWidth;
    const float gridW = std::max(0.0f, rect.w - kNameWidth);
    const float reserve = laneShown ? kLaneGap + kLaneHeight : 0.0f;
    const float gridH = std::max(0.0f, rect.h - (top - rect.y) - reserve);
    header_ = {gridX, rect.y, gridW, kHeaderHeight};
    grid_ = {gridX, top, gridW, gridH};
    names_ = {rect.x, top, kNameWidth, gridH};
    lane_ = {gridX, top + gridH + kLaneGap, gridW, laneShown ? kLaneHeight : 0.0f};

    const int count = std::max(1, grid().count);
    stepWidth_ = std::max(kMinStepWidth, gridW / static_cast<float>(count));
    const int rowCount = static_cast<int>(rows().size());
    scrollX_ = std::clamp(scrollX_, 0.0f, maxScroll(static_cast<float>(count) * stepWidth_, gridW));
    scrollY_ = std::clamp(scrollY_, 0.0f, maxScroll(static_cast<float>(rowCount + 1) * kRowHeight, gridH));
    addRow_ = {names_.x, grid_.y + static_cast<float>(rowCount) * kRowHeight - scrollY_, kNameWidth, kRowHeight};
}

void StepSequencerView::handleInput(const Input &input)
{
    now_ = input.time;
    const Clip *c = clip();
    const StepGrid g = grid();
    const std::vector<int> keys = rows();
    const int n = static_cast<int>(keys.size());
    const Vec2 m = input.mouse;
    const bool onGrid = c && grid_.contains(m);
    const bool onNames = c && names_.contains(m);
    const bool onLane = c && selectedRow_ >= 0 && lane_.contains(m);
    // Rows are 0..n-1; the add line under them is row n (names column only)
    const int line = c && (onGrid || onNames) ? rowUnder(m.y, grid_.y, scrollY_, n + 1) : -1;
    const int cellRow = onGrid && line >= 0 && line < n ? line : -1;
    const int nameRow = onNames && line >= 0 && line < n ? line : -1;
    const bool addHit = onNames && line == n;
    addHot_ = addHit && drag_ == Drag::None;
    const int step = onGrid ? stepUnder(m.x, grid_.x, scrollX_, stepWidth_, g.count) : -1;
    const float addY = grid_.y + static_cast<float>(n) * kRowHeight - scrollY_;

    auto stopAudition = [this]() {
        if (auditioning_ >= 0 && hooks_.auditionOff)
            hooks_.auditionOff(auditioning_);
        auditioning_ = -1;
    };
    auto startAudition = [this, &stopAudition](int key, float velocity, double until) {
        stopAudition();
        if (hooks_.auditionOn)
            hooks_.auditionOn(key, velocity);
        auditioning_ = key;
        auditionUntil_ = until;
    };
    // Turns a step on or off; one clip edit, and a turned-on step plays its key when hearing is on
    auto setCell = [this, &g, &startAudition](int key, int at, bool on) {
        if (std::vector<Note> *notes = editNotes())
            daw::setStep(*notes, key, g, at, on, view_.velocity, view_.gate);
        if (on && view_.audition)
            startAudition(key, view_.velocity, now_ + kAuditionSeconds);
    };
    // Sets the velocity of the selected row's step under x from the height y in the lane
    auto laneVelocity = [this, &g](Vec2 p) {
        const Clip *now = clip();
        const int at = stepUnder(p.x, grid_.x, scrollX_, stepWidth_, g.count);
        if (!now || at < 0 || selectedRow_ < 0)
            return;
        const int idx = daw::findStep(now->notes, selectedRow_, g, at);
        if (idx < 0)
            return;
        const float v = std::clamp(1.0f - (p.y - lane_.y) / lane_.h, 0.01f, 1.0f);
        if (std::fabs(now->notes[static_cast<size_t>(idx)].velocity - v) < 1e-4f)
            return;
        if (std::vector<Note> *notes = editNotes())
            daw::setStepVelocity(*notes, selectedRow_, g, at, v);
    };

    // Release ends the drag (and a row's audition)
    if (!input.down) {
        if (drag_ == Drag::Name)
            stopAudition();
        drag_ = Drag::None;
    }
    if (auditioning_ >= 0 && now_ >= auditionUntil_)
        stopAudition();

    // The wheel scrolls the rows; Shift or a sideways wheel scrolls the steps
    if ((onGrid || onNames) && (input.scroll != 0.0f || input.scrollX != 0.0f)) {
        const bool across = input.scrollX != 0.0f || input.shift;
        const float wheel = input.scrollX != 0.0f ? input.scrollX : input.scroll;
        if (across)
            scrollX_ -= wheel * stepWidth_ * 2.0f;
        else
            scrollY_ -= wheel * kRowHeight * 2.0f;
        input.scroll = input.scrollX = 0.0f;
    }

    // Right-click on a row's name or a step: the row's menu
    const int hitRow = nameRow >= 0 ? nameRow : cellRow;
    if (input.secondaryPressed && hitRow >= 0 && (nameRow >= 0 || step >= 0)) {
        input.secondaryPressed = false;
        if (hooks_.rowMenu)
            hooks_.rowMenu(keys[static_cast<size_t>(hitRow)], m);
    }

    if (input.pressed) {
        if (cellRow >= 0 && step >= 0) {
            const int key = keys[static_cast<size_t>(cellRow)];
            paintOn_ = !daw::stepOn(c->notes, key, g, step); // c is read before the edit
            setCell(key, step, paintOn_);
            drag_ = Drag::Paint;
        } else if (nameRow >= 0) {
            const int key = keys[static_cast<size_t>(nameRow)];
            selectRow(key);
            drag_ = Drag::Name;
            if (view_.audition)
                startAudition(key, view_.velocity, kForever);
        } else if (addHit) {
            if (hooks_.addRowMenu)
                hooks_.addRowMenu({names_.x, addY + kRowHeight});
        } else if (onLane) {
            drag_ = Drag::Lane;
            laneVelocity(m);
        }
    }

    // While held: paint the cell under the mouse, or set the lane's velocity
    if (input.down && drag_ == Drag::Paint && onGrid) {
        const int row = rowUnder(m.y, grid_.y, scrollY_, n);
        const int at = stepUnder(m.x, grid_.x, scrollX_, stepWidth_, g.count);
        const Clip *now = clip();
        if (now && row >= 0 && at >= 0) {
            const int key = keys[static_cast<size_t>(row)];
            if (daw::stepOn(now->notes, key, g, at) != paintOn_)
                setCell(key, at, paintOn_);
        }
    } else if (input.down && drag_ == Drag::Lane) {
        laneVelocity(m);
    }

    // The grid, names and lane clamp their scroll to the content
    scrollX_ = std::clamp(scrollX_, 0.0f, maxScroll(static_cast<float>(g.count) * stepWidth_, grid_.w));
    scrollY_ = std::clamp(scrollY_, 0.0f, maxScroll(static_cast<float>(n + 1) * kRowHeight, grid_.h));

    hint_.clear();
    if (c) {
        if (cellRow >= 0)
            hint_ = "Click to turn a step on or off, drag to paint";
        else if (nameRow >= 0)
            hint_ = "Click to hear and select the row; right-click for its tools";
        else if (addHit)
            hint_ = "Add a row for another drum";
        else if (onLane)
            hint_ = "Drag to set the step's velocity";
    }
    if (cellRow >= 0 || nameRow >= 0 || addHit)
        input.setCursor(engine::Cursor::Hand);
}

void StepSequencerView::draw(DrawList &list, const Theme &theme) const
{
    const Clip *c = clip();
    if (!c) {
        list.add(theme.surface(rect_, Shape::Soft, Elevation::Inset, 0.6f));
        list.add(theme.text("Select an instrument clip to edit its steps", rect_, TextStyle::Body, TextTone::Muted,
                            TextAlign::Center));
        return;
    }

    const StepGrid g = grid();
    const std::vector<int> keys = rows();
    const int n = static_cast<int>(keys.size());
    const int perBeat = std::max(1, static_cast<int>(std::lround(1.0 / g.step)));
    int playingStep = -1;
    if (hooks_.playing && hooks_.playing() && hooks_.position) {
        const double at = hooks_.position() - c->start;
        if (at >= 0.0 && at < c->length)
            playingStep = stepAt(g, at);
    }

    // The steps and rows on screen
    const int firstStep = std::max(0, static_cast<int>(std::floor(scrollX_ / stepWidth_)));
    const int lastStep = std::min(g.count, static_cast<int>(std::ceil((scrollX_ + grid_.w) / stepWidth_)));
    const int firstRow = std::max(0, static_cast<int>(std::floor(scrollY_ / kRowHeight)));
    const int lastRow = std::min(n, static_cast<int>(std::ceil((scrollY_ + grid_.h) / kRowHeight)));
    auto colX = [this](int s) { return grid_.x + static_cast<float>(s) * stepWidth_ - scrollX_; };
    auto rowY = [this](int i) { return grid_.y + static_cast<float>(i) * kRowHeight - scrollY_; };

    // Header: a step number at each beat, and the playing step's
    list.pushClip(header_);
    for (int s = firstStep; s < lastStep; ++s) {
        const bool groupStart = s % perBeat == 0;
        if (!groupStart && s != playingStep)
            continue;
        const TextTone tone = s == playingStep ? TextTone::Accent : TextTone::Muted;
        list.add(theme.text(std::to_string(s + 1), Rect{colX(s) + 3.0f, header_.y, stepWidth_ * static_cast<float>(perBeat), header_.h},
                            TextStyle::Caption, tone, TextAlign::Start));
    }
    list.popClip();

    // The grid: an inset well, beats in alternate shades, the playing step's column lit
    list.add(theme.surface(grid_, Shape::Soft, Elevation::Inset, 0.6f));
    list.pushClip(grid_);
    for (int b = firstStep / perBeat; b * perBeat < lastStep; ++b) {
        if (b % 2 == 0)
            continue;
        const int start = b * perBeat;
        const int end = std::min(g.count, start + perBeat);
        list.add(theme.ink(Rect{colX(start), grid_.y, static_cast<float>(end - start) * stepWidth_, grid_.h}, Shape::Rounded,
                           TextTone::Primary, true, 0.04f));
    }
    if (playingStep >= 0)
        list.add(theme.accent(Rect{colX(playingStep), grid_.y, stepWidth_, grid_.h}, Shape::Rounded, 0.08f));

    auto ring = [&list, &theme](const Rect &r) {
        list.add(theme.accent(Rect{r.x, r.y, r.w, kRing}, Shape::Rounded, 0.7f));
        list.add(theme.accent(Rect{r.x, r.y + r.h - kRing, r.w, kRing}, Shape::Rounded, 0.7f));
        list.add(theme.accent(Rect{r.x, r.y, kRing, r.h}, Shape::Rounded, 0.7f));
        list.add(theme.accent(Rect{r.x + r.w - kRing, r.y, kRing, r.h}, Shape::Rounded, 0.7f));
    };
    for (int i = firstRow; i < lastRow; ++i) {
        const int key = keys[static_cast<size_t>(i)];
        const float y = rowY(i);
        const bool muted = daw::rowMuted(c->notes, key, g);
        for (int s = firstStep; s < lastStep; ++s) {
            const Rect cell{colX(s) + kCellGap, y + kCellInset, stepWidth_ - 2.0f * kCellGap, kRowHeight - 2.0f * kCellInset};
            const int at = daw::findStep(c->notes, key, g, s);
            if (at < 0) {
                list.add(theme.surface(cell, Shape::Soft, Elevation::Inset, 0.35f));
            } else if (muted) {
                list.add(theme.surface(cell, Shape::Pill, Elevation::Inset, 0.4f));
                ring(cell);
            } else {
                const float velocity = c->notes[static_cast<size_t>(at)].velocity;
                list.add(theme.surface(cell, Shape::Pill, Elevation::Raised, 0.5f, Finish::Convex));
                list.add(theme.accent(cell, Shape::Pill, 0.35f + 0.65f * velocity));
            }
        }
    }
    list.popClip();

    // Row names: the selected one raised, the others secondary
    list.pushClip(names_);
    for (int i = firstRow; i < lastRow; ++i) {
        const float y = rowY(i);
        const Rect box{names_.x + 4.0f, y + 2.0f, kNameWidth - 8.0f, kRowHeight - 4.0f};
        const bool selected = keys[static_cast<size_t>(i)] == selectedRow_;
        if (selected)
            list.add(theme.surface(box, Shape::Soft, Elevation::Raised, 0.45f));
        list.add(theme.text(drumName(keys[static_cast<size_t>(i)]), Rect{box.x + 6.0f, box.y, box.w - 10.0f, box.h},
                            TextStyle::Caption, selected ? TextTone::Primary : TextTone::Secondary, TextAlign::Start));
    }
    const float addY = rowY(n);
    if (addY < names_.y + names_.h && addY + kRowHeight > names_.y)
        list.add(theme.text("+ Add row", Rect{names_.x + 10.0f, addY, kNameWidth - 14.0f, kRowHeight}, TextStyle::Caption,
                            addHot_ ? TextTone::Accent : TextTone::Muted, TextAlign::Start));
    list.popClip();

    // The velocity lane of the selected row: a bar per step that is on
    if (selectedRow_ >= 0 && lane_.h > 0.0f) {
        list.add(theme.surface(lane_, Shape::Soft, Elevation::Inset, 0.5f));
        list.pushClip(lane_.inset(2.0f));
        for (int s = firstStep; s < lastStep; ++s) {
            const int at = daw::findStep(c->notes, selectedRow_, g, s);
            if (at < 0)
                continue;
            const float h = std::max(2.0f, c->notes[static_cast<size_t>(at)].velocity * (lane_.h - 8.0f));
            list.add(theme.accent(Rect{colX(s) + kCellGap, lane_.y + lane_.h - 4.0f - h, stepWidth_ - 2.0f * kCellGap, h},
                                  Shape::Soft, 0.85f));
        }
        list.popClip();
        list.add(theme.text("Velocity", Rect{names_.x + 10.0f, lane_.y, kNameWidth - 14.0f, lane_.h}, TextStyle::Caption,
                            TextTone::Muted, TextAlign::Start));
    }
}

}
