#pragma once

#include <functional>
#include <string>

#include "daw/audio/AudioPool.h"
#include "daw/library/Library.h"
#include "daw/model/ProjectStore.h"
#include "daw/audio/RenderJob.h"
#include "ui/UI.h"
#include "audio/AudioSystem.h"
#include "env/Environment.h"
#include "plugin/Manager.h"

namespace daw {

// The DawApp services the exporter asks for: where a target without a folder of its own goes
// (beside the saved project, or empty for the library's Exports folder), the Exports panel to show,
// and the notice line
struct ExporterHooks {
    std::function<std::string()> outputFolder;
    std::function<void()> showExports;
    std::function<void(std::string)> notify;
};

// Project > Export: the export dialog (ExportDialog), and the render queue (ExportQueue) its presets
// are queued on. A render works on a snapshot of the project and an offline engine of its own, so
// the live app keeps playing and editing while it runs; this only starts renders and steps the
// queue once a frame.
class Exporter {
public:
    Exporter(env::Environment &env, ui::Context &gui, audio::AudioSystem &audio, plugin::PluginManager *plugins,
             InstrumentLibrary &library, AudioPool &pool, ProjectStore &store, ExporterHooks hooks);

    Exporter(const Exporter &) = delete;
    Exporter &operator=(const Exporter &) = delete;

    // Opens the export dialog (Project > Export)
    void openDialog();
    // Queues the preset at this index of the project as it is now, and shows the Exports panel
    void run(size_t preset);
    // Queues the project's "Mix" preset into the temp folder as "Vibin demo mix", without asking
    // (the --demo exportmix check)
    void runDemo();
    // Queues the project's "Release" preset into the temp folder under its own names, without
    // asking (the --demo exportrelease check)
    void runReleaseDemo();
    // The app was started with --silent: renders then use unity master gain, not the monitoring fader
    void setSilentRun(bool silent) { silentRun_ = silent; }
    // Once a frame: the render queue
    void update() { queue_.update(); }
    ExportQueue &queue() { return queue_; }

private:
    ui::Context &gui_;
    audio::AudioSystem &audio_;
    ProjectStore &store_;
    ExporterHooks hooks_;
    ExportQueue queue_;
    bool silentRun_ = false;

    // The master gain a render starts with (see setSilentRun)
    float renderGain() const;
    // Queues a preset of the project's with its targets in the temp folder (see runDemo)
    void runDemoPreset(const std::string &presetName, const std::string &fileName);
};

}
