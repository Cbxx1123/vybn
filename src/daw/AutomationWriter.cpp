#include "AutomationWriter.h"

#include <algorithm>

namespace daw {

AutomationWriter::AutomationWriter(ProjectStore &store, std::function<double()> playhead, std::function<bool()> playing)
    : store_(store), playhead_(std::move(playhead)), playing_(std::move(playing))
{
}

bool AutomationWriter::armed(int track) const
{
    const Track *t = store_.project().track(track);
    return t && armed_.count(t->id) > 0;
}

void AutomationWriter::setArmed(int track, bool on)
{
    const Track *t = store_.project().track(track);
    if (!t)
        return;
    const ObjectId id = t->id;
    if (on) {
        armed_.insert(id);
        return;
    }
    armed_.erase(id);
    for (auto it = passes_.begin(); it != passes_.end();) {
        if (it->track == id) {
            finish(*it);
            it = passes_.erase(it);
        } else {
            ++it;
        }
    }
}

void AutomationWriter::setMasterArmed(bool on)
{
    if (on) {
        masterArmed_ = true;
        return;
    }
    masterArmed_ = false;
    for (auto it = passes_.begin(); it != passes_.end();) {
        if (it->master) {
            finish(*it);
            it = passes_.erase(it);
        } else {
            ++it;
        }
    }
}

audio::AutomationLane *AutomationWriter::laneFor(Pass &pass)
{
    if (pass.master) {
        if (!pass.place.own) {
            const audio::DeviceSetting *d = store_.project().device(pass.place.device);
            if (!d || d->key() != pass.deviceKey)
                return nullptr;
        }
        std::vector<audio::AutomationLane> *lanes = editLanesAt(store_, -1, pass.place);
        return lanes ? &audio::automationFor(*lanes, pass.param) : nullptr;
    }
    const int index = store_.project().trackIndex(pass.track);
    if (index < 0)
        return nullptr;
    AutomationPlace place = pass.place;
    if (!place.own) {
        place.device.track = index;
        const audio::DeviceSetting *d = store_.project().device(place.device);
        if (!d || d->key() != pass.deviceKey)
            return nullptr;
    }
    std::vector<audio::AutomationLane> *lanes = editLanesAt(store_, index, place);
    return lanes ? &audio::automationFor(*lanes, pass.param) : nullptr;
}

void AutomationWriter::finish(Pass &pass)
{
    if (audio::AutomationLane *lane = laneFor(pass))
        audio::finishAutomationWrite(*lane, pass.write);
}

bool AutomationWriter::touch(int track, const AutomationPlace &place, uint32_t param, const audio::ParamCurve &curve, double value)
{
    const bool master = place.master || (!place.own && place.device.master);
    const Track *t = master ? nullptr : store_.project().track(track);
    if (!playing_ || !playing_() || (master ? !masterArmed_ : !(t && armed_.count(t->id))))
        return false;
    std::string deviceKey;
    if (!place.own) {
        const audio::DeviceSetting *d = store_.project().device(place.device);
        if (!d || d->empty())
            return false;
        deviceKey = d->key();
    }
    const ObjectId id = t ? t->id : kNoId;
    const float position = static_cast<float>(curve.position(value));
    const double beat = playhead();
    auto same = [&](const Pass &p) {
        return p.master == master && p.track == id && p.param == param && p.place.own == place.own &&
               (place.own || (p.place.device.index == place.device.index && p.deviceKey == deviceKey));
    };
    auto found = std::find_if(passes_.begin(), passes_.end(), same);
    if (found != passes_.end()) {
        if (audio::AutomationLane *lane = laneFor(*found)) {
            audio::writeAutomation(*lane, found->write, beat, position);
            return true;
        }
        passes_.erase(found); // its device went: a new pass starts below
    }
    Pass pass;
    pass.master = master;
    pass.track = id;
    pass.place = place;
    pass.deviceKey = deviceKey;
    pass.param = param;
    audio::AutomationLane *lane = laneFor(pass);
    if (!lane)
        return false;
    pass.write = audio::beginAutomationWrite(*lane, beat, position, curve.stepped, kTolerance);
    passes_.push_back(std::move(pass));
    return true;
}

bool AutomationWriter::touchMaster(double value)
{
    return touch(-1, AutomationPlace::masterVolume(), audio::kVolumeParam, automationCurve(audio::kVolumeParam), value);
}

bool AutomationWriter::touchTrack(int track, uint32_t param, double value)
{
    return touch(track, AutomationPlace{}, param, automationCurve(param), value);
}

void AutomationWriter::update(bool held)
{
    if (passes_.empty())
        return;
    // Playback stopped, or (touching) the control let go: each pass ends, a let-go one gliding back
    const bool letGo = mode_ == Mode::Touch && !held;
    if (!playing_ || !playing_() || letGo) {
        for (Pass &pass : passes_) {
            if (letGo)
                pass.write.returnGap = kTouchReturn;
            finish(pass);
        }
        passes_.clear();
        return;
    }
    const double beat = playhead();
    for (auto it = passes_.begin(); it != passes_.end();) {
        // Only when the playhead moved, so a still frame edits nothing
        if (beat == it->write.to) {
            ++it;
            continue;
        }
        audio::AutomationLane *lane = laneFor(*it);
        if (!lane) {
            it = passes_.erase(it);
            continue;
        }
        audio::writeAutomation(*lane, it->write, beat, it->write.value);
        ++it;
    }
}

}
