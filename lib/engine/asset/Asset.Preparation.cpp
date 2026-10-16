module;
#include "pP/Macros.h"

module engine.asset;

import :preparation;
import :pack;
import engine.core;
import std;

namespace pP::asset::preparation_detail {
    // ------------------------------------------------------------------
    // exception priority and owned diagnostic publication
    // ------------------------------------------------------------------

    bool allocationFailure(const std::exception_ptr &exception) noexcept {
        if (not exception) {
            return false;
        }

        // Classification only; other exceptions are translated after drain.
        try {
            std::rethrow_exception(exception);
        } catch (const std::bad_alloc &) {
            return true;
        } catch (...) {
            return false;
        }
    }

    void retainException(std::exception_ptr &retained, std::exception_ptr exception) noexcept {
        if (not retained or (allocationFailure(exception) and not allocationFailure(retained))) {
            retained = std::move(exception);
        }
    }

    std::error_code exceptionError(const std::exception_ptr &exception) {
        if (not exception) {
            return {};
        }

        std::error_code error;
        try {
            std::rethrow_exception(exception);
        } catch (const std::bad_alloc &) {
            throw;
        } catch (const std::system_error &failure) {
            error = failure.code() ? failure.code() : std::make_error_code(std::errc::state_not_recoverable);
        } catch (...) {
            error = std::make_error_code(std::errc::state_not_recoverable);
        }

        PPR_RETURN_ERROR_ON_FAIL(Asset, error);
        return {};
    }

    void appendDiagnostics(config::Diagnostics &destination, const config::Diagnostics &source) {
        for (const auto &entry: source.entries()) {
            destination.add(
                entry.m_location, entry.m_property, entry.m_message, entry.m_code, entry.m_alias,
                entry.m_related_properties);
        }

        destination.addOmitted(source.omittedCount());
    }
}
