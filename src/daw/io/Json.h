#pragma once

#include <string>

#include "daw/io/Toml.h"

namespace daw::json {

// A JSON reader into the same Value tree as the TOML reader, for the listings online sources
// answer with. Objects become tables (keys in order), arrays arrays, numbers integers when they
// have no fraction or exponent and floats otherwise; null becomes an empty table, so lookups on it
// fall back to their defaults. Strings are UTF-8, with \uXXXX escapes (and surrogate pairs)
// decoded. On failure returns false with "at N: what" (N a byte offset) in error.
bool parse(const std::string &text, toml::Value &root, std::string &error);

}
