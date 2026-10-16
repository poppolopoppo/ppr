// Shared Hal log category for all HAL backends.
//
// Included AFTER the module declaration and imports (never in the global
// fragment): requires pP/Macros.h from the includer's global fragment and
// the :logger partition plus std already imported.

#pragma once

#include "pP/Macros.h"

namespace pP::hal {
    PPR_DECLARE_LOG_CATEGORY(Hal)
}
