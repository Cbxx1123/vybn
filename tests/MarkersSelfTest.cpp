#include "MarkersSelfTest.h"
#include "Harness.h"

#include <string>

#include "daw/io/ProjectFile.h"
#include "daw/model/History.h"
#include "daw/model/ProjectSession.h"

namespace tests {

int runMarkersChecks()
{
    Checker c;

    daw::Project p;
    p.markers = {{0, 16.0, "Chorus"}, {0, 0.0, "Intro"}, {0, 8.0, "Verse"}};
    p.sortMarkers();
    c.check(p.markers[0].name == "Intro" && p.markers[2].name == "Chorus" && p.markerAt(9.0) == 1 && p.markerAt(8.0) == 1 &&
                p.markerAt(-1.0) == -1 && p.markerAfter(8.0) == 2 && p.markerAfter(16.0) == -1,
            "Markers: sorted, and found around a beat");

    // Ids from the clip counter, unique
    daw::ObjectId next = 1;
    p.normalizeIds(next);
    c.check(p.markers[0].id != daw::kNoId && p.markers[0].id != p.markers[1].id && p.markers[1].id != p.markers[2].id,
            "Markers: ids are given and unique");

    // The file: back exactly, sorted on reading; an older file has none
    const std::string text = daw::writeProject(p);
    daw::Project back;
    std::string error;
    const bool read = daw::readProject(text, back, error);
    c.check(read && back.markers.size() == 3 && back.markers[1].name == "Verse" && back.markers[1].beat == 8.0 &&
                back.markers[1].id == p.markers[1].id && daw::writeProject(back) == text && daw::writeProject(daw::Project()).find("markers") == std::string::npos,
            "Markers: they survive the file", error);
    daw::Project odd;
    const std::string shuffled = text.substr(0, text.find("name = ")) + "markers = [ { beat = 12, name = \"B\" }, { beat = -3, name = \"A\" } ]\n";
    c.check(daw::readProject(shuffled, odd, error) && odd.markers.size() == 2 && odd.markers[0].name == "A" && odd.markers[0].beat == 0.0,
            "Markers: a file's markers are sorted and kept on the timeline", error);

    // Undo names the edit and restores it
    daw::Project added = p;
    added.markers.push_back({0, 24.0, "Bridge"});
    daw::Project renamed = p;
    renamed.markers[1].name = "Verse 1";
    daw::Project moved = p;
    moved.markers[1].beat = 10.0;
    daw::Project gone = p;
    gone.markers.pop_back();
    c.check(daw::describeChange(p, added) == "Add marker" && daw::describeChange(p, renamed) == "Rename marker" &&
                daw::describeChange(p, moved) == "Move marker" && daw::describeChange(p, gone) == "Delete marker" && !daw::sameContent(p, moved),
            "Markers: undo names adding, renaming, moving and deleting");

    daw::ProjectSession session(p);
    session.recordHistory(true, true, 1.0);
    session.store().editMarkers().push_back({0, 32.0, "Outro"});
    session.recordHistory(true, true, 2.0);
    daw::HistoryRestore plan;
    const bool undone = session.history().undo(session.store(), 3.0, plan);
    const bool marked = plan.markers;
    session.restoreState(std::move(plan));
    c.check(undone && marked && session.project().markers.size() == 3, "Markers: an undo restores them through the store");
    return c.failures;
}

}
