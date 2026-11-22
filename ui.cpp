#include "ui.hpp"
#include "hardware.hpp"
#include "macros.hpp"
#include "tweaks.hpp"
#include "util.hpp"
#include "imgui.h"
#include <objbase.h>
#include <commdlg.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

// ------------------------------------------------------------------ estado
Ctx g_ctx;
std::vector<std::unique_ptr<Tweak>> g_tweaks;
std::vector<std::pair<std::string, std::vector<Tweak*>>> g_groups;
MacroEngine g_macros;
MacroSettings g_ms;
LiveStats g_live;
double g_liveT = -10;
std::atomic<bool> g_busy{false};
char g_iniBuf[600];
char g_search[64] = "";
int g_filter = 0;                 // 0 todos, 1 recomendados, 2 aplicados
int* g_cap = nullptr;
bool g_armed = false;
std::vector<Log::Line> g_lines;
size_t g_lastLog = (size_t)-1;
bool g_logScroll = false;
ImFont* g_fontBig = nullptr;
int g_nav = 0, g_jump = -1;
ImVec4 g_accent;

// ------------------------------------------------------------------ cores
const ImVec4 kAccent(0.40f, 0.60f, 1.00f, 1.0f);
const ImVec4 kGreen(0.30f, 0.85f, 0.55f, 1.0f);
const ImVec4 kRed(1.00f, 0.40f, 0.42f, 1.0f);
const ImVec4 kYellow(1.00f, 0.80f, 0.32f, 1.0f);
const ImVec4 kPurple(0.72f, 0.52f, 1.00f, 1.0f);
const ImVec4 kDim(0.60f, 0.63f, 0.72f, 1.0f);
const ImVec4 kCard(0.095f, 0.10f, 0.135f, 1.0f);
const ImVec4 kCardOn(0.085f, 0.145f, 0.13f, 1.0f);

ImU32 U32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }

std::string fmt(const char* f, ...) {
    char b[512];
    va_list a;
    va_start(a, f);
    vsnprintf(b, sizeof(b), f, a);
    va_end(a);
    return b;
}

void runJob(std::function<void()> fn) {
    bool exp = false;
    if (!g_busy.compare_exchange_strong(exp, true)) { Log::warn("Aguarde a tarefa atual terminar."); return; }
    std::thread([fn = std::move(fn)] { fn(); g_busy = false; }).detach();
}

bool containsCI(const std::string& hay, const char* needle) {
    if (!*needle) return true;
    auto low = [](std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; };
    return low(hay).find(low(needle)) != std::string::npos;
}

// ------------------------------------------------------------------ tema
void applyTheme() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0; s.ChildRounding = 12; s.FrameRounding = 8; s.GrabRounding = 8;
    s.PopupRounding = 8; s.ScrollbarRounding = 8; s.TabRounding = 8;
    s.FramePadding = ImVec2(12, 7); s.ItemSpacing = ImVec2(10, 9); s.WindowPadding = ImVec2(18, 16);
    s.ScrollbarSize = 12; s.ChildBorderSize = 1; s.FrameBorderSize = 0;
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]        = ImVec4(0.055f, 0.057f, 0.075f, 1);
    c[ImGuiCol_ChildBg]         = kCard;
    c[ImGuiCol_Border]          = ImVec4(0.17f, 0.18f, 0.25f, 0.9f);
    c[ImGuiCol_FrameBg]         = ImVec4(0.13f, 0.14f, 0.20f, 1);
    c[ImGuiCol_FrameBgHovered]  = ImVec4(0.18f, 0.20f, 0.29f, 1);
    c[ImGuiCol_FrameBgActive]   = ImVec4(0.21f, 0.24f, 0.35f, 1);
    c[ImGuiCol_Header]          = ImVec4(0.14f, 0.18f, 0.32f, 1);
    c[ImGuiCol_HeaderHovered]   = ImVec4(0.20f, 0.27f, 0.48f, 1);
    c[ImGuiCol_HeaderActive]    = ImVec4(0.24f, 0.32f, 0.56f, 1);
    c[ImGuiCol_Button]          = ImVec4(0.20f, 0.30f, 0.62f, 1);
    c[ImGuiCol_ButtonHovered]   = ImVec4(0.28f, 0.41f, 0.80f, 1);
    c[ImGuiCol_ButtonActive]    = ImVec4(0.17f, 0.26f, 0.54f, 1);
    c[ImGuiCol_CheckMark]       = kAccent;
    c[ImGuiCol_SliderGrab]      = kAccent;
    c[ImGuiCol_SliderGrabActive]= ImVec4(0.55f, 0.72f, 1.0f, 1);
    c[ImGuiCol_PlotHistogram]   = kAccent;
    c[ImGuiCol_ScrollbarBg]     = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]   = ImVec4(0.22f, 0.24f, 0.34f, 1);
    c[ImGuiCol_Separator]       = ImVec4(0.17f, 0.18f, 0.25f, 1);
    c[ImGuiCol_TableBorderLight]= ImVec4(0, 0, 0, 0);
}

// ------------------------------------------------------------------ widgets
void pill(const char* t, const ImVec4& c) {
    ImVec2 ts = ImGui::CalcTextSize(t), p = ImGui::GetCursorScreenPos();
    const float px = 8, py = 1;
    ImDrawList* d = ImGui::GetWindowDrawList();
    d->AddRectFilled(p, ImVec2(p.x + ts.x + px * 2, p.y + ts.y + py * 2), U32(ImVec4(c.x, c.y, c.z, 0.18f)), 10);
    d->AddText(ImVec2(p.x + px, p.y + py), U32(c), t);
    ImGui::Dummy(ImVec2(ts.x + px * 2, ts.y + py * 2));
}
void pillSame(const char* t, const ImVec4& c) { ImGui::SameLine(0, 8); pill(t, c); }

bool toggle(const char* id, bool* v) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetFrameHeight() * 0.78f, w = h * 1.9f;
    ImGui::InvisibleButton(id, ImVec2(w, h));
    bool changed = false;
    if (ImGui::IsItemClicked()) { *v = !*v; changed = true; }
    ImU32 bg = *v ? U32(ImVec4(0.30f, 0.52f, 1.0f, 1)) : U32(ImVec4(0.24f, 0.26f, 0.36f, 1));
    if (ImGui::IsItemHovered()) bg = *v ? U32(ImVec4(0.40f, 0.60f, 1.0f, 1)) : U32(ImVec4(0.30f, 0.32f, 0.44f, 1));
    ImDrawList* d = ImGui::GetWindowDrawList();
    d->AddRectFilled(p, ImVec2(p.x + w, p.y + h), bg, h * 0.5f);
    d->AddCircleFilled(ImVec2(p.x + h * 0.5f + (*v ? (w - h) : 0.f), p.y + h * 0.5f), h * 0.5f - 3.0f, IM_COL32(255, 255, 255, 255));
    return changed;
}

void toggleRow(const char* id, const char* label, bool* v) {
    toggle(id, v);
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
}

void sectionTitle(const char* title, const char* sub) {
    if (g_fontBig) ImGui::PushFont(g_fontBig);
    ImGui::TextUnformatted(title);
    if (g_fontBig) ImGui::PopFont();
    if (sub && *sub) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextWrapped("%s", sub);
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();
}

void beginCard(const char* id, const ImVec4& accent) {
    g_accent = accent;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kCard);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 15));
    ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
}
void endCard() {
    ImDrawList* d = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetWindowPos();
    d->AddRectFilled(ImVec2(p.x + 1, p.y + 12), ImVec2(p.x + 4, p.y + ImGui::GetWindowHeight() - 12), U32(g_accent), 2);
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void cardTitle(const char* t, const ImVec4& c) { ImGui::TextColored(c, "%s", t); ImGui::Spacing(); }

void dimText(const char* t) {
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped("%s", t);
    ImGui::PopStyleColor();
}

void pollCapture() {
    if (!g_cap) return;
    int hit = 0;
    for (int vk = 3; vk < 255; ++vk)
        if (GetAsyncKeyState(vk) & 0x8000) { hit = vk; break; }
    if (!g_armed) { if (!hit) g_armed = true; return; }   // espera soltar o clique do botão
    if (hit) { *g_cap = hit; g_cap = nullptr; }
}

void keyPicker(const char* label, int* vk, bool mouseShortcuts = false) {
    ImGui::PushID(label);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(300);
    std::string txt = (g_cap == vk) ? "pressione uma tecla..." : vkName(*vk);
    if (g_cap == vk) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.5f, 0.1f, 1));
    if (ImGui::Button(txt.c_str(), ImVec2(200, 0))) { g_cap = vk; g_armed = false; }
    if (g_cap == vk) ImGui::PopStyleColor();
    if (mouseShortcuts) {
        ImGui::SameLine(); if (ImGui::SmallButton("LMB")) *vk = VK_LBUTTON;
        ImGui::SameLine(); if (ImGui::SmallButton("RMB")) *vk = VK_RBUTTON;
    }
    ImGui::PopID();
}

void slider(const char* label, int* v, int lo, int hi, const char* unit = "ms") {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(300);
    ImGui::SetNextItemWidth(300);
    ImGui::PushID(label);
    ImGui::SliderInt("##s", v, lo, hi, (std::string("%d ") + unit).c_str());
    ImGui::PopID();
}

void updateLive() {
    double t = ImGui::GetTime();
    if (t - g_liveT >= 1.0) { g_live = pollLive(); g_liveT = t; }
}

void jumpHere(int id) {
    if (g_jump == id) { ImGui::SetScrollHereY(0.0f); g_jump = -1; }
}

// ------------------------------------------------------------------ painel
void statCard(const char* id, const char* title, const ImVec4& col, const std::string& big, const std::string& sub, float frac, float w) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kCard);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));
    ImGui::BeginChild(id, ImVec2(w, 150), ImGuiChildFlags_Borders);
    ImGui::TextColored(col, "%s", title);
    ImGui::Spacing();
    ImGui::TextWrapped("%s", big.c_str());
    dimText(sub.c_str());
    if (frac >= 0) {
        ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 30);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
        ImGui::ProgressBar(frac, ImVec2(-1, 8), "");
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void drawDashboard() {
    const HardwareInfo& hw = g_ctx.hw;
    sectionTitle("Painel", "Hardware detectado automaticamente. As recomendações de cada tweak são calculadas a partir disso.");

    float sp = ImGui::GetStyle().ItemSpacing.x;
    float w = (ImGui::GetContentRegionAvail().x - 3 * sp) / 4.0f;
    const char* tier = hardwareTier(hw) == 2 ? "PC forte" : hardwareTier(hw) == 1 ? "PC intermediário" : "PC básico";
    statCard("cpu", "PROCESSADOR", kAccent, hw.cpuName, fmt("%d núcleos / %d threads  -  %.0f%% em uso", hw.physCores, hw.logCores, g_live.cpu), (float)g_live.cpu / 100.f, w);
    ImGui::SameLine();
    statCard("gpu", "PLACA DE VÍDEO", kPurple, hw.gpuName, fmt("%llu MB de VRAM  -  %s", (unsigned long long)hw.vramMB, tier), -1.f, w);
    ImGui::SameLine();
    statCard("ram", "MEMÓRIA", kGreen, fmt("%.1f GB", hw.ramTotalGB), fmt("%.1f GB em uso (%.0f%%)", g_live.ramUsedGB, g_live.ramUsedPct), (float)g_live.ramUsedPct / 100.f, w);
    ImGui::SameLine();
    float used = hw.totalGB > 0 ? (float)(1.0 - hw.freeGB / hw.totalGB) : 0.f;
    statCard("disk", "ARMAZENAMENTO", kYellow, fmt("%s  -  %s", hw.gameDrive.c_str(), hw.ssdKnown ? (hw.ssd ? "SSD" : "HDD") : "tipo ?"),
             fmt("%.0f GB livres de %.0f GB", hw.freeGB, hw.totalGB), used, w);

    ImGui::Spacing();
    beginCard("sys", kAccent);
    cardTitle("SISTEMA E JOGO", kAccent);
    ImGui::TextDisabled("Windows");  ImGui::SameLine(160); ImGui::TextUnformatted(hw.osName.c_str());
    ImGui::TextDisabled("Monitor");  ImGui::SameLine(160); ImGui::Text("%dx%d @ %d Hz%s", hw.screenW, hw.screenH, hw.refreshHz, hw.isLaptop ? "   (notebook)" : "");
    ImGui::Spacing();
    ImGui::TextDisabled("GameUserSettings.ini");
    ImGui::BeginDisabled(g_busy.load());
    ImGui::SetNextItemWidth(-290);
    if (ImGui::InputText("##ini", g_iniBuf, sizeof(g_iniBuf))) setIniPath(g_ctx.hw, std::filesystem::path(util::widen(g_iniBuf)));
    ImGui::SameLine();
    if (ImGui::Button("Procurar...")) {
        wchar_t f[MAX_PATH]{};
        OPENFILENAMEW o{};
        o.lStructSize = sizeof(o);
        o.lpstrFilter = L"INI\0*.ini\0Todos\0*.*\0";
        o.lpstrFile = f;
        o.nMaxFile = MAX_PATH;
        o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameW(&o)) {
            setIniPath(g_ctx.hw, f);
            snprintf(g_iniBuf, sizeof(g_iniBuf), "%s", util::narrow(f).c_str());
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Auto-detectar")) {
        HardwareInfo fresh = detectHardware();
        g_ctx.hw.iniPath = fresh.iniPath; g_ctx.hw.iniFound = fresh.iniFound;
        g_ctx.hw.fortniteExe = fresh.fortniteExe; g_ctx.hw.exeFound = fresh.exeFound;
        snprintf(g_iniBuf, sizeof(g_iniBuf), "%s", util::narrow(g_ctx.hw.iniPath.wstring()).c_str());
    }
    ImGui::EndDisabled();
    if (hw.iniFound) ImGui::TextColored(kGreen, "Arquivo encontrado.");
    else ImGui::TextColored(kYellow, "Arquivo não encontrado: abra o Fortnite uma vez ou selecione manualmente. Os tweaks do .ini ficam indisponíveis.");
    if (hw.exeFound) ImGui::TextColored(kGreen, "Fortnite: %s", util::narrow(hw.fortniteExe.wstring()).c_str());
    else ImGui::TextColored(kYellow, "Executável do Fortnite não encontrado: tweaks que dependem dele ficam indisponíveis.");
    ImGui::Spacing();
    if (g_ctx.admin) ImGui::TextColored(kGreen, "Executando como administrador: todos os tweaks liberados.");
    else {
        ImGui::TextColored(kYellow, "Sem administrador: tweaks marcados ADMIN não rodam.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reiniciar como administrador")) util::relaunchAsAdmin();
    }
    endCard();

    ImGui::Spacing();
    beginCard("honest", kYellow);
    cardTitle("QUANTO FPS EU GANHO?", kYellow);
    ImGui::TextWrapped("Os tweaks tiram carga do jogo e do Windows e estabilizam o frametime (1%% low). Em PCs fracos, qualidade mínima + resolução menor "
                       "costumam dar de +30%% a +100%% de FPS. O teto, porém, é do seu CPU/GPU: nenhum programa multiplica o FPS por 10.");
    endCard();
}

// ------------------------------------------------------------------ presets
void drawPresets() {
    sectionTitle("Presets", "Dois botões que aplicam tudo de uma vez, com backup. Dá para reverter tudo no final.");
    beginCard("presets", kAccent);
    ImGui::BeginDisabled(g_busy.load());
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.45f, 0.95f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.55f, 1.0f, 1));
    if (ImGui::Button("Preset Competitivo", ImVec2(260, 46))) runJob([] { applyPreset(g_ctx, g_tweaks, false); });
    ImGui::PopStyleColor(2);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.45f, 0.10f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.55f, 0.18f, 1));
    if (ImGui::Button("Preset FPS Máximo", ImVec2(260, 46))) runJob([] { applyPreset(g_ctx, g_tweaks, true); });
    ImGui::PopStyleColor(2);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.18f, 0.20f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.24f, 0.26f, 1));
    if (ImGui::Button("Reverter TUDO", ImVec2(200, 46))) runJob([] { revertAll(g_ctx, g_tweaks); });
    ImGui::PopStyleColor(2);
    ImGui::EndDisabled();
    if (g_busy) { ImGui::SameLine(); ImGui::TextColored(kYellow, "Trabalhando..."); }
    ImGui::Spacing();
    dimText("Competitivo: valores recomendados para o seu PC (visual limpo, boa distância de visão, input rápido). "
            "FPS Máximo: tudo no mínimo possível, Modo Desempenho (ES31) e DX11. Fica feio, mas é o máximo de FPS que o jogo entrega. "
            "Itens arriscados ou de segurança (HVCI, Defender, MPO...) nunca entram nos presets.");
    endCard();
}

// ------------------------------------------------------------------ tweaks
bool tweakVisible(const Tweak& t) {
    if (!(containsCI(t.name, g_search) || containsCI(t.desc, g_search))) return false;
    if (g_filter == 1) return t.impl == Impl::Real && t.recommended >= 0;
    if (g_filter == 2) return t.applied;
    return true;
}

void drawTweakRow(Tweak& t) {
    const HardwareInfo& hw = g_ctx.hw;
    const bool busy = g_busy.load();
    const bool unavailable = (t.usesIni && !hw.iniFound) || (t.usesExe && !hw.exeFound);
    const bool sim = t.impl == Impl::Simulated;

    ImGui::PushID(t.id);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, t.applied ? kCardOn : kCard);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 12));
    ImGui::BeginChild("card", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    if (ImGui::BeginTable("t", 2, ImGuiTableFlags_None)) {
        ImGui::TableSetupColumn("l", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("r", ImGuiTableColumnFlags_WidthFixed, 340.f);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextColored(kAccent, "%02d", t.id);
        ImGui::SameLine();
        ImGui::TextUnformatted(t.name.c_str());
        pillSame(sim ? "SIMULADO" : "REAL", sim ? kYellow : kGreen);
        if (t.needsAdmin) pillSame("ADMIN", kRed);
        if (t.needsReboot) pillSame("REINICIAR", kYellow);
        if (t.applied) pillSame("APLICADO", kGreen);
        dimText(t.desc.c_str());
        if (!t.note.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.48f, 0.58f, 1));
            ImGui::TextWrapped("%s", t.note.c_str());
            ImGui::PopStyleColor();
        }
        if (unavailable) ImGui::TextColored(kYellow, "Indisponível: %s", t.usesIni && !hw.iniFound ? ".ini não encontrado" : "Fortnite não encontrado");

        ImGui::TableNextColumn();
        ImGui::BeginDisabled(unavailable);
        if (!t.options.empty()) {
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##opt", t.options[(size_t)t.choice].label.c_str())) {
                for (int i = 0; i < (int)t.options.size(); ++i) {
                    std::string l = t.options[(size_t)i].label + (i == t.recommended ? "   (recomendado)" : "");
                    if (ImGui::Selectable(l.c_str(), i == t.choice)) t.choice = i;
                }
                ImGui::EndCombo();
            }
            if (t.recommended >= 0) {
                ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
                ImGui::TextWrapped("Recomendado: %s", t.options[(size_t)t.recommended].label.c_str());
                ImGui::PopStyleColor();
            } else dimText("Não recomendado para o seu PC (opcional)");
        }
        ImGui::BeginDisabled(busy);
        if (ImGui::Button(sim ? "Ver orientação" : "Aplicar", ImVec2(sim ? -1.f : 160.f, 0))) {
            Tweak* p = &t;
            runJob([p] { applyTweak(g_ctx, *p); });
        }
        if (!sim) {
            ImGui::SameLine();
            ImGui::BeginDisabled(!t.applied);
            if (ImGui::Button("Reverter", ImVec2(-1.f, 0))) {
                Tweak* p = &t;
                runJob([p] { revertTweak(g_ctx, *p); });
            }
            ImGui::EndDisabled();
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::EndTable();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::PopID();
}

void drawTweaks() {
    int applied = 0;
    for (auto& t : g_tweaks) if (t->applied) ++applied;
    sectionTitle("Otimizações", fmt("%d tweaks, %d aplicados. Todo valor antigo é salvo em %%APPDATA%%\\FNBoost e pode ser revertido.", (int)g_tweaks.size(), applied).c_str());

    ImGui::SetNextItemWidth(320);
    ImGui::InputTextWithHint("##search", "Buscar tweak...", g_search, sizeof(g_search));
    ImGui::SameLine(); ImGui::RadioButton("Todos", &g_filter, 0);
    ImGui::SameLine(); ImGui::RadioButton("Recomendados", &g_filter, 1);
    ImGui::SameLine(); ImGui::RadioButton("Aplicados", &g_filter, 2);
    ImGui::Spacing();

    int gi = 0;
    for (auto& [cat, list] : g_groups) {
        int vis = 0;
        for (Tweak* t : list) if (tweakVisible(*t)) ++vis;
        jumpHere(10 + gi);
        ++gi;
        if (!vis) continue;
        std::string h = cat + "   (" + std::to_string(vis) + ")";
        if (ImGui::CollapsingHeader(h.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
            for (Tweak* t : list) if (tweakVisible(*t)) drawTweakRow(*t);
        ImGui::Spacing();
    }
}

// ------------------------------------------------------------------ macros
void testButton(int idx) {
    ImGui::SameLine();
    if (ImGui::SmallButton("Testar em 3 s")) g_macros.testRun(idx, 3000);
}

void drawMacros() {
    sectionTitle("Macros para Fortnite", "Ghost bind, Double edit e Drag. Todos os tempos são sincronizados com o FPS do jogo para o clique nunca ser perdido.");

    ImGui::TextColored(kRed, "Aviso: macros que fazem várias ações com um aperto podem violar as regras da Epic/anti-cheat e causar banimento. Use em Criativo/offline.");
    dimText("Se o Fortnite roda como administrador, rode o FNBoost como administrador também (o Windows bloqueia input entre níveis diferentes).");
    ImGui::Spacing();

    if (g_macros.killed()) {
        g_ms.ghost.enabled = g_ms.dedit.enabled = g_ms.drag.enabled = false;
        ImGui::TextColored(kRed, "KILL SWITCH ATIVO: macros desligadas e teclas soltas.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Rearmar")) g_macros.rearm();
    }
    ImGui::TextDisabled("Status:"); ImGui::SameLine(); ImGui::TextUnformatted(g_macros.status().c_str());
    ImGui::Spacing();

    beginCard("sync", kGreen);
    cardTitle("SINCRONIZAÇÃO COM O FPS (corrige macro que 'às vezes não funciona')", kGreen);
    toggleRow("##sync", "Sincronizar com o FPS do jogo", &g_ms.syncFps);
    slider("FPS que você faz no jogo", &g_ms.gameFps, 30, 360, "FPS");
    slider("Tempo da tecla pressionada (mínimo)", &g_ms.holdMs, 1, 60);
    ImGui::TextColored(kDim, "1 frame = %.1f ms  |  tecla fica pressionada por %.1f ms  |  pausas mínimas = %.1f ms",
                       g_macros.frameMs(), g_macros.effHoldMs(), g_macros.frameMs());
    dimText("O jogo lê o input uma vez por frame. Se a tecla fica pressionada por menos de 1 frame, o clique pode nem existir para ele. "
            "Use o FPS que você realmente faz em partida (ou o limite que configurou).");
    endCard();
    ImGui::Spacing();

    beginCard("kill", kRed);
    cardTitle("KILL SWITCH DE EMERGÊNCIA", kRed);
    keyPicker("Tecla de emergência (global)", &g_ms.killVk);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.15f, 0.18f, 1));
    if (ImGui::Button("PARAR TUDO AGORA")) g_macros.killNow();
    ImGui::PopStyleColor();
    endCard();
    ImGui::Spacing();

    auto warnTrigger = [](int trig) {
        if (trig == g_ms.killVk) ImGui::TextColored(kRed, "A tecla de disparo é igual à do Kill Switch: o disparo será ignorado.");
    };

    beginCard("ghost", kAccent);
    cardTitle("1) GHOST BIND", kAccent);
    toggleRow("##g", "Ativar", &g_ms.ghost.enabled);
    testButton(0);
    keyPicker("Tecla de disparo", &g_ms.ghost.trigger);
    warnTrigger(g_ms.ghost.trigger);
    keyPicker("Ação A (ex.: slot)", &g_ms.ghost.keyA, true);
    keyPicker("Ação B (ex.: construção)", &g_ms.ghost.keyB, true);
    slider("Delay entre ações", &g_ms.ghost.delayMs, 1, 300);
    slider("Repetições", &g_ms.ghost.repeats, 1, 10, "x");
    toggleRow("##g2", "Voltar para a ação A no final", &g_ms.ghost.returnToA);
    endCard();
    ImGui::Spacing();

    beginCard("dedit", kPurple);
    cardTitle("2) DOUBLE EDIT", kPurple);
    toggleRow("##d", "Ativar", &g_ms.dedit.enabled);
    testButton(1);
    keyPicker("Tecla de disparo ", &g_ms.dedit.trigger);
    warnTrigger(g_ms.dedit.trigger);
    keyPicker("Editar", &g_ms.dedit.editKey, true);
    keyPicker("Selecionar", &g_ms.dedit.selectKey, true);
    keyPicker("Confirmar", &g_ms.dedit.confirmKey, true);
    keyPicker("Resetar", &g_ms.dedit.resetKey, true);
    slider("Delay após editar", &g_ms.dedit.dEdit, 1, 200);
    slider("Delay após selecionar", &g_ms.dedit.dSelect, 1, 200);
    slider("Delay após confirmar", &g_ms.dedit.dConfirm, 1, 200);
    slider("Delay após resetar", &g_ms.dedit.dReset, 1, 200);
    dimText("Use as mesmas teclas dos seus binds no jogo.");
    endCard();
    ImGui::Spacing();

    beginCard("drag", kYellow);
    cardTitle("3) DRAG EDIT", kYellow);
    toggleRow("##dr", "Ativar", &g_ms.drag.enabled);
    testButton(2);
    keyPicker("Tecla de disparo  ", &g_ms.drag.trigger);
    warnTrigger(g_ms.drag.trigger);
    toggleRow("##dr1", "Apertar a tecla de editar antes", &g_ms.drag.useEdit);
    if (g_ms.drag.useEdit) {
        keyPicker("Tecla de editar", &g_ms.drag.editKey, true);
        slider("Espera após editar", &g_ms.drag.preDelayMs, 1, 200);
    }
    toggleRow("##dr2", "Segurar botão esquerdo durante o arrasto", &g_ms.drag.holdLeft);
    slider("Espera após apertar (antes de mover)", &g_ms.drag.settleMs, 1, 200);
    slider("Delta X", &g_ms.drag.dx, -1000, 1000, "px");
    slider("Delta Y", &g_ms.drag.dy, -1000, 1000, "px");
    slider("Duração do movimento", &g_ms.drag.durationMs, 1, 1000);
    slider("Passos do movimento", &g_ms.drag.steps, 1, 100, "passos");
    slider("Parado no fim (ainda pressionado)", &g_ms.drag.endHoldMs, 1, 200);
    toggleRow("##dr3", "Confirmar edição no final", &g_ms.drag.confirmAfter);
    if (g_ms.drag.confirmAfter) {
        keyPicker("Tecla de confirmar", &g_ms.drag.confirmKey, true);
        slider("Espera antes de confirmar", &g_ms.drag.confirmDelayMs, 1, 200);
    }
    dimText("Sequência: editar -> espera -> botão pressionado -> espera -> movimento em passos (cada passo >= 1 frame) -> parado -> solta -> confirma. "
            "O movimento é relativo e sofre com 'precisão do ponteiro' do Windows: desligue-a na seção CPU e energia.");
    endCard();
}

void drawHowItWorks() {
    beginCard("how", kAccent);
    cardTitle("COMO FUNCIONA", kAccent);
    ImGui::TextWrapped("O .ini reduz o que o engine renderiza (maior alavanca de FPS). Ajustes do Windows reduzem disputa de CPU e variação de frametime. "
                       "Rede só melhora DNS/jitter local. Frametime estável importa mais que pico de FPS para mira e edição.");
    ImGui::Spacing();
    ImGui::TextWrapped("As macros rodam em uma thread que consulta GetAsyncKeyState a cada ~1 ms e injeta teclado/mouse com SendInput (scancodes). "
                       "O Kill Switch é checado dentro de cada espera, solta tudo que a macro pressionou e desativa os três macros.");
    endCard();
}

void drawLog(float h) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.045f, 0.047f, 0.065f, 1));
    ImGui::BeginChild("log", ImVec2(0, h), ImGuiChildFlags_Borders);
    ImGui::TextColored(kAccent, "LOG");
    ImGui::SameLine();
    if (ImGui::SmallButton("Copiar")) {
        std::string all;
        for (auto& l : g_lines) all += l.text + "\n";
        ImGui::SetClipboardText(all.c_str());
    }
    ImGui::Separator();
    size_t cnt = Log::counter();
    if (cnt != g_lastLog) { g_lines = Log::snapshot(); g_lastLog = cnt; g_logScroll = true; }
    for (auto& l : g_lines) {
        ImVec4 c = l.lvl == Log::Level::Ok ? kGreen : l.lvl == Log::Level::Warn ? kYellow : l.lvl == Log::Level::Error ? kRed : kDim;
        ImGui::TextColored(c, "%s", l.text.c_str());
    }
    if (g_logScroll) { ImGui::SetScrollHereY(1.0f); g_logScroll = false; }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// ------------------------------------------------------------------ layout
void navBtn(const char* label, int id) {
    bool sel = g_nav == id;
    ImGui::PushStyleColor(ImGuiCol_Button, sel ? ImVec4(0.20f, 0.30f, 0.62f, 1) : ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.16f, 0.21f, 0.40f, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
    ImGui::PushID(id);
    if (ImGui::Button(label, ImVec2(-1, 36))) { g_nav = id; g_jump = id; }
    ImGui::PopID();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

void drawSidebar() {
    static const char* shortNames[] = {"Fortnite (.ini)", "Resolução / esticada", "CPU e energia", "GPU", "Memória e disco", "Rede", "Avançado"};
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.040f, 0.042f, 0.058f, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 18));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0);
    ImGui::BeginChild("side", ImVec2(250, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    if (g_fontBig) ImGui::PushFont(g_fontBig);
    ImGui::TextColored(kAccent, "FNBoost");
    if (g_fontBig) ImGui::PopFont();
    ImGui::TextDisabled("Otimizador e macros");
    ImGui::Spacing(); ImGui::Spacing();

    navBtn("Painel", 0);
    navBtn("Presets", 1);
    ImGui::Spacing();
    ImGui::TextDisabled("OTIMIZAÇÕES");
    for (size_t i = 0; i < g_groups.size(); ++i) navBtn(i < 7 ? shortNames[i] : g_groups[i].first.c_str(), 10 + (int)i);
    ImGui::Spacing();
    ImGui::TextDisabled("MACROS");
    navBtn("Macros do Fortnite", 99);

    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 78);
    ImGui::Separator();
    ImGui::Spacing();
    if (g_ctx.admin) ImGui::TextColored(kGreen, "Administrador: SIM");
    else {
        ImGui::TextColored(kYellow, "Administrador: NÃO");
        if (ImGui::SmallButton("Reiniciar como admin")) util::relaunchAsAdmin();
    }
    if (g_busy) ImGui::TextColored(kYellow, "Trabalhando...");
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void drawContent() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 20));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0);
    ImGui::BeginChild("content", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);

    const float logH = 160.0f;
    ImGui::BeginChild("main", ImVec2(0, -(logH + 10.0f)), ImGuiChildFlags_None);
    jumpHere(0);
    drawDashboard();
    ImGui::Spacing();
    drawHowItWorks();
    ImGui::Spacing(); ImGui::Spacing();
    jumpHere(1);
    drawPresets();
    ImGui::Spacing(); ImGui::Spacing();
    drawTweaks();
    ImGui::Spacing(); ImGui::Spacing();
    jumpHere(99);
    drawMacros();             // rolando para baixo
    ImGui::Dummy(ImVec2(0, 30));
    ImGui::EndChild();

    drawLog(logH);
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

} // namespace

// ------------------------------------------------------------------------------------------
namespace ui {

void init() {
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    static const ImWchar ranges[] = {0x0020, 0x00FF, 0x2000, 0x206F, 0};
    if (!io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 18.0f, nullptr, ranges)) io.Fonts->AddFontDefault();
    g_fontBig = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", 28.0f, nullptr, ranges);
    applyTheme();

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g_ctx.hw = detectHardware();
    g_ctx.admin = util::isAdmin();
    g_ctx.backup.load();
    g_tweaks = buildTweaks(g_ctx.hw, g_ctx.backup);
    for (auto& t : g_tweaks) {
        if (g_groups.empty() || g_groups.back().first != t->category) g_groups.push_back({t->category, {}});
        g_groups.back().second.push_back(t.get());
    }
    snprintf(g_iniBuf, sizeof(g_iniBuf), "%s", util::narrow(g_ctx.hw.iniPath.wstring()).c_str());

    g_ms.gameFps = std::max(60, g_ctx.hw.refreshHz);   // chute inicial: taxa do monitor
    g_macros.set(g_ms);
    g_macros.start();

    Log::ok("FNBoost iniciado. " + std::to_string(g_tweaks.size()) + " tweaks carregados.");
    Log::info("CPU: " + g_ctx.hw.cpuName + " | GPU: " + g_ctx.hw.gpuName);
    if (!g_ctx.admin) Log::warn("Sem administrador: tweaks [ADMIN] ficarão bloqueados.");
}

void frame() {
    pollCapture();
    updateLive();

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                  ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar();

    drawSidebar();
    ImGui::SameLine(0, 0);
    drawContent();

    ImGui::End();
    g_macros.set(g_ms);
}

void shutdown() {
    g_macros.stop();
    while (g_busy.load()) Sleep(50);
}

} // namespace ui
