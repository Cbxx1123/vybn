#pragma once

namespace tests {

// Offline checks of the resource managers: the lock-free MPMC queue, the real-time pool (every
// job run once, batches that follow each other, more threads than jobs), the scheduler's lanes,
// groups and pollers, tracks mixed in parallel matching a single thread, and the memory
// manager streaming a sample from its cache (the same audio as loaded whole, the cache read back
// on a second load, starving without the disk, evicting to stay in budget, and sources let go).
// Prints PASS/FAIL per check; returns the process exit code.
int runEngineSelfTest();

}
