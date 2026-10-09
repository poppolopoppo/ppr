#pragma once

namespace pP::game::tests {
    void failAllocationAfter(int successful_allocations) noexcept;
    void clearAllocationFailure() noexcept;

    /// Test-executable-only, thread-local standard allocation fault.
    struct AllocationFailure final {
        explicit AllocationFailure(const int successful_allocations = 0) noexcept {
            failAllocationAfter(successful_allocations);
        }
        ~AllocationFailure() noexcept {
            clearAllocationFailure();
        }
        AllocationFailure(const AllocationFailure &) = delete;
        AllocationFailure &operator=(const AllocationFailure &) = delete;
    };
}
