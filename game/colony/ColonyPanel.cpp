module;

module game.colony.panel;

import imgui;
import engine.sim;
import game.colony.agents;

namespace pP::colony {
    void drawColonyPanel(const ColonyDriver &driver, const ColonyPanelCounts counts) {
        ImGui::SetNextWindowSize(ImVec2{252.0f, 0.0f}, ImGuiCond_FirstUseEver);
        ImGui::Begin("Colony", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

        ImGui::Text("Seed  %llu", static_cast<unsigned long long>(driver.seed()));
        ImGui::Text("Tick  %llu", static_cast<unsigned long long>(driver.tickCount()));
        ImGui::Text("Simulated  %.1f ms", driver.simMs());
        ImGui::Separator();

        ImGui::TextColored(driver.paused()
                               ? ImVec4{1.0f, 0.72f, 0.34f, 1.0f}
                               : ImVec4{0.48f, 0.88f, 0.62f, 1.0f},
            "%s", driver.paused() ? "PAUSED" : "RUNNING");
        ImGui::SameLine();
        ImGui::Text("·  %ux", driver.speed());

        ImGui::Text("Chunks  %u / %u", driver.grid().residentChunkCount(), sim::kChunkCount);
        ImGui::Text("Tiles   %u visible", counts.m_visible_tiles);
        ImGui::Text("Chunks  %u visible", counts.m_visible_chunks);
        ImGui::Text("Paths   %u ok / %u partial / %u blocked", counts.m_paths, counts.m_partials, counts.m_blocked);
        ImGui::Text("Errands %u active", counts.m_errands);
        u32 agent = 0u;
        for (const AgentSummary &row: driver.agentSummaries()) {
            ImGui::Text("A%u %s pc %u/%u @ %.0f,%.0f", agent, row.m_has_plan ? "plan" : "idle", row.m_pc, row.m_replans,
                row.m_x, row.m_y);
            ++agent;
        }

        ImGui::Spacing();
        ImGui::TextDisabled("Space pause · N step · 1–3 speed · R new seed");
        ImGui::End();
    }
}
