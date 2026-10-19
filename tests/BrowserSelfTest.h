#pragma once

namespace tests {

// Offline checks of the asset browser's parts: the JSON reader, the online sources' listings
// (GitHub, Freesound, catalogues, sources.toml) read from canned answers, names, licences and
// categories, zip extraction (stored, deflated, damaged, unsafe paths), the SFZ loader's includes,
// defines, velocity layers, round robins, keyswitches and loops, and the library's categories,
// installs and moves in a temporary folder. No network. Prints PASS/FAIL per check; returns the
// process exit code.
int runBrowserSelfTest();

}
