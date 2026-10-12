#pragma once
#include <istream>

#include "ir.h"

namespace ls {

// Parses the text IR described in DESIGN.md. Throws std::runtime_error on bad input
// (include the line number in the message).
Function parse_function(std::istream& in);

}  // namespace ls
