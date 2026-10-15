module;

export module engine.sim:element;

import :worldgen.plan;
import engine.asset;
import engine.core;
import std;

export namespace pP::sim::asset {
    // ------------------------------------------------------------------
    // canonical physical element descriptor
    // ------------------------------------------------------------------

    struct Element {
        static constexpr config::EnumDomain<EGenerationMatter, 3> phase_domain{
            {config::EnumEntry{EGenerationMatter::solid, "solid"},
                config::EnumEntry{EGenerationMatter::liquid, "liquid"},
                config::EnumEntry{EGenerationMatter::gas, "gas"}}};
        static constexpr config::FloatDomain density_domain{0.0, (std::numeric_limits<double>::max)(), false, true};

        double m_density{};
        EGenerationMatter m_phase{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                config::field(
                    &Element::m_phase, pP::asset::metadata("phase"), phase_domain, config::EPresence::required),
                config::field(
                    &Element::m_density, pP::asset::metadata("density"), density_domain, config::EPresence::required)};
        }
    };
}
