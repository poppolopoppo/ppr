module engine.tests.core;

import engine.core;

import :channel;
import :context;
import :enums;
import :event;
import :hal;
import :io;
import :io.file_watcher;
import :math;
import :opaque;
import :service;
import :strings;
import :utility;

namespace pP::tests {
// Defined here rather than as an inline constexpr umbrella in Core.Tests.cppm:
// MSVC 14.51 ICEs (C1001 function-signature.cpp:213) when a consumer TU
// deserializes this initializer from the IFC. Same test set and order as before.
const UnitTest core = UnitTest::Named("core") / [](UnitTest::IRun &_) -> void {
    _.recurse({
        enums,
        math,
        memory,
        strings,
        containers,
        opaque,
        channel,
        event,
        context,
        io,
        file_watcher,
        hal,
        utility,
        service,
    });
};
} // namespace pP::tests
