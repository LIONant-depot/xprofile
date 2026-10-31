#pragma once
// The profiler's tab for an ImGui editor (imgui.h included before this): the switch (Enable), the Tracy viewer's state and a button
// that starts it, and a simple view of the recorder (xprofile::Snapshot) - the frame times of the last frames, the zones that took the
// most time in them, the plots. The full timeline (every thread, the GPU) is Tracy's viewer.
//      xprofile::editor::tab Tab;   ...   Tab.Draw();      (the host's drawer or window; once a frame while visible)
#include "../xprofile.h"
#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace xprofile::editor
{
    struct tab
    {
        void Draw(void) noexcept
        {
        #if !(defined(XLION_PROFILE) && XLION_PROFILE)
            ImGui::TextDisabled("The profiler is not compiled in (XLION_PROFILE off).");
        #else
            // the switch, the viewer
            bool bOn = isEnabled();
            if (ImGui::Checkbox("Record", &bOn)) { Enable(bOn); if (bOn) ClearRecorder(); }
            ImGui::SameLine();
            ImGui::TextDisabled(isConnected() ? "Tracy viewer connected" : bOn ? "no viewer connected" : "off - nothing is recorded");
            ImGui::SameLine();
            if (m_bLooked == false) { m_Viewer = ViewerPath(); m_bLooked = true; }
            ImGui::BeginDisabled(m_Viewer.empty());
            if (ImGui::Button("Open Tracy viewer"))
            {
                if (!isEnabled()) Enable(true);
                m_Message = LaunchViewer(m_Viewer.c_str()) ? "" : "the viewer did not start: " + m_Viewer;
            }
            ImGui::EndDisabled();
            if (m_Viewer.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Tracy's viewer is not built: dependencies/xprofile/build/tools/BuildTracyTools.bat (Linux: .sh) <tracy source> Build/tracy");
            if (!m_Message.empty()) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1), "%s", m_Message.c_str()); }
            if (!bOn) return;

            // the window and the refresh (the sums are made every few frames, not every frame)
            ImGui::SetNextItemWidth(160);
            ImGui::SliderInt("frames", &m_nFrames, 10, 1000);
            ImGui::SameLine(); ImGui::SetNextItemWidth(200);
            ImGui::InputTextWithHint("##filter", "filter zones", m_Filter, sizeof(m_Filter));
            if (m_Refresh-- <= 0) { Snapshot(m_nFrames, m_Snap); m_Refresh = 15; }

            // the frames
            const auto& F = m_Snap.m_FrameMs;
            if (!F.empty())
            {
                std::vector<float> S(F); std::sort(S.begin(), S.end());
                double Sum = 0; for (float v : F) Sum += v;
                const double Mean = Sum / F.size();
                char Overlay[128]; std::snprintf(Overlay, sizeof(Overlay), "frame: mean %.2f ms  p95 %.2f  max %.2f  (%d threads)", Mean, S[std::min<std::size_t>(S.size() - 1, S.size() * 95 / 100)], S.back(), m_Snap.m_nThreads);
                ImGui::PlotLines("##frames", F.data(), static_cast<int>(F.size()), 0, Overlay, 0.0f, static_cast<float>(S.back() * 1.1), ImVec2(-1, 70));
            }

            // the zones: per frame of the window
            const double nFrames = std::max<std::size_t>(F.size(), 1);
            if (ImGui::BeginTable("##zones", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable, ImVec2(0, 260)))
            {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("zone", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("ms / frame");
                ImGui::TableSetupColumn("calls / frame");
                ImGui::TableSetupColumn("mean ms");
                ImGui::TableSetupColumn("max ms");
                ImGui::TableHeadersRow();
                for (const auto& Z : m_Snap.m_Zones)
                {
                    const char* pName = Z.m_pWhere && Z.m_pWhere->name ? Z.m_pWhere->name : "?";
                    if (m_Filter[0] && !Contains(pName, m_Filter)) continue;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(pName);
                    if (ImGui::IsItemHovered() && Z.m_pWhere && Z.m_pWhere->file && Z.m_pWhere->file[0]) ImGui::SetTooltip("%s\n%s:%u", Z.m_pWhere->function, Z.m_pWhere->file, Z.m_pWhere->line);
                    ImGui::TableNextColumn(); ImGui::Text("%.3f", Z.m_TotalMs / nFrames);
                    ImGui::TableNextColumn(); ImGui::Text("%.1f", Z.m_Count / nFrames);
                    ImGui::TableNextColumn(); ImGui::Text("%.3f", Z.m_TotalMs / std::max<std::uint32_t>(Z.m_Count, 1));
                    ImGui::TableNextColumn(); ImGui::Text("%.3f", Z.m_MaxMs);
                }
                ImGui::EndTable();
            }

            // the plots: column 0 the buttons (Rel: scaled to the largest point shown; Max: 0 .. the theoretical maximum - the one the code
            // declared, XPROFILE_PLOT_LIMIT, editable here: hardware differs), column 1 the graph
            if (!m_Snap.m_Plots.empty() && ImGui::BeginTable("##plots", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV))
            {
                ImGui::TableSetupColumn("##buttons", ImGuiTableColumnFlags_WidthFixed, 150);
                ImGui::TableSetupColumn("##graph", ImGuiTableColumnFlags_WidthStretch);
                for (const auto& P : m_Snap.m_Plots)
                {
                    if (m_Filter[0] && !Contains(P.m_pName, m_Filter)) continue;
                    auto& U = m_PlotUI[P.m_pName];
                    if (U.m_Limit < 0) U.m_Limit = static_cast<float>(P.m_Limit);
                    ImGui::PushID(P.m_pName);           // (one ID per plot: their widgets have the same hidden labels)
                    ImGui::TableNextRow();

                    ImGui::TableNextColumn();
                    if (ImGui::Button(U.m_bMax ? "Max" : "Rel", ImVec2(44, 0))) U.m_bMax = !U.m_bMax;
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(U.m_bMax ? "Scaled to the theoretical maximum (click: to the largest point shown)" : "Scaled to the largest point shown (click: to the theoretical maximum)");
                    ImGui::SameLine(); ImGui::SetNextItemWidth(-1);
                    ImGui::DragFloat("##limit", &U.m_Limit, std::max(1.0f, U.m_Limit * 0.01f), 0, 0, U.m_Limit > 0 ? "max %.0f" : "max: none");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The theoretical maximum of this plot (declared by the code; drag to change it for this machine)");

                    ImGui::TableNextColumn();
                    const bool  bMax = U.m_bMax && U.m_Limit > 0;
                    const float Lo   = bMax ? 0.0f : static_cast<float>(P.m_Min);
                    const float Hi   = bMax ? U.m_Limit : static_cast<float>(P.m_Max * 1.05 + 1e-6);
                    char Overlay[200];
                    if (U.m_Limit > 0) std::snprintf(Overlay, sizeof(Overlay), "%s: %.2f  (%.2f .. %.2f)  %.0f%% of max", P.m_pName, P.m_Last, P.m_Min, P.m_Max, 100.0 * P.m_Last / U.m_Limit);
                    else               std::snprintf(Overlay, sizeof(Overlay), "%s: %.2f  (%.2f .. %.2f)", P.m_pName, P.m_Last, P.m_Min, P.m_Max);
                    ImGui::PlotLines("##plot", P.m_History.data(), static_cast<int>(P.m_History.size()), 0, Overlay, Lo, Hi, ImVec2(-1, 40));
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        #endif
        }

        static bool Contains(const char* pText, const char* pWhat) noexcept
        {
            const std::string T(pText), W(pWhat);
            return std::search(T.begin(), T.end(), W.begin(), W.end(), [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); }) != T.end();
        }

    #if defined(XLION_PROFILE) && XLION_PROFILE
        snapshot                m_Snap      {};
    #endif
        std::string             m_Viewer    {};
        bool                    m_bLooked   = false;
        std::string             m_Message   {};
        int                     m_nFrames   = 120;
        int                     m_Refresh   = 0;
        char                    m_Filter[64] {};
        struct plot_ui { bool m_bMax = false; float m_Limit = -1; };   // (-1: take the declared limit)
        std::map<std::string, plot_ui> m_PlotUI {};
    };
}
