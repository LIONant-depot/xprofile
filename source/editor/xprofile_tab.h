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
                if (ImGui::BeginTabItem("Memory", nullptr, Pick("Memory") | Pick("VRAM") | Pick("System RAM")))  { DrawMemory(); ImGui::EndTabItem(); }
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

        // The memory pools (always kept, Record on or off) as one map: every pool's regions one after the other, each allocation (its
        // owner and size, in its tag's colour) and each hole (grey) in address order - the strip read top to bottom, column after column
        // like text. Pools (kinds of memory: VRAM, ReBAR, shared system RAM) one after the other, each starting at a thick orange line;
        // region starts: a thin line (hover: the pool and the region). Fit: all the memory in the columns; wheel: zoom around the mouse (the
        // strip longer); right button: scroll along it.
        // Two address spaces, two maps: VRAM (the card's: pools named "VRAM...") and System RAM (everything else - the CPU's data and the
        // shared memory the GPU reads over the bus). ponytail: the space by the pool's name prefix; a pool attribute if a third space comes
        void DrawMemory(void) noexcept
        {
            if (m_MemRefresh-- <= 0) { MemSnapshot(m_Mem); m_MemRefresh = 15; }
            if (m_Mem.empty()) { ImGui::TextDisabled("No memory pool reported yet."); return; }
            if (ImGui::BeginTabBar("##spaces"))
            {
                for (int Space = 0; Space < 2; ++Space)
                {
                    const char* pSpace = Space == 0 ? "VRAM" : "System RAM";
                    if (!ImGui::BeginTabItem(pSpace, nullptr, m_Select == pSpace ? ImGuiTabItemFlags_SetSelected : 0)) continue;
                    std::vector<mem_pool> Pools;
                    for (const auto& P : m_Mem) if ((std::string_view(P.m_pName).starts_with("VRAM")) == (Space == 0)) Pools.push_back(P);
                    if (Pools.empty()) ImGui::TextDisabled("Nothing reported in this memory yet.");
                    else DrawMemoryMap(Pools, m_MemView[Space]);
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }

        struct mem_view { double m_Zoom = 1, m_Top = 0; int m_Columns = 4; };      // a map's view: 1 = all of it; its first byte shown (0..1); the strip's columns

        void DrawMemoryMap(const std::vector<mem_pool>& Pools, mem_view& V) noexcept
        {
            constexpr double MB = 1.0 / (1024.0 * 1024.0);

            // the totals and the legend
            std::uint64_t Total = 0;
            std::map<std::string, int> Tags;
            for (const auto& P : Pools)
            {
                std::uint64_t Free = 0, Largest = 0;
                for (const auto& R : P.m_Regions) { Free += R.m_Size - std::min(R.m_Used, R.m_Size); Largest = std::max(Largest, R.m_LargestFree); for (const auto& A : R.m_Allocs) Tags.emplace(A.m_pTag ? A.m_pTag : "?", 0); }
                ImGui::Text("%s: %.1f MB used in %u allocations, %.1f MB reserved in %zu regions (peak %.1f MB); free inside them %.1f MB, largest hole %.1f MB",
                    P.m_pName, P.m_Used * MB, P.m_nAllocs, P.m_Reserved * MB, P.m_Regions.size(), P.m_Peak * MB, Free * MB, Largest * MB);
                Total += P.m_Reserved;
            }
            if (ImGui::Button("Fit")) { V.m_Zoom = 1; V.m_Top = 0; }
            ImGui::SameLine(); ImGui::SetNextItemWidth(90); ImGui::SliderInt("columns", &V.m_Columns, 1, 12);
            ImGui::SameLine(); ImGui::TextDisabled("(wheel: zoom, right button: scroll)");
            for (const auto& [Tag, n] : Tags)
            {
                ImGui::SameLine(0, 18);
                ImGui::ColorButton(("##" + Tag).c_str(), ImGui::ColorConvertU32ToFloat4(TagColor(Tag.c_str())), ImGuiColorEditFlags_NoTooltip, ImVec2(12, 12));
                ImGui::SameLine(); ImGui::TextUnformatted(Tag.c_str());
            }
            ImGui::SameLine(0, 18); ImGui::ColorButton("##free", ImGui::ColorConvertU32ToFloat4(IM_COL32(150, 150, 150, 255)), ImGuiColorEditFlags_NoTooltip, ImVec2(12, 12));
            ImGui::SameLine(); ImGui::TextUnformatted("free");
            if (Total == 0) return;

            // the canvas: n columns of height H; the strip's pixel p is column p / H, row p % H; V.m_Top: the first byte shown (0..1)
            const ImVec2 C0 = ImGui::GetCursorScreenPos();
            const ImVec2 Size(std::max(ImGui::GetContentRegionAvail().x, 200.0f), std::max(ImGui::GetContentRegionAvail().y, 240.0f));
            ImGui::InvisibleButton("##memory", Size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
            const bool   bHover = ImGui::IsItemHovered();
            auto&        IO     = ImGui::GetIO();
            const int    nCols  = std::clamp(V.m_Columns, 1, 12);
            const float  Gap    = 6, ColW = (Size.x - Gap * (nCols - 1)) / nCols, H = Size.y;
            const double View   = double(nCols) * H;                                // the strip's pixels on screen
            const auto   StripAt = [&](ImVec2 M) { const int c = std::clamp(int((M.x - C0.x) / (ColW + Gap)), 0, nCols - 1); return c * double(H) + std::clamp(double(M.y - C0.y), 0.0, double(H)); };
            if (bHover && IO.MouseWheel != 0)
            {
                const double OldPx = View * V.m_Zoom / double(Total);
                const double At    = V.m_Top * double(Total) + StripAt(IO.MousePos) / OldPx;      // the byte under the mouse stays there
                V.m_Zoom = std::clamp(V.m_Zoom * std::pow(1.25, IO.MouseWheel), 1.0, 1e6);
                V.m_Top  = (At - StripAt(IO.MousePos) / (View * V.m_Zoom / double(Total))) / double(Total);
            }
            const double Px  = View * V.m_Zoom / double(Total);                   // pixels per byte
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0)) V.m_Top -= IO.MouseDelta.y / Px / double(Total);
            V.m_Top = std::clamp(V.m_Top, 0.0, std::max(0.0, 1.0 - 1.0 / V.m_Zoom));
            const double TopB = V.m_Top * double(Total);

            auto* pDraw = ImGui::GetWindowDrawList();
            pDraw->PushClipRect(C0, ImVec2(C0.x + Size.x, C0.y + Size.y), true);
            for (int c = 0; c < nCols; ++c) { const float X = C0.x + c * (ColW + Gap); pDraw->AddRectFilled(ImVec2(X, C0.y), ImVec2(X + ColW, C0.y + H), IM_COL32(30, 30, 34, 255)); }
            std::string Hover;
            const auto Mb = [&](std::uint64_t Bytes) { char T[32]; std::snprintf(T, sizeof(T), "%.2f MB", Bytes * MB); return std::string(T); };

            // bytes [B0, B1) of the strip: a box in each column it falls in, its label in its tallest piece
            const auto Seg = [&](double B0, double B1, ImU32 Col, const std::string& Label, const std::string& Tip)
            {
                const double P0 = (B0 - TopB) * Px, P1 = (B1 - TopB) * Px;
                if (P1 < 0 || P0 >= View) return;
                int Best = -1; float BestY0 = 0, BestY1 = 0;
                for (int c = std::max(0, int(P0 / H)); c < nCols && c * double(H) < P1; ++c)
                {
                    const float Y0 = C0.y + float(std::max(P0, c * double(H)) - c * double(H)), Y1 = C0.y + float(std::min(P1, (c + 1) * double(H)) - c * double(H));
                    const float X0 = C0.x + c * (ColW + Gap), X1 = X0 + ColW;
                    pDraw->AddRectFilled(ImVec2(X0, Y0), ImVec2(X1, std::max(Y0 + 1.0f, Y1 - (Y1 - Y0 > 3 ? 1.0f : 0.0f))), Col);
                    if (Y1 - Y0 > BestY1 - BestY0) { Best = c; BestY0 = Y0; BestY1 = Y1; }
                    if (bHover && IO.MousePos.x >= X0 && IO.MousePos.x < X1 && IO.MousePos.y >= Y0 && IO.MousePos.y < std::max(Y0 + 1.0f, Y1)) Hover = Tip;
                }
                if (Best >= 0 && BestY1 - BestY0 >= 15)
                {
                    const float X0 = C0.x + Best * (ColW + Gap);
                    pDraw->PushClipRect(ImVec2(X0, BestY0), ImVec2(X0 + ColW, BestY1), true);
                    pDraw->AddText(ImVec2(X0 + 6, (BestY0 + BestY1) * 0.5f - 7), IM_COL32(16, 16, 16, 255), Label.c_str());
                    pDraw->PopClipRect();
                }
            };
            // a region's start: a line across its column
            const auto Line = [&](double B, ImU32 Col, float Thick)
            {
                const double P = (B - TopB) * Px;
                if (P < 0 || P >= View) return;
                const int c = int(P / H); const float X0 = C0.x + c * (ColW + Gap), Y = C0.y + float(P - c * double(H));
                pDraw->AddLine(ImVec2(X0, Y), ImVec2(X0 + ColW, Y), Col, Thick);
            };

            double R0 = 0;
            std::vector<double> PoolStarts;
            for (const auto& P : Pools)
            {
                PoolStarts.push_back(R0);
                for (const auto& R : P.m_Regions)
                {
                    const std::uint64_t RFree = R.m_Size - std::min(R.m_Used, R.m_Size);
                    char Where[200]; std::snprintf(Where, sizeof(Where), "\n%s region %llx: %.2f MB, %zu allocations, %.2f MB free, largest hole %.2f MB, fragmentation %.0f%%",
                        P.m_pName, static_cast<unsigned long long>(R.m_Id), R.m_Size * MB, R.m_Allocs.size(), RFree * MB, R.m_LargestFree * MB, RFree ? 100.0 * (1.0 - double(R.m_LargestFree) / double(RFree)) : 0.0);
                    std::uint64_t End = 0;
                    for (const auto& A : R.m_Allocs)
                    {
                        if (A.m_Offset > End) Seg(R0 + End, R0 + A.m_Offset, IM_COL32(150, 150, 150, 255), "FREE: " + Mb(A.m_Offset - End), "free: " + Mb(A.m_Offset - End) + Where);
                        const std::string Owner = A.m_pOwner ? A.m_pOwner : "(no owner)";
                        Seg(R0 + A.m_Offset, R0 + A.m_Offset + A.m_Size, TagColor(A.m_pTag ? A.m_pTag : "?"), Owner + ": " + Mb(A.m_Size),
                            Owner + "\n" + (A.m_pTag ? A.m_pTag : "?") + ", " + Mb(A.m_Size) + " at " + Mb(A.m_Offset) + Where);
                        End = std::max(End, A.m_Offset + A.m_Size);
                    }
                    if (R.m_Size > End) Seg(R0 + End, R0 + R.m_Size, IM_COL32(150, 150, 150, 255), "FREE: " + Mb(R.m_Size - End), "free: " + Mb(R.m_Size - End) + Where);
                    Line(R0, IM_COL32(235, 235, 235, 255), 2.0f);
                    R0 += double(R.m_Size);
                }
            }
            for (double B : PoolStarts) Line(B, IM_COL32(255, 150, 40, 255), 5.0f);     // a pool's start (a kind of memory: the header's lines in order)
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
        mem_view                m_MemView[2] {};        // the VRAM map's view, the System RAM map's
    #endif
        std::string             m_Select    {};             // a page to bring forward on the next Draw ("Metrics", "Memory", "VRAM", "System RAM": Profiler -Show)
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
