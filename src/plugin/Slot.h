#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Instance.h"

namespace plugin {

class PluginManager;

// Parameter values by id, sorted by id: what a project keeps for a plugin beside its state
using ParamValues = std::vector<std::pair<uint32_t, double>>;

// The value of `id` in `values`, or `fallback`
double valueOf(const ParamValues &values, uint32_t id, double fallback);
// Sets (or adds, in order) the value of `id`
void setValue(ParamValues &values, uint32_t id, double value);

// One place a plugin plays in the app (an effect in a chain, an instrument on a track), holding
// its instance and everything the UI shows of it. Loading, reloading and every control call run
// as jobs, one after another, so the main thread never waits for a plugin; a slot that goes away
// leaves its jobs to finish and its host to close in the background.
//
// What the project keeps is `state` (the plugin's own saved state, opaque) and `values`, the
// parameters set since: apply() makes the plugin match them. Values dropped from the list (an
// undo of the first change to a parameter) bring the state back first.
//
// Threads: everything is for the main thread but process() and live(), which are for the audio
// thread (one at a time). The instance reaches the audio thread through one atomic pointer; a
// replaced instance is let go only once the audio thread is no longer in it.
class Slot {
public:
    enum class State {
        Loading, // its host is starting (passing the sound through, for an effect)
        Ready,
        Failed,  // couldn't be loaded: message() says why
        Crashed, // its host ended: silent until reloaded
        Hung,    // it stopped answering and was stopped: silent until reloaded
    };

    // `name` is shown until the plugin is loaded (a project's saved name, say)
    Slot(PluginManager &manager, std::string id, std::string name);
    ~Slot();
    Slot(const Slot &) = delete;
    Slot &operator=(const Slot &) = delete;

    const std::string &id() const { return id_; }
    const std::string &name() const { return name_; }

    // The sample rate it runs at: loading starts with the first, a new one activates it again
    void setFormat(double sampleRate);
    // Makes the plugin match a project's values and state. Cheap when nothing changed.
    void apply(const ParamValues &values, const std::shared_ptr<const std::string> &state);
    // A new host with the same values and state: after a crash, or to pick up a rebuilt plugin
    void reload();
    // Another plugin in this place (an instrument track's sound changing), from its defaults or
    // `state`; an empty id leaves it empty
    void retarget(const std::string &id, const std::string &name);
    // Once a frame: takes what jobs finished, the watchdog's verdicts and the plugin's own changes
    void update();

    State state() const { return state_; }
    // Loading, or a job (activating at a new rate, loading a state) is under way or waiting
    bool busy() const;
    // A sentence for the UI when it isn't Ready: "Loading", "Couldn't load: ...", "Crashed (...)"
    std::string message() const;
    bool contained() const;

    // Its parameters, as the UI shows them (values follow the plugin's), and a count that goes
    // up when the list itself changes
    const std::vector<ParamInfo> &params() const { return params_; }
    uint64_t paramsVersion() const { return paramsVersion_; }
    const ParamInfo *param(uint32_t id) const;
    // A value as the plugin writes it ("-6.0 dB"); asked for in the background, so a number
    // until the plugin's words arrive
    std::string text(uint32_t id, double value);
    // The plugin's own changes to its parameters (from its interface or its processing) since
    // the last call, for the project. Read-only parameters (meters) aren't included.
    ParamValues takeChanges();
    // The plugin's state now, for saving: waits for it (main thread), up to `timeoutMs`. Null if
    // it has no state or couldn't say.
    std::shared_ptr<const std::string> captureState(uint32_t timeoutMs = 3000);
    // The plugin's own window: whether it has one, whether it is open, and opening or closing it
    // (in the background; a failure lands in editorError())
    bool hasEditor() const { return instance_ && (instance_->info().gui || instance_->info().embedded); }
    bool editorOpen() const { return instance_ && instance_->guiOpen(); }
    void toggleEditor();
    // Its interface drawn inside the app (when the plugin can be embedded): opened in the
    // background, then its pictures read and the app's mouse and keys sent to it
    bool canEmbedEditor() const { return instance_ && instance_->info().embedded; }
    void openEmbeddedEditor();
    void closeEditor();
    bool embeddedEditorOpen() const { return instance_ && instance_->guiEmbedded(); }
    bool editorOpening() const { return editorOpening_; }
    bool readEditorFrame(std::vector<uint8_t> &pixels, uint32_t &width, uint32_t &height, uint32_t &sequence);
    void sendEditorInput(const GuiInput &input);
    void resizeEditor(uint32_t width, uint32_t height);
    void placeEditor(int32_t x, int32_t y, bool shown, void *appWindow = nullptr);
    // Lets the plugin's host bring its window to the front (from the program in front)
    void allowEditorToFront();
    void clipEditor(int32_t x, int32_t y, int32_t width, int32_t height);
    const std::string &editorError() const { return editorError_; }
    // A parameter is being dragged in the plugin's window (so undo waits for it to finish)
    bool gesturing() const { return !gestures_.empty(); }
    uint32_t latency() const;
    uint32_t missed() const;
    // Rendering offline: the plugin is waited for (exports)
    void setOffline(bool offline);

    // Audio thread: whether an instance is in place, and processing through it (false, with
    // silent outputs, when it couldn't)
    bool live() const { return current_.load(std::memory_order_acquire) != nullptr; }
    bool process(const ProcessBlock &block);

private:
    struct Core;
    void enqueue(std::function<void(Core &)> op);
    void swapIn(std::shared_ptr<Instance> next);
    void startLoad();

    PluginManager &manager_;
    std::string id_;
    std::string name_;
    std::shared_ptr<Core> core_;

    // Main thread
    State state_ = State::Loading;
    std::string message_;
    double rate_ = 0.0;
    bool offline_ = false;
    uint64_t generation_ = 0; // of the latest load: an older one finishing late is let go
    std::shared_ptr<Instance> instance_;
    std::vector<ParamInfo> params_;
    uint64_t paramsVersion_ = 0;
    ParamValues applied_;                          // the values last applied
    std::shared_ptr<const std::string> appliedState_; // the state last applied (the project's)
    std::shared_ptr<const std::string> baseline_;  // the state values are on top of
    std::map<uint32_t, std::pair<double, std::string>> texts_;
    std::map<uint32_t, double> asked_; // texts asked for, by the value
    ParamValues changes_;
    std::vector<uint32_t> gestures_; // parameters held in the plugin's window
    std::string editorError_;
    bool editorOpening_ = false;

    // The audio thread's instance, and the one it is in now (a single-reader hazard pointer)
    std::atomic<Instance *> current_{nullptr};
    std::atomic<Instance *> using_{nullptr};
};

}
