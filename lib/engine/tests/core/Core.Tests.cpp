module engine.tests.core;

import engine.core;

namespace pP::tests {
    const UnitTest &enumsTests() noexcept;

    const UnitTest &mathTests() noexcept;

    const UnitTest &stringsTests() noexcept;

    const UnitTest &opaqueTests() noexcept;

    const UnitTest &channelTests() noexcept;

    const UnitTest &eventTests() noexcept;

    const UnitTest &contextTests() noexcept;

    const UnitTest &ioTests() noexcept;

    const UnitTest &file_watcherTests() noexcept;

    const UnitTest &halTests() noexcept;

    const UnitTest &utilityTests() noexcept;

    const UnitTest &serviceTests() noexcept;

    const UnitTest &page_poolTests() noexcept;

    const UnitTest &arenaTests() noexcept;

    const UnitTest &slabTests() noexcept;

    const UnitTest &allocatorTests() noexcept;

    const UnitTest &bufferTests() noexcept;

    const UnitTest &safe_ptr_testTests() noexcept;

    const UnitTest &poisoningTests() noexcept;

    const UnitTest &relocatableTests() noexcept;

    const UnitTest &hashTests() noexcept;

    const UnitTest &sortTests() noexcept;

    const UnitTest &bitmaskTests() noexcept;

    const UnitTest &pointersTests() noexcept;

    const UnitTest &iteratorsTests() noexcept;

    const UnitTest &stackTests() noexcept;

    const UnitTest &ring_bufferTests() noexcept;

    const UnitTest &stable_vectorTests() noexcept;

    const UnitTest &sparse_vectorTests() noexcept;

    const UnitTest &flat_mapTests() noexcept;

    const UnitTest &hash_mapTests() noexcept;

    const UnitTest memory = UnitTest::Named("memory") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            page_poolTests(),
            arenaTests(),
            slabTests(),
            allocatorTests(),
            bufferTests(),
            safe_ptr_testTests(),
            poisoningTests(),
        });
    };
    const UnitTest containers = UnitTest::Named("containers") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            relocatableTests(),
            hashTests(),
            sortTests(),
            bitmaskTests(),
            pointersTests(),
            iteratorsTests(),
            stackTests(),
            ring_bufferTests(),
            stable_vectorTests(),
            sparse_vectorTests(),
            flat_mapTests(),
            hash_mapTests(),
        });
    };
    // Defined here rather than as an inline constexpr umbrella in Core.Tests.cppm:
    // MSVC 14.51 ICEs (C1001 function-signature.cpp:213) when a consumer TU
    // deserializes this initializer from the IFC. Same test set and order as before.
    extern const UnitTest core = UnitTest::Named("core") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            enumsTests(),
            mathTests(),
            memory,
            stringsTests(),
            containers,
            opaqueTests(),
            channelTests(),
            eventTests(),
            contextTests(),
            ioTests(),
            file_watcherTests(),
            halTests(),
            utilityTests(),
            serviceTests(),
        });
    };
} // namespace pP::tests
