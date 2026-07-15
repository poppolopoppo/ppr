import engine.core;
import engine.math;
import engine.rhi;
import engine.app;

import std;

#include "pP/Macros.h"

// ------------------------------------------------------------------
// tests
// ------------------------------------------------------------------

template <std::size_t A, std::size_t B>
struct same_size {
    static constexpr bool value() noexcept {
        static_assert(A == B);
        return A == B;
    }
};

template <typename T, std::size_t SizeBytes>
struct check_size : same_size<sizeof(T), SizeBytes> {

};

int main(int argc, char *argv[]) {
    using namespace pP;
    // std::println("Starting Encapsulated Video Game App...");

    // 1. Math check
    const auto perspective = float4x4::perspectiveD3D(4.0f, 3.0f, 0.1f, 1000.0f);
    //std::cout << "Math perspective matrix created successfully (Left-Handed, Z-to-1).\n";


    /*pP::hal::outputDebugFmt(TEXT("platform name: {}\n"), pP::hal::platformName());
    pP::hal::outputDebugFmt(TEXT("user name: {}\n"), pP::hal::userName());*/

    // hal::outputDebugFmt(TEXT("home dir: {}\n"), hal::homeDir().path().native());
    // hal::outputDebugFmt(TEXT("system dir: {}\n"), hal::systemDir().path());
    //
    // hal::outputDebugFmt(TEXT("app local dir: {}\n"), hal::appDataLocalDir());
    // hal::outputDebugFmt(TEXT("app roaming dir: {}\n"), hal::appDataRoamingDir().path().native());

    // std::println("app local dir: {}\n", hal::appDataLocalDir());

    Application app("ppr", std::span<const char * const>(&argv[0], argc));
    const int exit_code = app.run();

    float3 a{1, 2, 3};
    float3 b{4, 5, 6};
    [[maybe_unused]] auto v = (a + b) * .5f;
    [[maybe_unused]] auto n = normalize(v);
    [[maybe_unused]] auto d = distance(a, b);
    [[maybe_unused]] auto c = dot(a, b);
    [[maybe_unused]] auto d2 = sqrt(dot2(b - a));
//
// #ifdef _MSC_VER
//     std::println("MSVC version: {}", _MSC_VER);
//     std::println("Full version: {}", _MSC_FULL_VER);
//     std::println("Build: {}", _MSC_BUILD);
// #else
//     std::cout << "Not MSVC\n";
// #endif

    return exit_code;
}
