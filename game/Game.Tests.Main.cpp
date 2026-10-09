#include "Game.TestAllocationFailure.h"
#include <cstdlib>
#include <new>

import std;
import engine.tests;
import game.tests;

namespace {
    thread_local int allocations_before_failure = -1;
}

void pP::game::tests::failAllocationAfter(const int successful_allocations) noexcept {
    allocations_before_failure = successful_allocations;
}

void pP::game::tests::clearAllocationFailure() noexcept {
    allocations_before_failure = -1;
}

void *operator new(const std::size_t size) {
    if (allocations_before_failure == 0) {
        allocations_before_failure = -1;
        throw std::bad_alloc{};
    }
    if (allocations_before_failure > 0) {
        --allocations_before_failure;
    }
    if (void *const allocation = std::malloc(size == 0u ? 1u : size)) {
        return allocation;
    }
    throw std::bad_alloc{};
}

void operator delete(void *const allocation) noexcept {
    std::free(allocation);
}
void operator delete(void *const allocation, std::size_t) noexcept {
    std::free(allocation);
}
void *operator new[](const std::size_t size) {
    return ::operator new(size);
}
void operator delete[](void *const allocation) noexcept {
    ::operator delete(allocation);
}
void operator delete[](void *const allocation, std::size_t) noexcept {
    ::operator delete(allocation);
}
int main(const int argc, char *argv[]) {
    return pP::tests::runTests(argc, argv, pP::game::tests::game, "game-tests");
}
