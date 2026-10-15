// Private implementation shared with the foundation's collision fixture.
// Included after imports; no public hash injection or alternate identity policy.
namespace pP::asset {
    namespace {
        // ------------------------------------------------------------------
        // canonical identity collision validation
        // ------------------------------------------------------------------

        template<typename Entry>
        [[nodiscard]] std::error_code validateIdentityCollisions(
            const std::span<const Entry> entries, const std::span<const std::size_t> key_order,
            pP::config::Diagnostics &diagnostics) {
            for (std::size_t index = 1u; index < key_order.size(); ++index) {
                const auto &previous = entries[key_order[index - 1u]];
                const auto &current = entries[key_order[index]];
                if (*previous.m_key != *current.m_key) {
                    continue;
                }

                const bool same_name = previous.m_name.m_pack == current.m_name.m_pack and
                                       previous.m_name.m_local == current.m_name.m_local;
                const auto message = same_name ? "Duplicate asset declaration: "
                                               : "Asset identity hash collision with ";
                PPR_RETURN_ERROR_ON_FAIL(
                    Asset,
                    pP::asset::reject(
                        diagnostics, current.m_descriptor.string(), current.m_name.string(),
                        std::string{message} + previous.m_name.string(), pP::config::EConfigErrc::duplicate_key));
            }

            return {};
        }
    }
}
