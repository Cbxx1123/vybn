#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ProjectStore.h"
#include "RenderJob.h"
#include "ui/UI.h"

namespace daw {

// What the Exports panel reaches through the app: the export dialog, for the empty state
struct ExportsHooks {
    std::function<void()> openDialog;
};

// The Exports panel: the render queue, newest first. A job that is still going is a card with its
// preset's name, a progress bar with what it is doing and how long it has taken, and Cancel. A
// finished one lists each file it wrote with its length, size and measurements (integrated
// loudness, loudness range, true and sample peak, the gain applied, clipped samples: a warning when
// some), a short-term loudness graph, and Show in folder; then Remove takes the job out of the
// list. The cards are rebuilt after the frame when a job changes state; the progress and status
// are kept in step each frame. The queue belongs to the Exporter, which updates it every frame.
class ExportsView : public ui::Container {
public:
    ExportsView(ui::Context &context, ProjectStore &store, ExportQueue &queue, ExportsHooks hooks);

    void handleInput(const ui::Input &input) override;

private:
    std::string signature();
    void rebuild();
    RenderJob *find(uint64_t id) const;
    void buildJob(ui::Container &list, uint64_t id);
    void buildReport(ui::Container &card, const RenderJob &job, const TargetReport &report);

    ui::Context &context_;
    ProjectStore &store_;
    ExportQueue &queue_;
    ExportsHooks hooks_;
    ui::Container *cards_ = nullptr;
    std::string built_ = "?";
    bool rebuilding_ = false;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true); // a post made before this goes is ignored
    std::vector<std::function<void()>> syncs_;                   // the progress bars, kept in step each frame
};

}
