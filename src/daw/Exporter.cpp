#include "Exporter.h"

#include <algorithm>
#include <filesystem>

#include "ExportDialog.h"

namespace daw {

Exporter::Exporter(env::Environment &env, ui::Context &gui, audio::AudioSystem &audio, plugin::PluginManager *plugins,
                   InstrumentLibrary &library, AudioPool &pool, ProjectStore &store, ExporterHooks hooks)
    : gui_(gui), audio_(audio), store_(store), hooks_(std::move(hooks)), queue_(env, plugins, library, pool)
{
}

void Exporter::openDialog()
{
    openExportDialog(gui_, store_, ExportDialogHooks{[this](size_t preset) { run(preset); }});
}

// The master fader is for monitoring, so exports take its value; a --silent run has it at zero,
// which would make every export silent, so those render at unity
float Exporter::renderGain() const
{
    return silentRun_ ? 1.0f : audio_.masterGain().get();
}

void Exporter::run(size_t index)
{
    const std::vector<ExportPreset> &presets = store_.project().exportPresets;
    if (index >= presets.size())
        return;
    const ExportPreset &preset = presets[index];
    const bool anything = std::any_of(preset.targets.begin(), preset.targets.end(), [](const ExportTarget &t) { return t.enabled; });
    if (!anything) {
        if (hooks_.notify)
            hooks_.notify("\"" + preset.name + "\" has no targets turned on");
        return;
    }
    // The snapshot is a copy: the live project may change while this renders
    queue_.enqueue(store_.project(), preset, renderGain(), hooks_.outputFolder ? hooks_.outputFolder() : std::string());
    if (hooks_.showExports)
        hooks_.showExports();
}

void Exporter::runDemo()
{
    runDemoPreset("Mix", "Vibin demo mix");
}

void Exporter::runReleaseDemo()
{
    runDemoPreset("Release", "");
}

// A preset of the project's, with every target written into the temp folder (under `fileName` when
// given, else its own name template), queued without asking where. The project's presets stay as
// they are: only the snapshot's copy changes.
void Exporter::runDemoPreset(const std::string &presetName, const std::string &fileName)
{
    const std::vector<ExportPreset> &presets = store_.project().exportPresets;
    const auto found = std::find_if(presets.begin(), presets.end(), [&](const ExportPreset &p) { return p.name == presetName; });
    if (found == presets.end())
        return;
    ExportPreset preset = *found;
    const std::string temp = std::filesystem::temp_directory_path().string();
    for (ExportTarget &target : preset.targets) {
        target.folder = temp;
        if (!fileName.empty())
            target.fileName = fileName;
    }
    queue_.enqueue(store_.project(), preset, renderGain(), temp);
    if (hooks_.showExports)
        hooks_.showExports();
}

}
