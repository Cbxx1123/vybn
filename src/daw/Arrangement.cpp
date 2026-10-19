#include "Arrangement.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "Widgets.h"

namespace daw {

using namespace ui;

namespace {

constexpr float kHeaderWidth = 268.0f;
constexpr float kRulerHeight = 30.0f;
constexpr float kTrackHeight = 76.0f;
constexpr float kBarHeight = 10.0f; // the scroll bar under the lanes
constexpr float kGap = 10.0f;

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

TrackHeader::TrackHeader(ProjectStore &store, int index, const TransportHooks &hooks)
    : Container(Direction::Row), store_(store), index_(index), chooseInstrument_(hooks.chooseInstrument)
{
    setAlign(Align::Center);
    setPadding(10.0f);
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
    caption_ = &names.add<LiveLabel>([&store, index, input] {
        if (input)
            return input(index);
        const Track *t = store.project().track(index);
        return std::string(t ? kindName(t->kind) : "");
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
    add<SmallToggle>("M", muteGet, muteSet);
    add<SmallToggle>("S", soloGet, soloSet);
    add<SmallToggle>("R", armGet, armSet);
}

void TrackHeader::arrange(const Rect &rect, const Theme &theme)
{
    // The names get the width the number and the M/S/R toggles leave, and cut themselves short
    // to fit, so the toggles (the record arm especially) stay inside the header
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
    for (const auto &child : children_) {
        if (dynamic_cast<SmallToggle *>(child.get()) && child->rect().contains(input.mouse))
            return;
    }
    store_.select(index_);
    // The instrument line of an instrument track: choose its instrument
    const Track *t = project().track(index_);
    if (t && t->kind == TrackKind::Instrument && caption_ && caption_->rect().contains(input.mouse) && chooseInstrument_)
        chooseInstrument_(index_, caption_->rect());
}

void TrackHeader::draw(DrawList &list, const Theme &theme) const
{
    if (project().selectedTrack == index_)
        list.add(theme.surface(rect_, Shape::Soft, Elevation::Inset, 0.6f));
    Container::draw(list, theme);
}

// ArrangementView

ArrangementView::ArrangementView(ProjectStore &store, TransportHooks transport)
    : store_(store), transport_(std::move(transport))
{
    rebuild();
}

void ArrangementView::rebuild()
{
    shownStructure_ = store_.structureRevision();
    shownShape_ = trackShape(project());
    headers_.clear();
    for (size_t i = 0; i < project().tracks.size(); ++i)
        headers_.push_back(std::make_unique<TrackHeader>(store_, static_cast<int>(i), transport_));
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
    clampScroll();

    for (size_t i = 0; i < headers_.size(); ++i) {
        const Rect r{headerArea_.x, lanes_.y + static_cast<float>(i) * kTrackHeight - scrollY_ + 4.0f, headerArea_.w,
                     kTrackHeight - 8.0f};
        headers_[i]->arrange(r, theme);
    }
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
    const int row = static_cast<int>(offset / kTrackHeight);
    return row < static_cast<int>(project().tracks.size()) ? row : -1;
}

Rect ArrangementView::clipRect(int track, const Clip &clip) const
{
    return {beatX(clip.start) + 1.0f, lanes_.y + static_cast<float>(track) * kTrackHeight - scrollY_ + 6.0f,
            static_cast<float>(clip.length) * pixelsPerBeat_ - 2.0f, kTrackHeight - 12.0f};
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
    const float maxY = std::max(0.0f, static_cast<float>(project().tracks.size()) * kTrackHeight - lanes_.h);
    scrollX_ = std::clamp(scrollX_, 0.0f, maxX);
    scrollY_ = std::clamp(scrollY_, 0.0f, maxY);
}

void ArrangementView::handleInput(const Input &input)
{
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
        // A press anywhere but on the name being edited finishes it
        if (input.pressed && headers_[i]->name().editing() && !headers_[i]->name().hitTest(input.mouse))
            headers_[i]->name().endEdit();
        headers_[i]->handleInput(headerInput);
    }

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

    // What's under the mouse in the lanes
    hoveredTrack_ = -1;
    hoveredClip_ = -1;
    if (well_.contains(input.mouse)) {
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

    if (input.pressed) {
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
            // Onto another track
            const float y = std::clamp(input.mouse.y, well_.y, well_.y + well_.h - 1.0f);
            const int row = rowAt(y);
            if (row >= 0 && row != project().selectedTrack) {
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
    if (r.x > well_.x + well_.w || r.x + r.w < well_.x || r.y > well_.y + well_.h || r.y + r.h < well_.y)
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
                std::vector<Vec2> points;
                points.reserve(static_cast<size_t>(steps) + 1);
                for (int k = 0; k <= steps; ++k) {
                    const double f = static_cast<double>(k) / steps;
                    const double level = in ? std::sin(f * 1.5707963) : std::cos(f * 1.5707963);
                    const float x = r.x + static_cast<float>((from + f * beats) * pixelsPerBeat_);
                    const float y = body.y + static_cast<float>(1.0 - level) * body.h;
                    points.push_back({x, y});
                }
                list.addPolyline(points.data(), points.size(), theme.accentLine({}, {}, 1.5f, 0.7f));
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

void ArrangementView::draw(DrawList &list, const Theme &theme) const
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
    for (int i = 1; i <= tracks; ++i) {
        const float y = lanes_.y + static_cast<float>(i) * kTrackHeight - scrollY_;
        list.add(theme.ink({well_.x, y, well_.w, 1.0f}, Shape::Rounded, TextTone::Muted, true, 0.25f));
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
        const int clips = static_cast<int>(project().tracks[static_cast<size_t>(t)].clips.size());
        for (int c = 0; c < clips; ++c)
            drawClip(list, theme, t, c);
    }
    list.popClip();

    // Playhead, across the ruler and the lanes
    if (transport_.position) {
        const float x = beatX(transport_.position());
        if (x >= ruler_.x - 1.0f && x <= ruler_.x + ruler_.w + 1.0f) {
            list.add(theme.accent({x - 1.0f, rulerBottom - 4.0f, 2.0f, well_.y + well_.h - rulerBottom + 2.0f},
                                  Shape::Rounded, 0.9f));
            list.add(theme.accent({x - 6.0f, rulerBottom - 12.0f, 12.0f, 9.0f}, Shape::TriangleDown));
        }
    }

    // Track headers, scrolled with the lanes
    list.pushClip(headerArea_);
    const size_t count = std::min(headers_.size(), project().tracks.size());
    for (size_t i = 0; i < count; ++i)
        headers_[i]->draw(list, theme);
    list.popClip();

    // Where a dragged instrument would go: its row, header and lane, outlined in the accent
    if (dropRow_ >= 0) {
        const Rect area{headerArea_.x, lanes_.y, lanes_.x + lanes_.w - headerArea_.x, lanes_.h};
        const Rect r{headerArea_.x, lanes_.y + static_cast<float>(dropRow_) * kTrackHeight - scrollY_ + 3.0f, area.w,
                     kTrackHeight - 6.0f};
        list.pushClip(area);
        list.add(theme.accent(r, Shape::Rounded, 0.07f));
        const float t = 2.0f;
        list.add(theme.accent({r.x + 8.0f, r.y, r.w - 16.0f, t}, Shape::Pill, 0.8f));
        list.add(theme.accent({r.x + 8.0f, r.y + r.h - t, r.w - 16.0f, t}, Shape::Pill, 0.8f));
        if (!dropHint_.empty())
            list.add(theme.text(dropHint_, r, theme.fontSize(TextStyle::Body), TextAlign::Center, theme.textColor(TextTone::Accent)));
        list.popClip();
    }

    // Scroll bar for time
    list.add(theme.surface(bar_, Shape::Pill, Elevation::Inset, 0.4f));
    if (contentWidth() > lanes_.w)
        list.add(theme.surface(thumb().inset(1.0f), Shape::Pill, Elevation::Raised, draggingBar_ ? 0.7f : 0.45f,
                               Finish::Convex));
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
