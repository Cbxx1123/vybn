#include "Routing.h"

#include <utility>

#include "Track.h"

namespace audio {

void Routing::beginStage()
{
    stageEnds.push_back(static_cast<uint32_t>(routes.size()));
}

void Routing::add(Route route)
{
    if (stageEnds.empty())
        beginStage();
    routes.push_back(std::move(route));
    stageEnds.back() = static_cast<uint32_t>(routes.size());
}

void Routing::finish()
{
    // Feeds point into the sources' send lists, which stay put from here on
    auto routeOf = [this](const TrackProcessor *track) -> Route * {
        for (Route &r : routes) {
            if (r.track == track)
                return &r;
        }
        return nullptr;
    };
    for (Route &r : routes)
        r.feeds.clear();
    for (Route &r : routes) {
        if (Route *to = r.output ? routeOf(r.output) : nullptr)
            to->feeds.push_back({r.track, nullptr});
        for (const RouteSend &send : r.sends) {
            if (Route *to = send.to ? routeOf(send.to) : nullptr)
                to->feeds.push_back({r.track, &send});
        }
    }
}

bool Routing::contains(const AudioProcessor *processor) const
{
    for (const Route &route : routes) {
        if (route.track == processor || route.output == processor)
            return true;
        for (const RouteSend &send : route.sends) {
            if (send.to == processor)
                return true;
        }
        for (const KeyFeed &key : route.keys) {
            if (key.from == processor)
                return true;
        }
    }
    for (const KeyFeed &key : masterKeys) {
        if (key.from == processor)
            return true;
    }
    return false;
}

}
