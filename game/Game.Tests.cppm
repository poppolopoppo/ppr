module;
#include "pP/UnitTest.h"

export module game.tests;

import engine.core;

export namespace pP::game::tests {
    // MSVC C1001 workaround (same shape as engine.tests.core/app): the
    // umbrella is declared here and defined in Game.Tests.cpp so consumers
    // only see a declaration.
    extern const UnitTest game;
}
