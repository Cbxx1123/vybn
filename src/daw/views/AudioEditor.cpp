#include "daw/views/AudioEditor.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
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
    const audio::TempoMap &timing = project().timing;
    const float inX = beatX(timing.beatAt(timing.secondsAt(c.start) + c.fadeIn));
    const float outX = beatX(timing.beatAt(timing.secondsAt(c.start + c.length) - c.fadeOut));
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

    // Fades are in seconds, the clip's edges in beats: the tempo map turns one into the other
    const audio::TempoMap &timing = project().timing;
    const double beat = xBeat(input.mouse.x);
    const double lengthSeconds = timing.secondsAt(c->start + c->length) - timing.secondsAt(c->start);
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
            end = std::min(end, edited.start + project().beatsFor(edited.start, data->seconds() - edited.offset));
        edited.length = std::max(0.0625, end - edited.start);
        break;
    }
    case Drag::FadeIn:
        edited.fadeIn = std::clamp(timing.secondsAt(beat) - timing.secondsAt(edited.start), 0.0, std::max(0.0, lengthSeconds - edited.fadeOut));
        break;
    case Drag::FadeOut:
        edited.fadeOut = std::clamp(timing.secondsAt(edited.start + edited.length) - timing.secondsAt(beat), 0.0,
                                    std::max(0.0, lengthSeconds - edited.fadeIn));
        break;
    default:
        break;
    }
    const double editedSeconds = timing.secondsAt(edited.start + edited.length) - timing.secondsAt(edited.start);
    edited.fadeIn = std::min(edited.fadeIn, editedSeconds);
    edited.fadeOut = std::min(edited.fadeOut, editedSeconds - edited.fadeIn);
}

uint64_t AudioEditorView::contentKey(const Clip *c, const audio::AudioData *data) const
{
    Fingerprint fp;
    // The project (every edit moves revision) and the selection (which clip is shown)
    fp.add(store_.revision());
    fp.add(store_.selectionRevision());
    fp.add(project().selectedTrack);
    fp.add(project().selectedClip);
    // Layout and view
    fp.add(rect_);
    fp.add(ruler_);
    fp.add(area_);
    fp.add(pixelsPerBeat_);
    fp.add(viewStart_);
    // Hover and the drag in progress
    fp.add(hover_);
    fp.add(drag_);
    // The clip's sound arrives later (loading) and can be let go: its address
    fp.add(c != nullptr);
    fp.add(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(data)));
    return fp.value();
}

void AudioEditorView::draw(DrawList &list, const Theme &theme) const
{
    // Everything but the playhead is cached. A clip whose sound is still loading paints every frame
    // (its picture says "Loading..." until the sound arrives, and it is only a frame or two).
    const Clip *c = clip();
    const audio::AudioData *data = c && hooks_.audioFor ? hooks_.audioFor(*c) : nullptr;
    const bool loading = c && c->hasAudio() && !data;
    cache_.draw(list, theme, rect_, contentKey(c, data), [&] { drawContent(list, theme); }, !loading);

    // Playhead, every frame: it moves while playing. Nothing is drawn without a clip (as before).
    if (!c || !hooks_.position)
        return;
    const float x = beatX(hooks_.position());
    if (x >= area_.x && x <= area_.x + area_.w)
        list.add(theme.accent({x - 1.0f, ruler_.y + ruler_.h - 6.0f, 2.0f, area_.y + area_.h - ruler_.y - ruler_.h + 6.0f},
                              Shape::Rounded, 0.9f));
}

void AudioEditorView::drawContent(DrawList &list, const Theme &theme) const
{
    const audio::TempoMap &timing = project().timing;
    // Ruler: bars (of the signatures' lengths) and their units
    list.add(theme.surface(ruler_, Shape::Soft, Elevation::Raised, 0.35f));
    list.pushClip(ruler_);
    const double first = std::max(0.0, viewStart_);
    const double last = xBeat(ruler_.x + ruler_.w);
    const float barPixels = pixelsPerBeat_ * static_cast<float>(timing.signatureAt(first).barLength());
    int every = 1;
    while (barPixels * static_cast<float>(every) < 50.0f)
        every *= 2;
    timing.forEachBar(first, last + 1e-9, [&](int barIndex, double start, const audio::Signature &s) {
        const float unit = pixelsPerBeat_ * static_cast<float>(s.unit());
        for (int u = 0; u < s.numerator; ++u) {
            const bool bar = u == 0;
            if (!bar && unit < 10.0f)
                break;
            const float x = beatX(start + u * s.unit());
            list.add(theme.ink({x, ruler_.y + ruler_.h - (bar ? 10.0f : 5.0f), 1.0f, bar ? 8.0f : 3.0f}, Shape::Rounded,
                               TextTone::Muted, true, 0.8f));
        }
        if (barIndex % every == 0)
            list.add(theme.text(std::to_string(barIndex + 1), {beatX(start) + 4.0f, ruler_.y, 40.0f, ruler_.h - 6.0f},
                                TextStyle::Caption, TextTone::Secondary, TextAlign::Start));
    });
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

    const float mid = area_.y + area_.h * 0.5f;
    const float half = area_.h * 0.44f;
    if (!data) {
        list.add(theme.text(c->hasAudio() ? "Loading..." : "Empty clip: record or import audio into it", r,
                            TextStyle::Caption, TextTone::Muted));
    } else {
        // The whole file, brighter inside the clip
        const double fileSeconds = timing.secondsAt(c->start) - c->offset; // where the file starts in time
        const double fileStart = timing.beatAt(fileSeconds);
        const double fileEnd = timing.beatAt(fileSeconds + data->seconds());
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
            data->range(timing.secondsAt(b0) - fileSeconds, timing.secondsAt(b1) - fileSeconds, low, high);
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

    // Fades: a curve (a Line plot) from silence to full, and a handle at the top
    auto fade = [&](double fromBeat, double beats, bool in) {
        const int steps = std::max(2, static_cast<int>(beats * pixelsPerBeat_ / 6.0));
        // The levels at evenly spaced x (the first at the fade's start, the last at its end)
        std::vector<float> levels;
        levels.reserve(static_cast<size_t>(steps) + 1);
        float firstX = 0.0f, lastX = 0.0f;
        for (int k = 0; k <= steps; ++k) {
            const double f = static_cast<double>(k) / steps;
            const double level = in ? std::sin(f * kHalfPi) : std::cos(f * kHalfPi);
            const float x = beatX(fromBeat + f * beats);
            if (k == 0)
                firstX = x;
            lastX = x;
            levels.push_back(static_cast<float>(level));
        }
        if (levels.size() < 2)
            return;
        // Level 1 sits at the top inset (r.y + 4), level 0 at the bottom one (r.y + r.h - 4)
        Plot curve = theme.accentPlot({firstX, r.y + 4.0f, lastX - firstX, r.h - 8.0f}, PlotMode::Line, 0.8f);
        curve.values = levels.data();
        curve.count = static_cast<uint32_t>(levels.size());
        curve.low = 0.0f;
        curve.high = 1.0f;
        curve.width = 1.5f;
        list.add(curve);
    };
    if (c->fadeIn > 0.0)
        fade(c->start, project().beatsFor(c->start, c->fadeIn), true);
    if (c->fadeOut > 0.0) {
        const double end = c->start + c->length;
        const double outStart = timing.beatAt(timing.secondsAt(end) - c->fadeOut);
        fade(outStart, end - outStart, false);
    }
    auto handle = [&](float x, bool active) {
        const Rect h{x - kHandle * 0.5f, r.y + kHandle * 0.5f, kHandle, kHandle};
        if (active)
            list.add(theme.accent(h, Shape::Circle));
        else
            list.add(theme.surface(h, Shape::Circle, Elevation::Raised, 0.6f, Finish::Convex));
    };
    handle(beatX(c->start + project().beatsFor(c->start, c->fadeIn)), hover_ == Drag::FadeIn || drag_ == Drag::FadeIn);
    handle(beatX(timing.beatAt(timing.secondsAt(c->start + c->length) - c->fadeOut)), hover_ == Drag::FadeOut || drag_ == Drag::FadeOut);
    // Edges, lit while grabbed
    if (hover_ == Drag::Start || drag_ == Drag::Start)
        list.add(theme.accent({r.x - 1.0f, r.y, 3.0f, r.h}, Shape::Pill));
    if (hover_ == Drag::End || drag_ == Drag::End)
        list.add(theme.accent({r.x + r.w - 2.0f, r.y, 3.0f, r.h}, Shape::Pill));
    list.popClip();
}

}
