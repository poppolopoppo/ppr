module;
#include "pP/UnitTest.h"

export module engine.tests.core;

import engine.core;

export namespace pP::tests {
    // MSVC 14.51 (VS18 Insiders) ICE workaround (C1001 function-signature.cpp:213):
    // a constexpr `core` umbrella forces every consumer TU to deserialize its full
    // initializer from the IFC, which crashes the frontend once the transitive suite
    // graph is large enough. Declared here and defined in Core.Tests.cpp so consumers
    // only see a declaration. Test set, order, and paths are unchanged.
    extern const UnitTest core;
}
