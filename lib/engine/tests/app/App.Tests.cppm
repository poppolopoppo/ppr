module;
#include "pP/UnitTest.h"

export module engine.tests.app;

import engine.core;

export namespace pP::tests {
    // MSVC C1001 workaround (same shape as engine.tests.core): an inline
    // constexpr `app` umbrella forces every consumer TU to deserialize its full
    // initializer from the IFC. Declared here and defined in App.Tests.cpp so
    // consumers only see a declaration. Test set, order, and paths are unchanged.
    extern const UnitTest app;
}
