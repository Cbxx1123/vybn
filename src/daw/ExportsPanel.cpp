#include "ExportsPanel.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <utility>

#include "Format.h"
#include "Widgets.h"
#include "engine/Shell.h"

namespace daw {

using namespace ui;

namespace {

// The loudness range the graph shows
constexpr double kGraphFloor = -60.0;
constexpr double kGraphCeiling = 0.0;

// Where a loudness falls on the graph, 0 at the floor and 1 at the ceiling
float graphPosition(double lufs)
{
    if (!std::isfinite(lufs))
        return 0.0f;
    const double t = (std::clamp(lufs, kGraphFloor, kGraphCeiling) - kGraphFloor) / (kGraphCeiling - kGraphFloor);
    return static_cast<float>(t);
}

// The short-term loudness over the file, a column per reading in an inset well, with a faint line at
// the loudness the target normalized to (when it did)
class LoudnessGraph : public Component {
public:
    LoudnessGraph(std::vector<float> series, double target, bool hasTarget)
        : series_(std::move(series)), target_(target), hasTarget_(hasTarget)
    {
    }

    Vec2 measure(const Theme &) const override { return {320.0f, 72.0f}; }

    void draw(DrawList &list, const Theme &theme) const override
    {
        list.add(theme.surface(rect_, Shape::Rounded, Elevation::Inset));
        const Rect plot = rect_.inset(8.0f);
        const float bottom = plot.y + plot.h;
        if (series_.empty())
            return;
        const float step = plot.w / static_cast<float>(series_.size());
        if (series_.size() < 2) {
            // A single reading is one column (a plot needs two points)
            const float x = plot.x;
            const float y = bottom - plot.h * graphPosition(series_[0]);
            const float width = std::max(2.0f, step - 1.0f);
            list.add(theme.accent({x, y, width, std::max(1.0f, bottom - y)}, Shape::Rounded, 0.13f));
            list.add(theme.accent({x, y - 1.0f, width, 2.0f}, Shape::Rounded, 0.9f));
        } else {
            std::vector<float> values(series_.size());
            for (size_t i = 0; i < series_.size(); ++i)
                values[i] = graphPosition(series_[i]);
            // From the first point's centre to the last's
            const Rect span{plot.x + step * 0.5f, plot.y, step * static_cast<float>(series_.size() - 1), plot.h};
            const auto count = static_cast<uint32_t>(values.size());

            // A soft fill from the floor to the curve, and the curve itself
            Plot fill = theme.accentPlot(span, PlotMode::Fill, 0.12f);
            fill.values = values.data();
            fill.count = count;
            fill.low = 0.0f;
            fill.high = 1.0f;
            fill.baseline = 0.0f;
            list.add(fill);

            Plot curve = theme.accentPlot(span, PlotMode::Line, 0.9f);
            curve.values = values.data();
            curve.count = count;
            curve.low = 0.0f;
            curve.high = 1.0f;
            curve.width = 1.5f;
            list.add(curve);
        }
        if (hasTarget_) {
            const float y = bottom - plot.h * graphPosition(target_);
            list.add(theme.inkLine({plot.x, y}, {plot.x + plot.w, y}, 1.0f, TextTone::Secondary, 0.45f));
        }
    }

private:
    std::vector<float> series_;
    double target_;
    bool hasTarget_;
};

// What a job is doing, for its card
const char *stateName(RenderJob::State state)
{
    switch (state) {
    case RenderJob::Preparing:
        return "Getting ready";
    case RenderJob::Rendering:
        return "Rendering";
    case RenderJob::Finalizing:
        return "Writing files";
    case RenderJob::Done:
        return "Done";
    case RenderJob::Failed:
        return "Failed";
    case RenderJob::Cancelled:
        return "Cancelled";
    }
    return "";
}

// A loudness or level as it is read, or "silent" for no signal (minus infinity)
std::string measureText(double value, const char *pattern)
{
    return std::isfinite(value) ? format(pattern, value) : std::string("silent");
}

std::string sizeText(uint64_t bytes)
{
    if (bytes < 1024 * 1024)
        return format("%.0f KB", static_cast<double>(bytes) / 1024.0);
    return format("%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
}

// One measurement: its value above, its name below
void stat(Grid &grid, const std::string &value, const char *caption, TextTone tone)
{
    Container &cell = grid.add<Container>(Direction::Column);
    cell.setAlign(Align::Start);
    cell.add<Label>(value, TextStyle::Body, tone);
    cell.add<Label>(caption, TextStyle::Caption, TextTone::Muted);
}

}

ExportsView::ExportsView(Context &context, ProjectStore &store, ExportQueue &queue, ExportsHooks hooks)
    : Container(Direction::Column), context_(context), store_(store), queue_(queue), hooks_(std::move(hooks))
{
    setAlign(Align::Stretch);
    auto &scroll = add<ScrollView>();
    scroll.setGrow(1.0f);
    cards_ = &scroll.content().add<Container>(Direction::Column);
    cards_->setAlign(Align::Stretch);
}

std::string ExportsView::signature()
{
    // Each job by its state: a card changes when a job starts, finishes or is taken away
    std::string s;
    for (RenderJob *job : queue_.jobs())
        s += std::to_string(job->id()) + ":" + std::to_string(static_cast<int>(job->state())) + ";";
    return s;
}

void ExportsView::handleInput(const Input &input)
{
    if (!rebuilding_ && signature() != built_) {
        rebuilding_ = true;
        std::weak_ptr<bool> alive = alive_;
        context_.post([this, alive] {
            if (alive.expired())
                return;
            built_ = signature();
            rebuild();
            rebuilding_ = false;
        });
    }
    for (auto &sync : syncs_)
        sync();
    Container::handleInput(input);
}

RenderJob *ExportsView::find(uint64_t id) const
{
    for (RenderJob *job : queue_.jobs()) {
        if (job->id() == id)
            return job;
    }
    return nullptr;
}

void ExportsView::rebuild()
{
    syncs_.clear();
    cards_->clear();
    const std::vector<RenderJob *> jobs = queue_.jobs();
    if (jobs.empty()) {
        cards_->add<Label>("Exports you run appear here", TextStyle::Subtitle, TextTone::Secondary);
        cards_->add<Label>("Press Export to render a preset of this project to files.", TextStyle::Caption, TextTone::Muted)
            .setWrapWidth(360.0f);
        cards_->add<Button>([this] {
                if (hooks_.openDialog)
                    hooks_.openDialog();
            })
            .setLabel("Export...")
            .setVariant(ButtonVariant::Accent)
            .setShape(Shape::Pill)
            .setSize(Size::Small)
            .setAlignSelf(Align::Start);
        return;
    }
    // Newest first
    for (auto it = jobs.rbegin(); it != jobs.rend(); ++it)
        buildJob(*cards_, (*it)->id());
}

void ExportsView::buildJob(Container &list, uint64_t id)
{
    RenderJob *job = find(id);
    if (!job)
        return;
    const bool finished = job->finished();
    const bool failed = job->state() == RenderJob::Failed || job->state() == RenderJob::Cancelled;
    const std::string presetName = job->presetName();

    Panel &card = list.add<Panel>(Direction::Column);
    card.setAlign(Align::Stretch);
    card.setDepth(Depth::Normal);

    // The head: the preset's name, its state and how long it has taken, and Cancel or Remove
    Container &head = card.add<Container>(Direction::Row);
    head.setAlign(Align::Center);
    head.add<Label>(presetName, TextStyle::Subtitle).setMaxWidth(260.0f);
    head.add<Spacer>();
    head.add<LiveLabel>(
        [this, id] {
            RenderJob *j = find(id);
            if (!j)
                return std::string();
            return std::string(stateName(j->state())) + "  -  " + format("%.0f s", j->elapsedSeconds());
        },
        TextStyle::Caption, TextTone::Muted);
    if (!finished) {
        head.add<Button>([this, id] {
                if (find(id))
                    queue_.cancel(id);
            })
            .setLabel("Cancel")
            .setVariant(ButtonVariant::Flat)
            .setSize(Size::Small);
    } else {
        ExportQueue *queue = &queue_;
        head.add<Button>([queue, id, this] { context_.post([queue, id] { queue->remove(id); }); })
            .setLabel("Remove")
            .setVariant(ButtonVariant::Flat)
            .setSize(Size::Small);
    }

    if (!finished) {
        // Still going: its progress, and what it is doing
        ProgressBar &bar = card.add<ProgressBar>(job->fraction());
        ProgressBar *raw = &bar;
        syncs_.push_back([this, id, raw] {
            if (RenderJob *j = find(id))
                raw->setValue(j->fraction());
        });
        card.add<LiveLabel>(
            [this, id] {
                RenderJob *j = find(id);
                return j ? j->status() : std::string();
            },
            TextStyle::Caption, TextTone::Muted)
            .setWrapWidth(420.0f);
        return;
    }

    // Finished: what it said about the job, then each file it wrote
    if (failed)
        card.add<Label>(job->status(), TextStyle::Caption, TextTone::Accent).setWrapWidth(420.0f);
    for (const TargetReport &report : job->reports())
        buildReport(card, *job, report);
}

void ExportsView::buildReport(Container &card, const RenderJob &job, const TargetReport &report)
{
    Panel &box = card.add<Panel>(Direction::Column, Elevation::Inset);
    box.setAlign(Align::Stretch);
    box.setDepth(Depth::Subtle);

    // The file, and Show in folder
    Container &head = box.add<Container>(Direction::Row);
    head.setAlign(Align::Center);
    const std::string file = report.path.empty() ? report.target : std::filesystem::path(report.path).filename().string();
    head.add<Label>(file, TextStyle::Body, report.ok ? TextTone::Primary : TextTone::Accent).setMaxWidth(360.0f);
    head.add<Spacer>();
    if (report.ok && !report.path.empty()) {
        const std::string path = report.path;
        head.add<Button>([path] { engine::showInFolder(path); })
            .setLabel("Show in folder")
            .setVariant(ButtonVariant::Flat)
            .setSize(Size::Small);
    }

    // The target it came from, its length and size
    std::string details = report.target;
    if (report.ok)
        details += "  -  " + format("%.1f s", report.seconds) + "  -  " + sizeText(report.bytes);
    box.add<Label>(details, TextStyle::Caption, TextTone::Muted).setMaxWidth(420.0f);
    if (!report.note.empty())
        box.add<Label>(report.note, TextStyle::Caption, TextTone::Muted).setWrapWidth(420.0f);
    if (!report.ok) {
        const std::string text = report.error.empty() ? std::string("Not written") : report.error;
        box.add<Label>(text, TextStyle::Caption, TextTone::Accent).setWrapWidth(420.0f);
        return;
    }
    if (report.midi) {
        box.add<Label>("A MIDI file: its notes and controllers, no audio figures", TextStyle::Caption, TextTone::Muted);
        return;
    }

    // The measurements of the file as written
    // Three to a row, so the six measurements show in two rows across the card
    Grid &stats = box.add<Grid>(3);
    stat(stats, measureText(report.integratedLufs, "%.1f LUFS"), "Integrated", TextTone::Primary);
    stat(stats, format("%.1f LU", report.loudnessRange), "Loudness range", TextTone::Primary);
    stat(stats, measureText(report.truePeakDb, "%.1f dBTP"), "True peak", TextTone::Primary);
    stat(stats, measureText(report.samplePeakDb, "%.1f dBFS"), "Sample peak", TextTone::Primary);
    stat(stats, format("%+.1f dB", report.gainDb), "Gain applied", TextTone::Primary);
    stat(stats, std::to_string(report.clipped), "Clipped samples", report.clipped > 0 ? TextTone::Accent : TextTone::Primary);

    // The short-term loudness over the file, against the target when it normalized to one
    if (!report.shortTerm.empty()) {
        box.add<Label>("Short-term loudness", TextStyle::Caption, TextTone::Muted);
        double targetLufs = 0.0;
        bool hasTarget = false;
        for (const ExportPreset &preset : store_.project().exportPresets) {
            if (preset.name != job.presetName())
                continue;
            for (const ExportTarget &target : preset.targets) {
                const bool named = report.target == target.name || report.target.rfind(target.name + " - ", 0) == 0;
                if (named && target.normalize == NormalizeMode::Loudness) {
                    targetLufs = target.loudnessLufs;
                    hasTarget = true;
                }
            }
        }
        box.add<LoudnessGraph>(report.shortTerm, targetLufs, hasTarget);
    }
}

}
