#include "FreezeSelfTest.h"
#include "Harness.h"

#include <functional>
#include <string>

#include "audio/Effects.h"
#include "daw/io/ProjectFile.h"
#include "daw/model/Freeze.h"
#include "daw/model/History.h"
#include "daw/model/Project.h"
#include "daw/model/ProjectStore.h"

namespace tests {

namespace {

// An instrument track with a note and an effect, keyed by a second track
daw::Project frozenSong()
{
    daw::Project p;
    p.timing = audio::TempoMap(120.0, 4, 4);
    p.tracks.resize(2);
    daw::Track &lead = p.tracks[0];
    lead.name = "Lead";
    lead.kind = daw::TrackKind::Instrument;
    lead.clips.push_back(daw::Clip("Riff", 0.0, 4.0, {daw::Note{0.0, 1.0, 60, 0.8f}}));
    daw::Track &kick = p.tracks[1];
    kick.name = "Kick";
    kick.kind = daw::TrackKind::Instrument;
    kick.clips.push_back(daw::Clip("Beat", 0.0, 4.0, {daw::Note{0.0, 0.5, 36, 1.0f}}));
    daw::ObjectId next = 1;
    p.normalizeIds(next);
    audio::DeviceSetting comp = audio::effectSetting(audio::EffectType::Compressor);
    comp.sidechain = kick.id;
    lead.effects.push_back(comp);
    lead.freeze = daw::TrackFreeze{"C:/Songs/Song Audio/Lead freeze 1.wav", 4.25, daw::freezeHash(p, 0)};
    return p;
}

}

int runFreezeProjectChecks()
{
    Checker c;

    // What the hash follows: the track's sound and its key's, not its fader or name
    {
        const daw::Project p = frozenSong();
        auto stale = [&](const std::function<void(daw::Project &)> &change) {
            daw::Project q = p;
            change(q);
            return !daw::freezeCurrent(q, 0);
        };
        const bool follows = daw::freezeCurrent(p, 0) &&
                             stale([](daw::Project &q) { q.tracks[0].clips[0].notes[0].key = 62; }) &&
                             stale([](daw::Project &q) { q.tracks[0].effects[0].bypass = true; }) &&
                             stale([](daw::Project &q) { q.tracks[1].clips[0].notes[0].velocity = 0.5f; }) &&
                             stale([](daw::Project &q) { q.timing = audio::TempoMap(100.0, 4, 4); });
        const bool ignores = !stale([](daw::Project &q) { q.tracks[0].gain = 0.3f; }) &&
                             !stale([](daw::Project &q) { q.tracks[0].name = "Lead 2"; }) &&
                             !stale([](daw::Project &q) { q.tracks[0].clips[0].notes[0].selected = true; }) &&
                             !stale([](daw::Project &q) { q.tracks[0].mute = true; });
        c.check(follows && ignores, "Freeze: the hash follows the sound (and its key's), not the fader or the name");
    }

    // The file keeps a freeze; a bus's is dropped; a bad hash is refused
    {
        const daw::Project p = frozenSong();
        const std::string text = daw::writeProject(p);
        daw::Project back;
        std::string error;
        const bool read = daw::readProject(text, back, error);
        c.check(read && back.tracks[0].freeze == p.tracks[0].freeze && !back.tracks[1].frozen() && daw::freezeCurrent(back, 0),
                "File: a track's freeze round trip (still current)", error);
        std::string bad = text;
        const size_t at = bad.find("freeze_hash = \"");
        bad.replace(at + 15, 2, "zz");
        daw::Project refused;
        std::string why;
        c.check(!daw::readProject(bad, refused, why) && why.find("freeze_hash") != std::string::npos,
                "File: a freeze hash that isn't hex is refused", why);
        daw::Project thawed = p;
        thawed.tracks[0].freeze = {};
        c.check(!daw::sameSaved(p, thawed) && daw::writeProject(thawed).find("freeze") == std::string::npos,
                "File: an unfrozen track writes no freeze, and differs from a frozen one");
    }

    // The history names freezing, and undo puts a freeze back through the store
    {
        const daw::Project frozen = frozenSong();
        daw::Project live = frozen;
        live.tracks[0].freeze = {};
        daw::Project again = frozen;
        again.tracks[0].freeze.file = "C:/Songs/Song Audio/Lead freeze 2.wav";
        c.check(daw::describeChange(live, frozen) == "Freeze track" && daw::describeChange(frozen, live) == "Unfreeze track" &&
                    daw::describeChange(frozen, again) == "Refreeze track" && !daw::sameContent(live, frozen),
                "History: freezing is named", daw::describeChange(live, frozen));

        daw::ProjectStore store(live);
        daw::History history;
        history.update(store.project(), true, false, 0.0);
        store.editTrack(0).freeze = frozen.tracks[0].freeze;
        history.update(store.project(), true, false, 1.0);
        const std::string label = history.undoLabel();
        daw::HistoryRestore plan;
        const bool undone = history.undo(store, 2.0, plan) && (store.restore(std::move(plan)), !store.project().tracks[0].frozen());
        c.check(label == "Freeze track" && undone && store.project().tracks.size() == 2, "History: undo unfreezes", label);
    }

    return c.failures;
}

}
