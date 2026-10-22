#pragma once

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "daw/model/AutomationLanes.h"
#include "daw/model/Project.h"
#include "daw/model/ProjectStore.h"
#include "audio/Automation.h"

namespace daw {

// Recording automation: while the song plays, moving a control of a track whose writing is armed
// (its volume fader, its pan knob, any knob of its devices) writes the control's value into the
// matching lane at the playhead instead of changing the project's own value. In Latch mode, from the
// first touch the lane keeps taking the last value at the playhead every frame, replacing the points
// it passes, until playback stops; in Touch mode only while the control is held (the mouse down),
// and once it is let go the lane glides back to its own curve over kTouchReturn. Either way each lane
// written is finished (its own value put back after the pass, the pass thinned; audio::AutomationWrite).
// While anything is being written the edit isn't settled, so the whole pass is one undo step.
class AutomationWriter {
public:
    static constexpr float kTolerance = 0.004f; // how far thinning may move the written curve
    static constexpr double kTouchReturn = 0.25; // beats a touch pass let go of takes to reach the lane again

    enum class Mode { Latch, Touch };
    Mode mode() const { return mode_; }
    void setMode(Mode mode) { mode_ = mode; }

    AutomationWriter(ProjectStore &store, std::function<double()> playhead, std::function<bool()> playing);

    // Whether a track's lanes take what its controls do while playing (kept by track id, not saved).
    // Disarming finishes what the track was writing.
    bool armed(int track) const;
    void setArmed(int track, bool on);
    // The same for the master: its volume and its effects' parameters (kept as one flag, not saved)
    bool masterArmed() const { return masterArmed_; }
    void setMasterArmed(bool on);

    // A control moved to `value` (in the parameter's own units: a device parameter's value, the
    // track's linear gain, the pan from -1 to 1). Written into its lane, and true, while the song
    // plays and the track is armed; false otherwise, and the caller sets the value as it always did.
    // For a master place (`place.master`, or a device place of the master) `track` is ignored and the
    // master's writing decides.
    bool touch(int track, const AutomationPlace &place, uint32_t param, const audio::ParamCurve &curve, double value);
    bool touchTrack(int track, uint32_t param, double value); // the track's own volume or pan
    bool touchMaster(double value);                           // the master's volume (linear gain)

    // Once a frame, before the history looks: the lanes being written follow the playhead with the
    // value they hold, and when playback stops they are finished. `held` is whether the mouse is down
    // (a control being moved): in Touch mode the passes are finished as soon as it isn't.
    void update(bool held = true);
    // Whether a pass is under way (the edit isn't settled while one is)
    bool writing() const { return !passes_.empty(); }
    double playhead() const { return playhead_ ? playhead_() : 0.0; }

private:
    struct Pass {
        bool master = false;     // the master's lane (and no track)
        ObjectId track = kNoId;
        AutomationPlace place;   // its device's index (the track's is found from the id each time)
        std::string deviceKey;   // the device written, so a pass stops when the place holds another
        uint32_t param = 0;
        audio::AutomationWrite write;
    };

    // The lane a pass writes, for editing, or null when its track or device has gone
    audio::AutomationLane *laneFor(Pass &pass);
    // Ends a pass (its lane's own value back after it, thinned)
    void finish(Pass &pass);

    ProjectStore &store_;
    std::function<double()> playhead_;
    std::function<bool()> playing_;
    std::set<ObjectId> armed_;
    bool masterArmed_ = false;
    Mode mode_ = Mode::Latch;
    std::vector<Pass> passes_;
};

}
