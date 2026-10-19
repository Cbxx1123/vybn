// The miniaudio implementation, compiled once. The feature switches (MA_NO_ENGINE and so on, and
// the backend choice) are compile definitions in CMakeLists.txt, so every file that includes
// miniaudio.h sees the same configuration.

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>
