#pragma once

namespace tests {

// Freezing (docs/design/freeze-plan.md), the model's side: what the freeze hash follows, the file's
// round trip, the history's labels and undo. The renders are checked by the export suite.
int runFreezeProjectChecks();

}
