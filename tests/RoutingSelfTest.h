#pragma once

namespace tests {

// Routing (docs/routing-plan.md): the project's rules (daw/Routing.h), the file, undo, planRouting's
// stages, delays and solo, and the engine playing sends and groups (a hand-made audio::Routing with an
// impulse, and the bridge on a deviceless engine). Prints PASS/FAIL per check; returns the failure count.
int runRoutingChecks();

}
