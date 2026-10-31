#pragma once
// The profiler's tab for an ImGui editor (imgui.h included before this): the switch (Enable), the Tracy viewer's state and a button
// that starts it, and a simple view of the recorder (xprofile::Snapshot) - the frame times of the last frames, the zones that took the
// most time in them, the plots. The full timeline (every thread, the GPU) is Tracy's viewer.
//      xprofile::editor::tab Tab;   ...   Tab.Draw();      (the host's drawer or window; once a frame while visible)
#include "../xprofile.h"
#include <algorithm>
#include <cmath>
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

        // The memory pools (always kept, Record on or off) as an icicle chart: columns left to right - all of it, each pool, each region
        // (a VRAM block of 64 MB, a large resource's own allocation), what is in it (allocations in their tag's colour, holes grey); the
        // height is the bytes. Wheel: zoom around the mouse; right button: pan; Fit: all of it again. Hover: what it is.
        void DrawMemory(void) noexcept
        {
            if (m_MemRefresh-- <= 0) { MemSnapshot(m_Mem); m_MemRefresh = 15; }
            if (m_Mem.empty()) { ImGui::TextDisabled("No memory pool reported yet."); return; }
            constexpr double MB = 1.0 / (1024.0 * 1024.0);

            // the totals and the legend
            std::uint64_t Total = 0;
            std::map<std::string, int> Tags;
            for (const auto& P : m_Mem)
            {
                std::uint64_t Free = 0, Largest = 0;
                for (const auto& R : P.m_Regions) { Free += R.m_Size - std::min(R.m_Used, R.m_Size); Largest = std::max(Largest, R.m_LargestFree); for (const auto& A : R.m_Allocs) Tags.emplace(A.m_pTag ? A.m_pTag : "?", 0); }
                ImGui::Text("%s: %.1f MB used in %u allocations, %.1f MB reserved in %zu regions (peak %.1f MB); free inside them %.1f MB, largest hole %.1f MB",
                    P.m_pName, P.m_Used * MB, P.m_nAllocs, P.m_Reserved * MB, P.m_Regions.size(), P.m_Peak * MB, Free * MB, Largest * MB);
                Total += P.m_Reserved;
            }
            if (ImGui::Button("Fit")) { m_MemZoom = 1; m_MemTop = 0; }
            ImGui::SameLine(); ImGui::TextDisabled("(wheel: zoom, right button: pan)");
            for (const auto& [Tag, n] : Tags)
            {
                ImGui::SameLine(0, 18);
                ImGui::ColorButton(("##" + Tag).c_str(), ImGui::ColorConvertU32ToFloat4(TagColor(Tag.c_str())), ImGuiColorEditFlags_NoTooltip, ImVec2(12, 12));
                ImGui::SameLine(); ImGui::TextUnformatted(Tag.c_str());
            }
            if (Total == 0) return;

            // the canvas and its view: [m_MemTop, m_MemTop + 1 / m_MemZoom] of the whole (0..1) is shown
            const ImVec2 C0 = ImGui::GetCursorScreenPos();
            const ImVec2 Size(std::max(ImGui::GetContentRegionAvail().x, 200.0f), std::max(ImGui::GetContentRegionAvail().y, 240.0f));
            ImGui::InvisibleButton("##memory", Size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
            const bool bHover = ImGui::IsItemHovered();
            auto& IO = ImGui::GetIO();
            if (bHover && IO.MouseWheel != 0)
            {
                const double At = m_MemTop + (IO.MousePos.y - C0.y) / Size.y / m_MemZoom;          // the place under the mouse stays there
                m_MemZoom = std::clamp(m_MemZoom * std::pow(1.25, IO.MouseWheel), 1.0, 1e6);
                m_MemTop  = At - (IO.MousePos.y - C0.y) / Size.y / m_MemZoom;
            }
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0)) m_MemTop -= IO.MouseDelta.y / Size.y / m_MemZoom;
            m_MemTop = std::clamp(m_MemTop, 0.0, 1.0 - 1.0 / m_MemZoom);

            auto* pDraw = ImGui::GetWindowDrawList();
            pDraw->PushClipRect(C0, ImVec2(C0.x + Size.x, C0.y + Size.y), true);
            pDraw->AddRectFilled(C0, ImVec2(C0.x + Size.x, C0.y + Size.y), IM_COL32(30, 30, 34, 255));
            const float  ColW = Size.x / 4;
            const double Px   = Size.y * m_MemZoom / double(Total);             // pixels per byte
            const double Top  = m_MemTop * double(Total);                       // the first byte shown
            std::string  Hover;
            // one box: its bytes [B0, B1) of the whole, in column c
            const auto Box = [&](int c, double B0, double B1, ImU32 Col, const std::string& Label, const std::string& Tip)
            {
                const float Y0 = C0.y + float((B0 - Top) * Px), Y1 = C0.y + float((B1 - Top) * Px);
                if (Y1 < C0.y || Y0 > C0.y + Size.y) return;
                const float X0 = C0.x + c * ColW, X1 = X0 + ColW - 1;
                pDraw->AddRectFilled(ImVec2(X0, Y0), ImVec2(X1, std::max(Y0 + 1.0f, Y1 - 1)), Col);
                if (Y1 - Y0 >= 15)
                {
                    pDraw->PushClipRect(ImVec2(X0, std::max(Y0, C0.y)), ImVec2(X1, std::min(Y1, C0.y + Size.y)), true);
                    const float Lo = std::max(Y0, C0.y) + 1, Hi = std::max(Lo, std::min(Y1, C0.y + Size.y) - 15);     // (the label stays in the visible part)
                    const float Ty = std::clamp((Y0 + Y1) * 0.5f - 7, Lo, Hi);
                    pDraw->AddText(ImVec2(X0 + 6, Ty), IM_COL32(16, 16, 16, 255), Label.c_str());
                    pDraw->PopClipRect();
                }
                if (bHover && IO.MousePos.x >= X0 && IO.MousePos.x < X1 && IO.MousePos.y >= Y0 && IO.MousePos.y < std::max(Y0 + 1.0f, Y1)) Hover = Tip;
            };
            const auto Mb = [&](std::uint64_t Bytes) { char T[32]; std::snprintf(T, sizeof(T), "%.2f MB", Bytes * MB); return std::string(T); };
            const ImU32 Blue = IM_COL32(104, 152, 196, 255), Grey = IM_COL32(150, 150, 150, 255);

            Box(0, 0, double(Total), Blue, "Memory: " + Mb(Total), "all the memory reported: " + Mb(Total));
            double Pool0 = 0;
            for (const auto& P : m_Mem)
            {
                Box(1, Pool0, Pool0 + double(P.m_Reserved), Blue, std::string(P.m_pName) + ": " + Mb(P.m_Reserved),
                    std::string(P.m_pName) + "\n" + Mb(P.m_Used) + " used of " + Mb(P.m_Reserved) + " reserved, " + std::to_string(P.m_Regions.size()) + " regions");
                double R0 = Pool0;
                for (const auto& R : P.m_Regions)
                {
                    const std::uint64_t RFree = R.m_Size - std::min(R.m_Used, R.m_Size);
                    char Frag[160]; std::snprintf(Frag, sizeof(Frag), "region %llx: %zu allocations, %.2f MB free, largest hole %.2f MB, fragmentation %.0f%%",
                        static_cast<unsigned long long>(R.m_Id), R.m_Allocs.size(), RFree * MB, R.m_LargestFree * MB, RFree ? 100.0 * (1.0 - double(R.m_LargestFree) / double(RFree)) : 0.0);
                    Box(2, R0, R0 + double(R.m_Size), Blue, "region: " + Mb(R.m_Size), Frag);
                    std::uint64_t End = 0;
                    for (const auto& A : R.m_Allocs)
                    {
                        if (A.m_Offset > End) Box(3, R0 + End, R0 + A.m_Offset, Grey, "FREE: " + Mb(A.m_Offset - End), "free: " + Mb(A.m_Offset - End));
                        const std::string Owner = A.m_pOwner ? A.m_pOwner : "(no owner)";
                        Box(3, R0 + A.m_Offset, R0 + A.m_Offset + A.m_Size, TagColor(A.m_pTag ? A.m_pTag : "?"), Owner + ": " + Mb(A.m_Size),
                            Owner + "\n" + (A.m_pTag ? A.m_pTag : "?") + ", " + Mb(A.m_Size) + " at " + Mb(A.m_Offset));
                        End = std::max(End, A.m_Offset + A.m_Size);
                    }
                    if (R.m_Size > End) Box(3, R0 + End, R0 + R.m_Size, Grey, "FREE: " + Mb(R.m_Size - End), "free: " + Mb(R.m_Size - End));
                    R0 += double(R.m_Size);
                }
                Pool0 += double(P.m_Reserved);
            }
            pDraw->PopClipRect();
            if (!Hover.empty()) ImGui::SetTooltip("%s", Hover.c_str());
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
        double                  m_MemZoom    = 1;       // the memory chart's view: 1 = all of it
        double                  m_MemTop     = 0;       // its first byte shown, 0..1 of all of it
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
