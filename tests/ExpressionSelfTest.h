#pragma once

namespace tests {

// Offline checks of notes' expression curves: their values, the note tools carrying them
// (slicing, joining, chopping, stretching, reversing), the history naming an edit of one, and the
// file writing, reading and tidying them. Prints PASS/FAIL per check; returns the number of failures.
int runExpressionChecks();

}
