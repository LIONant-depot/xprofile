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

            if (ImGui::BeginTabBar("##profiler"))
            {
                const auto Pick = [&](const char* pName) { return m_Select == pName ? ImGuiTabItemFlags_SetSelected : 0; };
                if (ImGui::BeginTabItem("Metrics", nullptr, Pick("Metrics"))) { if (bOn) DrawMetrics(); else ImGui::TextDisabled("Record is off: no zones or plots."); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Memory", nullptr, Pick("Memory")))  { DrawMemory(); ImGui::EndTabItem(); }
                ImGui::EndTabBar();
                m_Select.clear();
            }
        #endif
        }

    #if defined(XLION_PROFILE) && XLION_PROFILE
        void DrawMetrics(void) noexcept
        {
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

            // the plots: column 0 the buttons only (Rel: scaled to the largest point shown; Max: 0 .. the theoretical maximum the code
            // declared, XPROFILE_PLOT_LIMIT), column 1 the graph (its overlay: the values, the maximum, the share of it)
            if (!m_Snap.m_Plots.empty() && ImGui::BeginTable("##plots", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV))
            {
                ImGui::TableSetupColumn("##buttons", ImGuiTableColumnFlags_WidthFixed, 52);
                ImGui::TableSetupColumn("##graph", ImGuiTableColumnFlags_WidthStretch);
                for (const auto& P : m_Snap.m_Plots)
                {
                    if (m_Filter[0] && !Contains(P.m_pName, m_Filter)) continue;
                    auto& U = m_PlotUI[P.m_pName];
                    U.m_Limit = static_cast<float>(P.m_Limit);
                    ImGui::PushID(P.m_pName);           // (one ID per plot: their widgets have the same hidden labels)
                    ImGui::TableNextRow();

                    ImGui::TableNextColumn();
                    if (ImGui::Button(U.m_bMax ? "Max" : "Rel", ImVec2(44, 0))) U.m_bMax = !U.m_bMax;
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(U.m_Limit > 0 ? (U.m_bMax ? "Scaled to the theoretical maximum (click: to the largest point shown)" : "Scaled to the largest point shown (click: to the theoretical maximum)") : "Scaled to the largest point shown (this plot declares no maximum)");

                    ImGui::TableNextColumn();
                    const bool  bMax = U.m_bMax && U.m_Limit > 0;
                    const float Lo   = bMax ? 0.0f : static_cast<float>(P.m_Min);
                    const float Hi   = bMax ? U.m_Limit : static_cast<float>(P.m_Max * 1.05 + 1e-6);
                    char Overlay[200];
                    if (U.m_Limit > 0) std::snprintf(Overlay, sizeof(Overlay), "%s: %.2f  (%.2f .. %.2f)  max %.0f: %.0f%%", P.m_pName, P.m_Last, P.m_Min, P.m_Max, U.m_Limit, 100.0 * P.m_Last / U.m_Limit);
                    else               std::snprintf(Overlay, sizeof(Overlay), "%s: %.2f  (%.2f .. %.2f)", P.m_pName, P.m_Last, P.m_Min, P.m_Max);
                    ImGui::PlotLines("##plot", P.m_History.data(), static_cast<int>(P.m_History.size()), 0, Overlay, Lo, Hi, ImVec2(-1, 40));
                    if (ImGui::IsItemHovered())             // (the numbers and the share of the maximum together)
                    {
                        if (U.m_Limit > 0) ImGui::SetTooltip("%s\nnow %.2f of %.2f max: %.1f%%\nrange shown %.2f .. %.2f", P.m_pName, P.m_Last, U.m_Limit, 100.0 * P.m_Last / U.m_Limit, P.m_Min, P.m_Max);
                        else               ImGui::SetTooltip("%s\nnow %.2f\nrange shown %.2f .. %.2f (no maximum declared)", P.m_pName, P.m_Last, P.m_Min, P.m_Max);
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        }

        // The memory pools (always kept, Record on or off): per pool the totals, then its map - a bar per region (a VRAM block: 64 MB; a
        // large resource's own allocation), each allocation in its tag's colour, free space dark; hovering tells what is there
        void DrawMemory(void) noexcept
        {
            if (m_MemRefresh-- <= 0) { MemSnapshot(m_Mem); m_MemRefresh = 15; }
            if (m_Mem.empty()) { ImGui::TextDisabled("No memory pool reported yet."); return; }
            constexpr double MB = 1.0 / (1024.0 * 1024.0);
            for (const auto& P : m_Mem)
            {
                std::uint64_t Free = 0, Largest = 0;
                for (const auto& R : P.m_Regions) { Free += R.m_Size - std::min(R.m_Used, R.m_Size); Largest = std::max(Largest, R.m_LargestFree); }
                char Head[256];
                std::snprintf(Head, sizeof(Head), "%s: %.1f MB used in %u allocations, %.1f MB reserved in %zu regions (peak %.1f MB); free inside them %.1f MB, largest hole %.1f MB###%s",
                    P.m_pName, P.m_Used * MB, P.m_nAllocs, P.m_Reserved * MB, P.m_Regions.size(), P.m_Peak * MB, Free * MB, Largest * MB, P.m_pName);
                if (!ImGui::CollapsingHeader(Head, ImGuiTreeNodeFlags_DefaultOpen)) continue;

                // the legend: one colour per tag
                std::map<std::string, int> Tags;
                for (const auto& R : P.m_Regions) for (const auto& A : R.m_Allocs) Tags.emplace(A.m_pTag ? A.m_pTag : "?", 0);
                for (const auto& [Tag, n] : Tags)
                {
                    ImGui::ColorButton(("##" + Tag).c_str(), ImGui::ColorConvertU32ToFloat4(TagColor(Tag.c_str())), ImGuiColorEditFlags_NoTooltip, ImVec2(12, 12));
                    ImGui::SameLine(); ImGui::TextUnformatted(Tag.c_str()); ImGui::SameLine(0, 18);
                }
                ImGui::NewLine();

                ImGui::PushID(P.m_pName);
                if (ImGui::BeginChild("##map", ImVec2(0, std::min(24.0f + 16.0f * P.m_Regions.size(), 320.0f)), ImGuiChildFlags_Borders))
                {
                    for (const auto& R : P.m_Regions)
                    {
                        const std::uint64_t RFree = R.m_Size - std::min(R.m_Used, R.m_Size);
                        // fragmentation: how much of the free space is outside the largest hole
                        const double Frag = RFree ? 100.0 * (1.0 - double(R.m_LargestFree) / double(RFree)) : 0.0;
                        ImGui::Text("%6.1f MB %3.0f%% used", R.m_Size * MB, R.m_Size ? 100.0 * R.m_Used / R.m_Size : 0.0);
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("region %llu: %.2f MB, %zu allocations, %.2f MB free, largest hole %.2f MB, fragmentation %.0f%%",
                            static_cast<unsigned long long>(R.m_Id), R.m_Size * MB, R.m_Allocs.size(), RFree * MB, R.m_LargestFree * MB, Frag);
                        ImGui::SameLine(150);
                        const ImVec2 P0 = ImGui::GetCursorScreenPos();
                        const float  W  = std::max(ImGui::GetContentRegionAvail().x, 50.0f), H = 12.0f;
                        ImGui::InvisibleButton("##bar", ImVec2(W, H));
                        auto* pDraw = ImGui::GetWindowDrawList();
                        pDraw->AddRectFilled(P0, ImVec2(P0.x + W, P0.y + H), IM_COL32(40, 40, 46, 255));
                        const double Scale = R.m_Size ? W / double(R.m_Size) : 0.0;
                        for (const auto& A : R.m_Allocs)
                        {
                            const float X0 = P0.x + float(A.m_Offset * Scale), X1 = std::max(X0 + 1.0f, P0.x + float((A.m_Offset + A.m_Size) * Scale));
                            pDraw->AddRectFilled(ImVec2(X0, P0.y), ImVec2(X1, P0.y + H), TagColor(A.m_pTag ? A.m_pTag : "?"));
                        }
                        if (ImGui::IsItemHovered() && Scale > 0)
                        {
                            const std::uint64_t At = static_cast<std::uint64_t>((ImGui::GetIO().MousePos.x - P0.x) / Scale);
                            const mem_alloc* pHit = nullptr; std::uint64_t HoleLo = 0, HoleHi = R.m_Size;
                            for (const auto& A : R.m_Allocs)
                            {
                                if (At >= A.m_Offset && At < A.m_Offset + A.m_Size) { pHit = &A; break; }
                                if (A.m_Offset + A.m_Size <= At) HoleLo = std::max(HoleLo, A.m_Offset + A.m_Size);
                                else if (A.m_Offset > At) { HoleHi = std::min(HoleHi, A.m_Offset); }
                            }
                            if (pHit) ImGui::SetTooltip("%s\noffset %.2f MB, %.3f MB", pHit->m_pTag ? pHit->m_pTag : "?", pHit->m_Offset * MB, pHit->m_Size * MB);
                            else      ImGui::SetTooltip("free: %.3f MB (offset %.2f .. %.2f MB)", (HoleHi - HoleLo) * MB, HoleLo * MB, HoleHi * MB);
                        }
                    }
                }
                ImGui::EndChild();
                ImGui::PopID();
            }
        }

        static ImU32 TagColor(const char* pTag) noexcept
        {
            static const ImU32 Palette[] = { IM_COL32(86, 156, 214, 255), IM_COL32(206, 145, 87, 255), IM_COL32(106, 190, 120, 255), IM_COL32(197, 112, 192, 255),
                                             IM_COL32(220, 200, 90, 255), IM_COL32(90, 200, 200, 255), IM_COL32(230, 100, 100, 255), IM_COL32(160, 160, 230, 255) };
            std::size_t h = 0; for (const char* p = pTag; *p; ++p) h = h * 31 + static_cast<unsigned char>(*p);
            return Palette[h % (sizeof(Palette) / sizeof(Palette[0]))];
        }
    #endif

        static bool Contains(const char* pText, const char* pWhat) noexcept
        {
            const std::string T(pText), W(pWhat);
            return std::search(T.begin(), T.end(), W.begin(), W.end(), [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); }) != T.end();
        }

    #if defined(XLION_PROFILE) && XLION_PROFILE
        snapshot                m_Snap      {};
        std::vector<mem_pool>   m_Mem       {};
        int                     m_MemRefresh = 0;
    #endif
        std::string             m_Select    {};             // a sub-tab to bring forward on the next Draw ("Metrics", "Memory": Profiler -Show)
        std::string             m_Viewer    {};
        bool                    m_bLooked   = false;
        std::string             m_Message   {};
        int                     m_nFrames   = 120;
        int                     m_Refresh   = 0;
        char                    m_Filter[64] {};
        struct plot_ui { bool m_bMax = false; float m_Limit = 0; };    // (the declared limit, from the snapshot)
        std::map<std::string, plot_ui> m_PlotUI {};
    };
}
