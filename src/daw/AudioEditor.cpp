#include "AudioEditor.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace daw {

using namespace ui;

namespace {

constexpr float kRulerHeight = 26.0f;
constexpr float kEdge = 6.0f;    // how close to an edge counts as grabbing it
constexpr float kHandle = 9.0f;  // the fade handles' size
constexpr double kHalfPi = 1.5707963267948966;

}

AudioEditorView::AudioEditorView(ProjectStore &store, AudioEditorHooks hooks) : store_(store), hooks_(std::move(hooks))
{
}

const Clip *AudioEditorView::clip() const
{
    const Track *t = project().track(project().selectedTrack);
    if (!t || t->kind != TrackKind::Audio)
        return nullptr;
    return project().clip();
}

void AudioEditorView::zoom(float factor)
{
    const double centre = xBeat(area_.x + area_.w * 0.5f);
    pixelsPerBeat_ = std::clamp(pixelsPerBeat_ * factor, 4.0f, 2000.0f);
    viewStart_ = centre - (area_.w * 0.5f) / pixelsPerBeat_;
}

Vec2 AudioEditorView::measure(const Theme &) const
{
    return {400.0f, 160.0f};
}

void AudioEditorView::arrange(const Rect &rect, const Theme &)
{
    rect_ = rect;
    ruler_ = {rect.x, rect.y, rect.w, kRulerHeight};
    area_ = {rect.x, rect.y + kRulerHeight + 6.0f, rect.w, std::max(0.0f, rect.h - kRulerHeight - 6.0f)};
    if (fitPending_ && area_.w > 0.0f) {
        if (const Clip *c = clip())
            fitClip(*c);
        fitPending_ = false;
    }
}

void AudioEditorView::fitClip(const Clip &c)
{
    // The clip with a little room either side
    const double span = std::max(0.25, c.length * 1.2);
    pixelsPerBeat_ = std::clamp(static_cast<float>(area_.w / span), 4.0f, 2000.0f);
    viewStart_ = c.start - c.length * 0.1;
}

float AudioEditorView::beatX(double beat) const
{
    return area_.x + static_cast<float>((beat - viewStart_) * pixelsPerBeat_);
}

double AudioEditorView::xBeat(float x) const
{
    return viewStart_ + (x - area_.x) / pixelsPerBeat_;
}

Rect AudioEditorView::clipRect(const Clip &c) const
{
    const float x = beatX(c.start);
    return {x, area_.y, beatX(c.start + c.length) - x, area_.h};
}

AudioEditorView::Drag AudioEditorView::hitTestHandles(const Clip &c, Vec2 p) const
{
    const Rect r = clipRect(c);
    const double spb = project().secondsPerBeat();
    const float inX = beatX(c.start + c.fadeIn / spb);
    const float outX = beatX(c.start + c.length - c.fadeOut / spb);
    if (std::fabs(p.x - inX) <= kHandle && std::fabs(p.y - (r.y + kHandle)) <= kHandle)
        return Drag::FadeIn;
    if (std::fabs(p.x - outX) <= kHandle && std::fabs(p.y - (r.y + kHandle)) <= kHandle)
        return Drag::FadeOut;
    if (p.y >= r.y && p.y <= r.y + r.h) {
        if (std::fabs(p.x - r.x) <= kEdge)
            return Drag::Start;
        if (std::fabs(p.x - (r.x + r.w)) <= kEdge)
            return Drag::End;
    }
    return Drag::None;
}

void AudioEditorView::handleInput(const Input &input)
{
    const Clip *c = clip();
    // A new clip is fitted into view
    if (project().selectedTrack != shownTrack_ || project().selectedClip != shownClip_) {
        shownTrack_ = project().selectedTrack;
        shownClip_ = project().selectedClip;
        drag_ = Drag::None;
        if (c && area_.w > 0.0f)
            fitClip(*c);
        else
            fitPending_ = true;
    }

    if (rect_.contains(input.mouse)) {
        const float wheel = input.scrollX != 0.0f ? input.scrollX : input.scroll;
        if (wheel != 0.0f) {
            viewStart_ -= wheel * 60.0f / pixelsPerBeat_;
            input.scroll = input.scrollX = 0.0f;
        }
    }
    viewStart_ = std::max(-4.0, viewStart_);

    hover_ = c && area_.contains(input.mouse) ? hitTestHandles(*c, input.mouse) : Drag::None;
    if (input.pressed) {
        if (ruler_.contains(input.mouse)) {
            drag_ = Drag::Ruler;
        } else if (area_.contains(input.mouse)) {
            const bool doubleClick = input.time - lastPress_ < 0.3;
            lastPress_ = input.time;
            if (hover_ != Drag::None) {
                drag_ = hover_;
            } else if (doubleClick && c) {
                store_.edit().splitClip(xBeat(input.mouse.x));
                c = clip();
            } else if (hooks_.seek) {
                hooks_.seek(std::max(0.0, xBeat(input.mouse.x)));
            }
        }
    }
    if (!input.down)
        drag_ = Drag::None;

    if (drag_ == Drag::Ruler && hooks_.seek)
        hooks_.seek(std::max(0.0, xBeat(input.mouse.x)));
    if (!c || drag_ == Drag::None || drag_ == Drag::Ruler)
        return;

    const double spb = project().secondsPerBeat();
    const double beat = xBeat(input.mouse.x);
    const double lengthSeconds = c->length * spb;
    const audio::AudioData *data = hooks_.audioFor ? hooks_.audioFor(*c) : nullptr;
    // The drag changes the selected clip as it moves, so the edit is made here, each frame it is dragged
    const int trackIndex = project().selectedTrack;
    const int clipIndex = project().selectedClip;
    Clip &edited = store_.editClip(trackIndex, clipIndex);
    switch (drag_) {
    case Drag::Start:
        project().trimStart(edited, beat);
        break;
    case Drag::End: {
        // Not past the end of the sound file
        double end = std::max(beat, edited.start + 0.0625);
        if (data)
            end = std::min(end, edited.start + (data->seconds() - edited.offset) / spb);
        edited.length = std::max(0.0625, end - edited.start);
        break;
    }
    case Drag::FadeIn:
        edited.fadeIn = std::clamp((beat - edited.start) * spb, 0.0, std::max(0.0, lengthSeconds - edited.fadeOut));
        break;
    case Drag::FadeOut:
        edited.fadeOut = std::clamp((edited.start + edited.length - beat) * spb, 0.0, std::max(0.0, lengthSeconds - edited.fadeIn));
        break;
    default:
        break;
    }
    edited.fadeIn = std::min(edited.fadeIn, edited.length * spb);
    edited.fadeOut = std::min(edited.fadeOut, edited.length * spb - edited.fadeIn);
}

void AudioEditorView::draw(DrawList &list, const Theme &theme) const
{
    const int beatsPerBar = std::max(1, project().beatsPerBar);
    // Ruler
    list.add(theme.surface(ruler_, Shape::Soft, Elevation::Raised, 0.35f));
    list.pushClip(ruler_);
    const double first = std::floor(viewStart_ / beatsPerBar) * beatsPerBar;
    const double last = xBeat(ruler_.x + ruler_.w);
    const float barPixels = pixelsPerBeat_ * static_cast<float>(beatsPerBar);
    int every = 1;
    while (barPixels * static_cast<float>(every) < 50.0f)
        every *= 2;
    for (double b = std::max(0.0, first); b <= last; b += 1.0) {
        const float x = beatX(b);
        const bool bar = std::fmod(b, beatsPerBar) == 0.0;
        if (!bar && pixelsPerBeat_ < 10.0f)
            continue;
        list.add(theme.ink({x, ruler_.y + ruler_.h - (bar ? 10.0f : 5.0f), 1.0f, bar ? 8.0f : 3.0f}, Shape::Rounded,
                           TextTone::Muted, true, 0.8f));
        const int barIndex = static_cast<int>(b) / beatsPerBar;
        if (bar && barIndex % every == 0)
            list.add(theme.text(std::to_string(barIndex + 1), {x + 4.0f, ruler_.y, 40.0f, ruler_.h - 6.0f},
                                TextStyle::Caption, TextTone::Secondary, TextAlign::Start));
    }
    list.popClip();

    list.add(theme.surface(area_, Shape::Soft, Elevation::Inset, 0.6f));
    const Clip *c = clip();
    if (!c) {
        list.add(theme.text("Select an audio clip to edit it", area_, TextStyle::Body, TextTone::Muted));
        return;
    }
    const audio::AudioData *data = hooks_.audioFor ? hooks_.audioFor(*c) : nullptr;
    list.pushClip(area_.inset(2.0f));
    const Rect r = clipRect(*c);
    list.add(theme.surface(r.inset(1.0f), Shape::Soft, Elevation::Raised, 0.6f, Finish::Convex));

    const double spb = project().secondsPerBeat();
    const float mid = area_.y + area_.h * 0.5f;
    const float half = area_.h * 0.44f;
    if (!data) {
        list.add(theme.text(c->hasAudio() ? "Loading..." : "Empty clip: record or import audio into it", r,
                            TextStyle::Caption, TextTone::Muted));
    } else {
        // The whole file, brighter inside the clip
        const double fileStart = c->start - c->offset / spb;
        const double fileEnd = fileStart + data->seconds() / spb;
        const float from = std::max(area_.x, beatX(fileStart));
        const float to = std::min(area_.x + area_.w, beatX(fileEnd));
        // Runs of columns that are all inside the clip or all outside it, one Range plot per run
        std::vector<float> values; // low, high pairs of the current run
        float runStart = from;
        bool runInside = false;
        auto flush = [&]() {
            if (values.empty())
                return;
            const uint32_t columns = static_cast<uint32_t>(values.size() / 2);
            const Rect rect{runStart, mid - half, 2.0f * static_cast<float>(columns), 2.0f * half};
            Plot plot = theme.inkPlot(rect, PlotMode::Range, runInside ? TextTone::Secondary : TextTone::Muted,
                                      runInside ? 0.8f : 0.25f);
            plot.values = values.data();
            plot.count = columns;
            plot.low = -1.0f;
            plot.high = 1.0f;
            plot.barFraction = 0.75f;
            list.add(plot);
            values.clear();
        };
        for (float x = from; x < to; x += 2.0f) {
            const double b0 = xBeat(x);
            const double b1 = xBeat(x + 2.0f);
            float low = 0.0f, high = 0.0f;
            data->range((b0 - fileStart) * spb, (b1 - fileStart) * spb, low, high);
            const bool inside = b0 >= c->start && b0 < c->start + c->length;
            const float g = inside ? c->gain : 1.0f;
            const float lo = std::max(-1.0f, std::min(1.0f, low * g));
            const float hi = std::max(-1.0f, std::min(1.0f, high * g));
            if (values.empty() || inside != runInside) {
                flush();
                runStart = x;
                runInside = inside;
            }
            values.push_back(lo);
            values.push_back(hi);
        }
        flush();
    }
    list.add(theme.ink({area_.x, mid, area_.w, 1.0f}, Shape::Rounded, TextTone::Muted, true, 0.3f));

    // Fades: a curve of dots, and a handle at the top
    auto fade = [&](double fromBeat, double beats, bool in) {
        const int steps = std::max(2, static_cast<int>(beats * pixelsPerBeat_ / 6.0));
        std::vector<Vec2> points;
        points.reserve(static_cast<size_t>(steps) + 1);
        for (int k = 0; k <= steps; ++k) {
            const double f = static_cast<double>(k) / steps;
            const double level = in ? std::sin(f * kHalfPi) : std::cos(f * kHalfPi);
            const float x = beatX(fromBeat + f * beats);
            const float y = r.y + 4.0f + static_cast<float>(1.0 - level) * (r.h - 8.0f);
            points.push_back({x, y});
        }
        list.addPolyline(points.data(), points.size(), theme.accentLine({}, {}, 1.5f, 0.8f));
    };
    if (c->fadeIn > 0.0)
        fade(c->start, c->fadeIn / spb, true);
    if (c->fadeOut > 0.0)
        fade(c->start + c->length - c->fadeOut / spb, c->fadeOut / spb, false);
    auto handle = [&](float x, bool active) {
        const Rect h{x - kHandle * 0.5f, r.y + kHandle * 0.5f, kHandle, kHandle};
        if (active)
            list.add(theme.accent(h, Shape::Circle));
        else
            list.add(theme.surface(h, Shape::Circle, Elevation::Raised, 0.6f, Finish::Convex));
    };
    handle(beatX(c->start + c->fadeIn / spb), hover_ == Drag::FadeIn || drag_ == Drag::FadeIn);
    handle(beatX(c->start + c->length - c->fadeOut / spb), hover_ == Drag::FadeOut || drag_ == Drag::FadeOut);
    // Edges, lit while grabbed
    if (hover_ == Drag::Start || drag_ == Drag::Start)
        list.add(theme.accent({r.x - 1.0f, r.y, 3.0f, r.h}, Shape::Pill));
    if (hover_ == Drag::End || drag_ == Drag::End)
        list.add(theme.accent({r.x + r.w - 2.0f, r.y, 3.0f, r.h}, Shape::Pill));
    list.popClip();

    // Playhead
    if (hooks_.position) {
        const float x = beatX(hooks_.position());
        if (x >= area_.x && x <= area_.x + area_.w)
            list.add(theme.accent({x - 1.0f, ruler_.y + ruler_.h - 6.0f, 2.0f, area_.y + area_.h - ruler_.y - ruler_.h + 6.0f},
                                  Shape::Rounded, 0.9f));
    }
}

}
