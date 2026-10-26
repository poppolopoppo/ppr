module;
#include "pP/Macros.h"

module game.application;
import :session_service;
import game.colony.matter;
import std;

namespace pP::game {
    PPR_DEFINE_LOG_CATEGORY(SessionDeliveryLog, info, none)

    // ------------------------------------------------------------------
    // activated-world presentation acknowledgment
    // ------------------------------------------------------------------

    bool hasSessionWorldPresentation(
        const RenderFrameResult &result, const u64 reservation_epoch, const GridWorldIdentity requested,
        const bool published, const bool activated,
        const std::optional<GridParticlePresentationIdentity> resident) noexcept {
        if (not published or not activated or requested.m_job_id == 0u or requested.m_world_generation == 0u) {
            return false;
        }

        if (resident and result.m_presented_particles != resident) {
            return false;
        }

        const auto &presented = result.m_presented_grid;
        return result.m_reservation_epoch == reservation_epoch and
               result.m_presented and
               not result.m_error and
               not result.m_dropped and
               presented.m_job_id == requested.m_job_id and
               presented.m_world_generation == requested.m_world_generation and
               presented.m_content_fingerprint == requested.m_content_fingerprint;
    }

    std::optional<GridParticleGpuView> selectWorkshopRetainedParticles(
        const std::optional<GridParticleGpuView> &held, const GridWorldIdentity game_world, const bool retain_content,
        const bool workshop_open, const bool preview_displayed, const bool finishing,
        const bool activation_pending) noexcept {
        if (not retain_content or
            not workshop_open or
            preview_displayed or
            finishing or
            activation_pending or
            not held or
            held->m_world != game_world) {
            return std::nullopt;
        }

        return held;
    }

    // ------------------------------------------------------------------
    // main-owner world transition
    // ------------------------------------------------------------------

    bool canSelectSessionWorldView(const SessionWorldTransition &transition) noexcept {
        return transition.m_phase != ESessionWorldPhase::activating and
               transition.m_phase != ESessionWorldPhase::attaching;
    }

    bool sessionTransitionOwnsDriverService(const SessionWorldTransition &transition) noexcept {
        return transition.m_phase == ESessionWorldPhase::resuming and not transition.m_returning;
    }

    Expected<ESessionWorldStep> advanceSessionWorldTransition(
        SessionWorldTransition &transition, colony::ColonyDriver *const current, colony::ColonyDriver *const candidate,
        colony::MatterRuntime &runtime, const SessionWorldTransitionInput &input) {
        using Phase = ESessionWorldPhase;
        using Step = ESessionWorldStep;

        if (transition.m_phase == Phase::detaching) {
            if (current and current->matterAttached()) {
                current->setPaused(true);
                const auto detached = current->advanceDetachMatter();
                PPR_RETURN_UNEXPECTED_ON_FAIL(SessionDeliveryLog, detached);
                if (not *detached) {
                    return Step::wait;
                }
            }

            transition.m_phase = Phase::preparing;
        }

        // Cancellation remains reversible only before activation admission.
        if (input.m_cancel and (transition.m_phase == Phase::preparing or transition.m_phase == Phase::prepared)) {
            transition.m_phase = Phase::cancelling;
        }

        if (transition.m_phase == Phase::preparing) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                SessionDeliveryLog, candidate ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));

            const auto prepared = candidate->advancePrepareMatter(runtime);
            if (not prepared) {
                // Drain even a failed preparation against its original owner.
                transition.m_error = prepared.error();
                transition.m_phase = Phase::cancelling;
                return Step::wait;
            }

            if (not *prepared) {
                return Step::wait;
            }

            transition.m_phase = Phase::prepared;
        }

        if (transition.m_phase == Phase::prepared) {
            if (input.m_activation_admitted) {
                transition.m_phase = Phase::activating;
            } else {
                return Step::activate;
            }
        }

        if (transition.m_phase == Phase::activating) {
            if (not input.m_activated) {
                return Step::wait;
            }

            transition.m_phase = Phase::attaching;
        }

        if (transition.m_phase == Phase::attaching) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                SessionDeliveryLog, candidate ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));

            candidate->setPaused(true);
            const auto attached = candidate->advanceAttachMatter(runtime);
            if (not attached and not candidate->matterAttached()) {
                // The renderer already switched: this is terminal, never rollback.
                PPR_RETURN_UNEXPECTED_ON_FAIL(SessionDeliveryLog, attached);
            }

            if (attached and not *attached) {
                return Step::wait;
            }

            if (not attached) {
                transition.m_error = attached.error();
            }

            candidate->bindParticlePresentation(input.m_world);
            const auto view = candidate->particleView();
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                SessionDeliveryLog, view ? std::error_code{} : std::make_error_code(std::errc::state_not_recoverable));

            transition.m_expected = GridParticlePresentationIdentity{
                input.m_world, view->m_publication, view->m_terrain_revision, view->m_joint_tick};
            transition.m_phase = Phase::awaiting_presentation;
            return Step::publish;
        }

        if (transition.m_phase == Phase::cancelling) {
            // A graphics rejection can arrive while the old snapshot prefix is
            // still pending. Finish that owner before cancellation/resumption.
            if (current and current->matterAttached()) {
                const auto detached = current->advanceDetachMatter();
                PPR_RETURN_UNEXPECTED_ON_FAIL(SessionDeliveryLog, detached);
                if (not *detached) {
                    return Step::wait;
                }
            }

            if (candidate) {
                const auto detached = candidate->advanceDetachMatter();
                if (not detached) {
                    auto first = transition.m_error;
                    PPR_RETAIN_ERROR_ON_FAIL(SessionDeliveryLog, first, detached.error());
                    PPR_RETURN_UNEXPECTED_ON_FAIL(SessionDeliveryLog, first);
                }

                // A pending acknowledgement still retains its exact owner.
                if (not *detached) {
                    return Step::wait;
                }
            }

            if (not input.m_renderer_idle) {
                return Step::wait;
            }

            transition.m_phase = Phase::resuming;
            return Step::release_candidate;
        }

        if (transition.m_phase == Phase::resuming) {
            if (current and not current->matterAttached() and current->matterState()) {
                const auto resumed = current->advanceResumeMatter(runtime);
                PPR_RETURN_UNEXPECTED_ON_FAIL(SessionDeliveryLog, resumed);
                if (not *resumed) {
                    return Step::wait;
                }
            }

            if (not transition.m_returning) {
                if (current) {
                    current->bindParticlePresentation(transition.m_previous.m_world);
                    current->setPaused(transition.m_previous.m_paused);
                }

                transition.m_phase = Phase::restored;
                return Step::restore;
            }

            PPR_RETURN_UNEXPECTED_ON_FAIL(
                SessionDeliveryLog, current ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));

            current->setPaused(true);
            current->clearPlayerCommand();
            current->bindParticlePresentation(input.m_world);
            const auto view = current->particleView();
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                SessionDeliveryLog, view ? std::error_code{} : std::make_error_code(std::errc::state_not_recoverable));

            transition.m_expected = GridParticlePresentationIdentity{
                input.m_world, view->m_publication, view->m_terrain_revision, view->m_joint_tick};
            transition.m_phase = Phase::awaiting_presentation;
            return Step::render_return;
        }

        if (transition.m_phase == Phase::awaiting_presentation and
            input.m_frame and
            transition.m_expected and
            hasSessionWorldPresentation(
                *input.m_frame, transition.m_reservation_epoch, transition.m_expected->m_world, true, true,
                transition.m_expected)) {
            if (current) {
                current->setPaused(transition.m_returning ? transition.m_previous.m_paused : false);
            }

            transition.m_phase = Phase::complete;
            return Step::finish;
        }

        return Step::wait;
    }

    // ------------------------------------------------------------------
    // displayed-world tool delivery
    // ------------------------------------------------------------------

    Expected<std::optional<sim::Entity>> applySessionBuildOrder(
        colony::ColonyDriver &driver, const SessionBuildOrder &order, const u64 displayed_world) {
        if (order.m_world != driver.worldGeneration() or order.m_world != displayed_world) {
            return std::optional<sim::Entity>{};
        }

        if (not driver.captureReady()) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(SessionDeliveryLog, std::errc::device_or_resource_busy);
        }

        Expected<sim::Entity> placed{std::unexpect, std::make_error_code(std::errc::invalid_argument)};
        if (order.m_tool == colony::EColonyTool::build_wall) {
            placed = driver.buildWall(order.m_footprint);
        } else if (order.m_tool == colony::EColonyTool::demolish) {
            const auto &rect = order.m_footprint;
            placed = driver.demolish(rect.m_min, rect.m_width, rect.m_height);
        }

        PPR_RETURN_UNEXPECTED_ON_FAIL(SessionDeliveryLog, placed);
        return std::optional{*placed};
    }

    Expected<bool> applySessionDigOrder(
        colony::ColonyDriver &driver, const SessionDigOrder &order, const u64 displayed_world) {
        if (order.m_world != driver.worldGeneration() or order.m_world != displayed_world) {
            return false;
        }

        if (not driver.captureReady()) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(SessionDeliveryLog, std::errc::device_or_resource_busy);
        }

        const auto &rect = order.m_rect;
        PPR_RETURN_UNEXPECTED_ON_FAIL(SessionDeliveryLog, driver.requestDig(rect.m_min, rect.m_width, rect.m_height));
        return true;
    }

    // ------------------------------------------------------------------
    // admission and owned inbox
    // ------------------------------------------------------------------

    std::error_code SessionService::post(SessionDelivery delivery) {
        const bool acknowledgment = std::holds_alternative<RenderFrameResult>(delivery) or
                                    std::holds_alternative<GridWorldResult>(delivery);
        if (not m_receiver_open or (not m_controls_open and not acknowledgment)) {
            PPR_RETURN_ERROR_ON_FAIL(SessionDeliveryLog, std::errc::operation_canceled);
        }

        m_pending.push_back(std::move(delivery));
        return {};
    }

    std::error_code SessionService::deferTool(SessionToolOrder order) {
        if (not m_controls_open or not m_receiver_open) {
            PPR_RETURN_ERROR_ON_FAIL(SessionDeliveryLog, std::errc::operation_canceled);
        }

        m_retries.push_back(std::move(order));
        return {};
    }

    void SessionService::requestClose() const noexcept {
        auto expected = ECloseState::idle;
        m_close.compare_exchange_strong(expected, ECloseState::pending, std::memory_order_release);
    }

    std::optional<SessionServiceBatch> SessionService::beginService() {
        if (m_active or not m_receiver_open) {
            return std::nullopt;
        }

        SessionServiceBatch batch{};
        batch.m_deliveries.swap(m_pending);
        auto retries = std::move(m_retries);
        m_retries = {};

        for (auto &order: retries) {
            std::visit([&](auto &value) { batch.m_deliveries.emplace_back(std::move(value)); }, order);
        }

        auto expected = ECloseState::pending;
        batch.m_close = m_close.compare_exchange_strong(expected, ECloseState::idle, std::memory_order_acq_rel);
        m_active = true;
        return batch;
    }

    bool SessionService::takeClose() noexcept {
        auto expected = ECloseState::pending;
        return m_close.compare_exchange_strong(expected, ECloseState::idle, std::memory_order_acq_rel);
    }

    bool SessionService::acknowledgmentsPending() const noexcept {
        return m_close.load(std::memory_order_acquire) == ECloseState::pending or
               std::ranges::any_of(m_pending, [](const SessionDelivery &delivery) {
                   return std::holds_alternative<RenderFrameResult>(delivery) or
                          std::holds_alternative<GridWorldResult>(delivery);
               });
    }

    Array<SessionDelivery> SessionService::takeAcknowledgments() {
        Array<SessionDelivery> results;
        auto current = m_pending.begin();
        while (current != m_pending.end()) {
            if (std::holds_alternative<RenderFrameResult>(*current) or
                std::holds_alternative<GridWorldResult>(*current)) {
                results.push_back(std::move(*current));
                current = m_pending.erase(current);
            } else {
                ++current;
            }
        }

        return results;
    }

    void SessionService::endService() noexcept {
        PPR_ASSERT(m_active);
        m_active = false;
    }

    // ------------------------------------------------------------------
    // terminal admission and queries
    // ------------------------------------------------------------------

    void SessionService::stopControls() {
        m_controls_open = false;
        m_close.store(ECloseState::stopped, std::memory_order_release);
        m_retries.clear();
        std::erase_if(m_pending, [](const SessionDelivery &delivery) {
            return not std::holds_alternative<RenderFrameResult>(delivery) and
                   not std::holds_alternative<GridWorldResult>(delivery);
        });
    }

    std::error_code SessionService::close() {
        if (m_active or not m_pending.empty() or not m_retries.empty()) {
            PPR_RETURN_ERROR_ON_FAIL(SessionDeliveryLog, std::errc::device_or_resource_busy);
        }

        m_controls_open = false;
        m_receiver_open = false;
        m_close.store(ECloseState::stopped, std::memory_order_release);
        return {};
    }

    bool SessionService::active() const noexcept {
        return m_active;
    }

    bool SessionService::pending() const noexcept {
        return not m_pending.empty() or m_close.load(std::memory_order_acquire) == ECloseState::pending;
    }

    bool SessionService::retriesPending() const noexcept {
        return not m_retries.empty();
    }
}
