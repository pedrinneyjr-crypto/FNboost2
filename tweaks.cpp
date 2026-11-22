#include "tweaks.hpp"
#include "util.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <psapi.h>
#include <shellapi.h>
#include <sstream>
#include <tlhelp32.h>

namespace fs = std::filesystem;
using util::widen;
using util::narrow;

// ---------------------------------------------------------------------------------------------
// Backup (persistido em %APPDATA%\FNBoost\backup.tsv)
// ---------------------------------------------------------------------------------------------
void Backup::load() {
    m_.clear();
    fs::path dir = util::roamingAppData() / L"FNBoost";
    std::error_code ec;
    fs::create_directories(dir, ec);
    file_ = dir / L"backup.tsv";
    std::ifstream in(file_);
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> f;
        std::stringstream ss(line);
        std::string c;
        while (std::getline(ss, c, '\t')) f.push_back(c);
        while (f.size() < 6) f.emplace_back();
        m_[atoi(f[0].c_str())].push_back({f[1], f[2], f[3], f[4] == "1", f[5]});
    }
}

void Backup::save() const {
    std::ofstream out(file_, std::ios::trunc);
    for (auto& [id, v] : m_)
        for (auto& s : v)
            out << id << '\t' << s.type << '\t' << s.loc << '\t' << s.name << '\t'
                << (s.existed ? "1" : "0") << '\t' << s.prev << '\n';
}

std::vector<Saved> Backup::take(int id) {
    auto it = m_.find(id);
    if (it == m_.end()) return {};
    auto v = std::move(it->second);
    m_.erase(it);
    return v;
}

namespace {

constexpr const char* C_INI = "Fortnite - GameUserSettings.ini";
constexpr const char* C_RES = "Resolução e tela esticada";
constexpr const char* C_WIN = "CPU, threads e energia";
constexpr const char* C_GPU = "GPU e pipeline gráfico";
constexpr const char* C_MEM = "Memória e disco";
constexpr const char* C_NET = "Rede e latência";
constexpr const char* C_ADV = "Avançado (driver/kernel) - simulado";

constexpr const char* FGS = "/Script/FortniteGame.FortGameUserSettings";
constexpr const char* SG  = "ScalabilityGroups";

std::string two(int n) { char b[16]; snprintf(b, sizeof(b), "%02d", n); return b; }

// ----------------------------- restauracao -----------------------------
bool restoreOne(const Saved& s) {
    if (s.type == "ini") {
        std::wstring p = widen(s.loc);
        auto bar = s.name.find('|');
        {
            std::wstring sec0 = widen(s.name.substr(0, bar)), key0 = widen(s.name.substr(bar + 1));
            DWORD at = GetFileAttributesW(p.c_str());
            bool ro = at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_READONLY);
            if (ro) SetFileAttributesW(p.c_str(), at & ~FILE_ATTRIBUTE_READONLY);
            BOOL okw = s.existed ? WritePrivateProfileStringW(sec0.c_str(), key0.c_str(), widen(s.prev).c_str(), p.c_str())
                                 : WritePrivateProfileStringW(sec0.c_str(), key0.c_str(), nullptr, p.c_str());
            if (ro) SetFileAttributesW(p.c_str(), at);
            return okw != FALSE;
        }
        std::wstring sec = widen(s.name.substr(0, bar)), key = widen(s.name.substr(bar + 1));
        return (s.existed ? WritePrivateProfileStringW(sec.c_str(), key.c_str(), widen(s.prev).c_str(), p.c_str())
                          : WritePrivateProfileStringW(sec.c_str(), key.c_str(), nullptr, p.c_str())) != FALSE;
    }
    if (s.type == "regd" || s.type == "regs") {
        auto slash = s.loc.find('\\');
        HKEY root = s.loc.substr(0, slash) == "HKCU" ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
        std::wstring path = widen(s.loc.substr(slash + 1)), name = widen(s.name);
        if (!s.existed) return util::regDeleteValue(root, path, name);
        return s.type == "regd" ? util::regSetDword(root, path, name, (DWORD)std::stoul(s.prev))
                                : util::regSetString(root, path, name, widen(s.prev));
    }
    if (s.type == "attr") {
        std::wstring p = widen(s.loc);
        DWORD at = GetFileAttributesW(p.c_str());
        if (at == INVALID_FILE_ATTRIBUTES) return false;
        return SetFileAttributesW(p.c_str(), s.prev == "1" ? (at | FILE_ATTRIBUTE_READONLY) : (at & ~FILE_ATTRIBUTE_READONLY)) != FALSE;
    }
    if (s.type == "disp") {   // prev = "largura,altura,hz"
        int w = 0, hh = 0, hz = 0;
        size_t a = s.prev.find(','), b = s.prev.find(',', a + 1);
        if (a == std::string::npos || b == std::string::npos) return false;
        w = std::stoi(s.prev.substr(0, a)); hh = std::stoi(s.prev.substr(a + 1, b - a - 1)); hz = std::stoi(s.prev.substr(b + 1));
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm)) return false;
        dm.dmPelsWidth = (DWORD)w; dm.dmPelsHeight = (DWORD)hh; dm.dmDisplayFrequency = (DWORD)hz;
        dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
        return ChangeDisplaySettingsExW(nullptr, &dm, nullptr, CDS_UPDATEREGISTRY, nullptr) == DISP_CHANGE_SUCCESSFUL;
    }
    if (s.type == "cmd") return util::run(widen(s.prev)) == 0;
    return true; // "none": acao de limpeza, nao ha o que desfazer
}

// ----------------------------- escrita com backup -----------------------------
std::string locStr(HKEY r, const std::wstring& p) {
    return std::string(r == HKEY_CURRENT_USER ? "HKCU" : "HKLM") + "\\" + narrow(p);
}

bool setDw(Ctx& c, int id, HKEY r, const std::wstring& p, const std::wstring& n, DWORD v) {
    auto prev = util::regGetDword(r, p, n);
    c.backup.push(id, {"regd", locStr(r, p), narrow(n), prev.has_value(), prev ? std::to_string(*prev) : std::string()});
    bool ok = util::regSetDword(r, p, n, v);
    if (!ok) Log::err("  falha ao escrever " + locStr(r, p) + "\\" + narrow(n));
    return ok;
}

bool setStr(Ctx& c, int id, HKEY r, const std::wstring& p, const std::wstring& n, const std::wstring& v) {
    auto prev = util::regGetString(r, p, n);
    c.backup.push(id, {"regs", locStr(r, p), narrow(n), prev.has_value(), prev ? narrow(*prev) : std::string()});
    bool ok = util::regSetString(r, p, n, v);
    if (!ok) Log::err("  falha ao escrever " + locStr(r, p) + "\\" + narrow(n));
    return ok;
}

bool setIni(Ctx& c, const Tweak& t, const std::string& sec, const std::string& key, const std::string& val) {
    std::wstring p = c.hw.iniPath.wstring(), ws = widen(sec), wk = widen(key);
    const wchar_t* sentinel = L"<<fnboost-missing>>";
    wchar_t buf[1024];
    GetPrivateProfileStringW(ws.c_str(), wk.c_str(), sentinel, buf, 1024, p.c_str());
    bool existed = wcscmp(buf, sentinel) != 0;
    c.backup.push(t.id, {"ini", narrow(p), sec + "|" + key, existed, existed ? narrow(buf) : std::string()});
    DWORD at = GetFileAttributesW(p.c_str());
    bool ro = at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_READONLY);
    if (ro) SetFileAttributesW(p.c_str(), at & ~FILE_ATTRIBUTE_READONLY);   // tweaks funcionam mesmo com o .ini travado
    BOOL ok = WritePrivateProfileStringW(ws.c_str(), wk.c_str(), widen(val).c_str(), p.c_str());
    if (ro) SetFileAttributesW(p.c_str(), at);
    if (!ok) Log::err("  falha ao gravar no .ini (somente leitura? Fortnite aberto?) erro " + std::to_string(GetLastError()));
    return ok != FALSE;
}

void markDone(Ctx& c, const Tweak& t) { c.backup.push(t.id, {"none", "", "", false, ""}); }

// ----------------------------- helpers de comando -----------------------------
std::string findGuid(const std::string& s) {
    for (size_t i = 0; i + 36 <= s.size(); ++i) {
        if (s[i + 8] == '-' && s[i + 13] == '-' && s[i + 18] == '-' && s[i + 23] == '-') {
            bool ok = true;
            for (int k = 0; k < 36 && ok; ++k) {
                char ch = s[i + k];
                if (k == 8 || k == 13 || k == 18 || k == 23) continue;
                ok = std::isxdigit((unsigned char)ch) != 0;
            }
            if (ok) return s.substr(i, 36);
        }
    }
    return {};
}

// Muda um valor de energia (CA) do plano atual e guarda o valor anterior.
// Le o penultimo "0x" da saida do powercfg (AC), o que independe do idioma do Windows.
bool powerValue(Ctx& c, const Tweak& t, const std::string& sub, const std::string& setting, int val) {
    std::string out;
    if (util::run(widen("powercfg /query SCHEME_CURRENT " + sub + " " + setting), &out) != 0) {
        Log::err("  powercfg /query falhou (configuração inexistente neste PC?)");
        return false;
    }
    std::vector<unsigned long> hx;
    for (size_t p = 0; (p = out.find("0x", p)) != std::string::npos; p += 2)
        hx.push_back(strtoul(out.c_str() + p, nullptr, 16));
    if (hx.size() < 2) { Log::err("  não consegui ler o valor atual"); return false; }
    unsigned long prev = hx[hx.size() - 2];
    std::string base = "powercfg /setacvalueindex SCHEME_CURRENT " + sub + " " + setting + " ";
    bool ok = util::run(widen(base + std::to_string(val) + " & powercfg /setactive SCHEME_CURRENT")) == 0;
    if (ok) c.backup.push(t.id, {"cmd", "", "", true, base + std::to_string(prev) + " & powercfg /setactive SCHEME_CURRENT"});
    return ok;
}

// Comando com desfazer explicito
bool cmdUndo(Ctx& c, const Tweak& t, const std::string& doCmd, const std::string& undoCmd) {
    std::string out;
    int r = util::run(widen(doCmd), &out);
    if (r != 0) Log::err("  comando falhou (" + std::to_string(r) + "): " + doCmd);
    else c.backup.push(t.id, {"cmd", "", "", true, undoCmd});
    return r == 0;
}

std::string psUndoWrap(const std::string& s) {
    return "powershell -NoProfile -ExecutionPolicy Bypass -Command \"" + s + "\"";
}

bool setDns(Ctx& c, const Tweak& t, const std::string& servers) {
    std::string out;
    util::powershell("(Get-NetAdapter | Where-Object Status -eq 'Up' | Sort-Object ifIndex | Select-Object -First 1).ifIndex", &out);
    std::string idx = util::trim(out);
    if (idx.empty() || !std::all_of(idx.begin(), idx.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
        Log::err("  nenhum adaptador de rede ativo encontrado");
        return false;
    }
    out.clear();
    util::powershell("(Get-DnsClientServerAddress -InterfaceIndex " + idx + " -AddressFamily IPv4).ServerAddresses -join ','", &out);
    std::string prev = util::trim(out);
    auto quote = [](const std::string& csv) {
        std::string r, a;
        std::stringstream ss(csv);
        while (std::getline(ss, a, ',')) { if (!r.empty()) r += ","; r += "'" + a + "'"; }
        return r;
    };
    std::string undo = prev.empty()
        ? "Set-DnsClientServerAddress -InterfaceIndex " + idx + " -ResetServerAddresses"
        : "Set-DnsClientServerAddress -InterfaceIndex " + idx + " -ServerAddresses (" + quote(prev) + ")";
    bool ok = util::powershell("Set-DnsClientServerAddress -InterfaceIndex " + idx + " -ServerAddresses (" + quote(servers) + ")") == 0;
    if (ok) c.backup.push(t.id, {"cmd", "", "", true, psUndoWrap(undo)});
    else Log::err("  Set-DnsClientServerAddress falhou");
    return ok;
}

// ----------------------------- limpeza -----------------------------
uintmax_t wipeDir(const fs::path& dir) {
    std::error_code ec;
    uintmax_t freed = 0;
    if (dir.empty() || !fs::exists(dir, ec)) return 0;
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        uintmax_t sz = 0;
        std::error_code e2;
        if (it->is_directory(e2)) {
            for (fs::recursive_directory_iterator r(it->path(), fs::directory_options::skip_permission_denied, e2), re;
                 !e2 && r != re; r.increment(e2)) {
                std::error_code e3;
                if (r->is_regular_file(e3)) { auto s = r->file_size(e3); if (!e3) sz += s; }
            }
        } else {
            sz = it->file_size(e2);
            if (e2) sz = 0;
        }
        std::error_code e4;
        fs::remove_all(it->path(), e4);
        if (!e4) freed += sz;
    }
    return freed;
}

int trimWorkingSets() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    int n = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID <= 4) continue;
            if (_wcsicmp(pe.szExeFile, L"FortniteClient-Win64-Shipping.exe") == 0) continue; // nunca mexe no jogo
            if (_wcsicmp(pe.szExeFile, L"dwm.exe") == 0) continue;
            HANDLE h = OpenProcess(PROCESS_SET_QUOTA | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (h) { if (EmptyWorkingSet(h)) ++n; CloseHandle(h); }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return n;
}

std::string mb(uintmax_t bytes) { char b[32]; snprintf(b, sizeof(b), "%.1f MB", (double)bytes / (1024.0 * 1024.0)); return b; }

void prepareIni(Ctx& c) {
    if (util::processRunning(L"FortniteClient-Win64-Shipping.exe"))
        Log::warn("O Fortnite está aberto: feche-o, ele pode sobrescrever o .ini ao sair.");
    if (c.iniBackedUp) return;
    std::error_code ec;
    fs::path dir = util::roamingAppData() / L"FNBoost" / L"backups";
    fs::create_directories(dir, ec);
    std::time_t tt = std::time(nullptr);
    std::tm tmv{};
    localtime_s(&tmv, &tt);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tmv);
    fs::path dst = dir / (std::wstring(L"GameUserSettings_") + widen(ts) + L".ini");
    fs::copy_file(c.hw.iniPath, dst, fs::copy_options::overwrite_existing, ec);
    if (!ec) { Log::info("Backup completo do .ini salvo em " + narrow(dst.wstring())); c.iniBackedUp = true; }
    else Log::warn("Não consegui copiar o .ini inteiro (" + ec.message() + "); backup por chave continua ativo.");
}

bool setDisplay(Ctx& c, const Tweak& t, int w, int hh) {
    DEVMODEW cur{};
    cur.dmSize = sizeof(cur);
    if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &cur)) return false;
    DEVMODEW dm = cur;
    dm.dmPelsWidth = (DWORD)w; dm.dmPelsHeight = (DWORD)hh;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
    LONG r = ChangeDisplaySettingsExW(nullptr, &dm, nullptr, CDS_UPDATEREGISTRY, nullptr);
    if (r != DISP_CHANGE_SUCCESSFUL) { Log::err("  o monitor/driver recusou esse modo (código " + std::to_string(r) + "); nada foi alterado"); return false; }
    c.backup.push(t.id, {"disp", "", "", true, std::to_string(cur.dmPelsWidth) + "," + std::to_string(cur.dmPelsHeight) + "," + std::to_string(cur.dmDisplayFrequency)});
    return true;
}

// ----------------------------- descricao de itens de registro -----------------------------
struct RI {
    HKEY root;
    std::wstring path, name;
    bool str = false;
    std::string fixed;   // se vazio, usa o valor escolhido pelo usuario
};
RI D(HKEY r, std::wstring p, std::wstring n, std::string fixed = "") { return RI{r, std::move(p), std::move(n), false, std::move(fixed)}; }
RI S(HKEY r, std::wstring p, std::wstring n, std::string fixed = "") { return RI{r, std::move(p), std::move(n), true, std::move(fixed)}; }

const HKEY HKCU_ = HKEY_CURRENT_USER;
const HKEY HKLM_ = HKEY_LOCAL_MACHINE;

} // namespace

// ---------------------------------------------------------------------------------------------
// Lista de tweaks
// ---------------------------------------------------------------------------------------------
std::vector<std::unique_ptr<Tweak>> buildTweaks(const HardwareInfo& h, const Backup& bk) {
    std::vector<std::unique_ptr<Tweak>> v;
    int id = 0;
    const int T = hardwareTier(h);

    auto add = [&](const char* cat, const char* name, const char* desc, Impl impl, bool admin, bool reboot,
                   std::vector<Opt> opts, int rec) -> Tweak& {
        auto t = std::make_unique<Tweak>();
        t->id = ++id; t->category = cat; t->name = name; t->desc = desc; t->impl = impl;
        t->needsAdmin = admin; t->needsReboot = reboot; t->options = std::move(opts);
        t->recommended = rec; t->choice = rec >= 0 ? rec : 0;
        v.push_back(std::move(t));
        return *v.back();
    };

    const std::vector<Opt> Q4 = {{"0 - Baixo", "0"}, {"1 - Médio", "1"}, {"2 - Alto", "2"}, {"3 - Épico", "3"}};
    const std::vector<Opt> BOOL = {{"Desligado", "False"}, {"Ligado", "True"}};
    const std::vector<Opt> ON_OFF = {{"Ligado", "1"}, {"Desligado", "0"}};

    auto ini = [&](const char* name, const char* desc, const char* sec, const char* key, std::vector<Opt> opts, int rec, int maxFps = -2) {
        Tweak& t = add(C_INI, name, desc, Impl::Real, false, false, std::move(opts), rec);
        t.usesIni = true; t.maxFps = maxFps;
        t.note = std::string("[") + sec + "] " + key + " (o nome da chave pode variar entre versões do jogo)";
        std::string s = sec, k = key;
        t.apply = [s, k](Ctx& c, const Tweak& tw, const std::string& val) { return setIni(c, tw, s, k, val); };
    };

    auto reg = [&](const char* cat, const char* name, const char* desc, bool admin, bool reboot,
                   std::vector<RI> items, std::vector<Opt> opts, int rec) {
        Tweak& t = add(cat, name, desc, Impl::Real, admin, reboot, std::move(opts), rec);
        t.apply = [items](Ctx& c, const Tweak& tw, const std::string& val) {
            bool ok = true;
            for (auto& i : items) {
                std::string x = i.fixed.empty() ? val : i.fixed;
                if (i.str) ok &= setStr(c, tw.id, i.root, i.path, i.name, widen(x));
                else       ok &= setDw(c, tw.id, i.root, i.path, i.name, (DWORD)std::stoul(x, nullptr, 0));
            }
            return ok;
        };
    };

    // =========================== 01-17  GameUserSettings.ini ===========================
    ini("Modo de renderização (RHI)", "DX11 é mais estável e leve; DX12 pode render mais em CPU/GPU modernas.", FGS, "PreferredRHI",
        {{"DirectX 11", "dx11"}, {"DirectX 12", "dx12"}}, (h.vramMB >= 8000 && h.modernGpu) ? 1 : 0, 0);
    ini("Modo Desempenho (feature level)", "ES31 = modo desempenho: menos qualidade visual, muito menos carga. Só para PCs fracos.", FGS, "PreferredFeatureLevel",
        {{"Modo Desempenho (ES31)", "es31"}, {"Padrão (SM5)", "sm5"}}, (h.vramMB < 4000 || h.ramTotalGB < 7.5) ? 0 : 1, 0);
    ini("Resolução 3D", "Renderiza o jogo em % da resolução da tela. Maior alavanca de FPS na GPU.", SG, "sg.ResolutionQuality",
        {{"50 %", "50.000000"}, {"67 %", "67.000000"}, {"75 %", "75.000000"}, {"85 %", "85.000000"}, {"100 %", "100.000000"}},
        T == 0 ? 2 : 4, 1);
    ini("Distância de visão", "Épico mostra construções e jogadores distantes. Custa CPU, mas ajuda no competitivo.", SG, "sg.ViewDistanceQuality", Q4, T == 0 ? 1 : 3, 1);
    ini("Sombras", "Sombras são pesadas na GPU e quase não ajudam na mira.", SG, "sg.ShadowQuality", Q4, 0);
    ini("Anti-aliasing", "Suaviza bordas. Baixo = imagem mais serrilhada e mais FPS.", SG, "sg.AntiAliasingQuality", Q4, T == 2 ? 1 : 0, 0);
    ini("Texturas", "Depende de VRAM. Não pesa em FPS se a VRAM sobra; trava se faltar.", SG, "sg.TextureQuality", Q4,
        h.vramMB >= 8000 ? 3 : (h.vramMB >= 4000 ? 2 : 1));
    ini("Efeitos", "Explosões, fumaça e partículas. Baixo reduz picos de frametime em lutas.", SG, "sg.EffectsQuality", Q4, 0);
    ini("Pós-processamento", "Bloom, DoF e afins. Baixo = imagem limpa e mais leve.", SG, "sg.PostProcessQuality", Q4, 0);
    ini("Folhagem", "Densidade de vegetação. Baixo ajuda a enxergar inimigos.", SG, "sg.FoliageQuality", Q4, 0);
    ini("Iluminação global", "Luz indireta. Custo alto na GPU.", SG, "sg.GlobalIlluminationQuality", Q4, 0);
    ini("Reflexos", "Reflexos em tempo real. Custo alto na GPU.", SG, "sg.ReflectionQuality", Q4, 0);
    ini("V-Sync", "Desligado reduz input lag (pode haver tearing).", FGS, "bUseVSync", BOOL, 0);
    ini("Motion blur", "Borrão de movimento atrapalha a mira.", FGS, "bMotionBlur", BOOL, 0);
    {
        int hz = h.refreshHz, rec = 0;
        const int opts[] = {60, 120, 144, 165, 240};
        if (T == 0) { rec = 4; for (int i = 0; i < 5; ++i) if (opts[i] >= hz) { rec = i + 1; break; } }
        ini("Limite de FPS", "Ilimitado dá o menor input lag se a CPU aguenta; em PC fraco, limitar ao refresh estabiliza o frametime.", FGS, "FrameRateLimit",
            {{"Ilimitado", "0.000000"}, {"60", "60.000000"}, {"120", "120.000000"}, {"144", "144.000000"}, {"165", "165.000000"}, {"240", "240.000000"}}, rec, 0);
    }
    ini("Mostrar grama", "Desligar remove a grama e melhora a visibilidade.", FGS, "bShowGrass", BOOL, 0);
    ini("Limite de FPS no menu", "Evita a GPU a 100% no lobby.", FGS, "MenuFrameRateLimit",
        {{"30", "30.000000"}, {"60", "60.000000"}, {"120", "120.000000"}}, 1);

    // =========================== 18-32  CPU, threads e energia ===========================
    {
        Tweak& t = add(C_WIN, "Plano de energia", "Alto desempenho evita a CPU baixar clock entre rajadas. Em notebook, mantenha Equilibrado.",
                       Impl::Real, false, false,
                       {{"Alto desempenho", "8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c"},
                        {"Desempenho máximo (Ultimate)", "e9a42b02-d5df-448d-aa00-03f14749eb61"},
                        {"Equilibrado", "381b4222-f694-41f0-9685-ff5bb260df2e"}},
                       h.isLaptop ? 2 : 0);
        t.apply = [](Ctx& c, const Tweak& tw, const std::string& guid) {
            std::string out;
            util::run(L"powercfg /getactivescheme", &out);
            std::string cur = findGuid(out);
            if (cur.empty()) { Log::err("  não consegui ler o plano ativo"); return false; }
            std::string target = guid;
            if (util::run(widen("powercfg /setactive " + target)) != 0) {
                out.clear();
                if (util::run(widen("powercfg /duplicatescheme " + target), &out) != 0) { Log::err("  plano não disponível"); return false; }
                target = findGuid(out);
                if (target.empty() || util::run(widen("powercfg /setactive " + target)) != 0) return false;
            }
            c.backup.push(tw.id, {"cmd", "", "", true, "powercfg /setactive " + cur});
            return true;
        };
    }
    reg(C_WIN, "Game Mode do Windows", "Prioriza o jogo em primeiro plano e segura atualizações em segundo plano.", false, false,
        {D(HKCU_, L"Software\\Microsoft\\GameBar", L"AutoGameModeEnabled"), D(HKCU_, L"Software\\Microsoft\\GameBar", L"AllowAutoGameMode")}, ON_OFF, 0);
    reg(C_WIN, "Xbox Game Bar", "Desligar remove overlay e hooks que o Game Bar injeta.", false, false,
        {D(HKCU_, L"Software\\Microsoft\\GameBar", L"UseNexusForGameBarEnabled"), D(HKCU_, L"Software\\Microsoft\\GameBar", L"ShowStartupPanel"),
         D(HKCU_, L"System\\GameConfigStore", L"GameDVR_Enabled")},
        {{"Desligado", "0"}, {"Ligado", "1"}}, 0);
    reg(C_WIN, "Captura em segundo plano (Game DVR)", "Gravação em background consome GPU/disco.", false, false,
        {D(HKCU_, L"Software\\Microsoft\\Windows\\CurrentVersion\\GameDVR", L"AppCaptureEnabled")}, {{"Desligado", "0"}, {"Ligado", "1"}}, 0);
    reg(C_WIN, "Otimizações de tela cheia (global)", "Faz o Windows respeitar tela cheia exclusiva em vez de janela sem borda otimizada.", false, false,
        {D(HKCU_, L"System\\GameConfigStore", L"GameDVR_FSEBehaviorMode", "2"), D(HKCU_, L"System\\GameConfigStore", L"GameDVR_HonorUserFSEBehaviorMode", "1"),
         D(HKCU_, L"System\\GameConfigStore", L"GameDVR_DXGIHonorFSEWindowsCompatible", "1"), D(HKCU_, L"System\\GameConfigStore", L"GameDVR_EFSEFeatureFlags", "0")},
        {{"Aplicar", "1"}}, 0);
    {
        Tweak& t = add(C_WIN, "Desativar otimizações de tela cheia (só Fortnite)", "Marca o executável do Fortnite com DISABLEDXMAXIMIZEDWINDOWEDMODE.",
                       Impl::Real, false, false, {{"Desativar", "1"}}, 0);
        t.usesExe = true;
        t.note = "HKCU\\...\\AppCompatFlags\\Layers = ~ DISABLEDXMAXIMIZEDWINDOWEDMODE";
        t.apply = [](Ctx& c, const Tweak& tw, const std::string&) {
            return setStr(c, tw.id, HKCU_, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers",
                          c.hw.fortniteExe.wstring(), L"~ DISABLEDXMAXIMIZEDWINDOWEDMODE");
        };
    }
    reg(C_WIN, "Prioridade de CPU do Fortnite", "Define a prioridade via Image File Execution Options (sem tocar no processo, compatível com o anti-cheat).", true, false,
        {D(HKLM_, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\FortniteClient-Win64-Shipping.exe\\PerfOptions", L"CpuPriorityClass")},
        {{"Alta", "3"}, {"Acima do normal", "6"}}, h.logCores >= 6 ? 0 : 1);
    reg(C_WIN, "Win32PrioritySeparation", "0x26 = fatias curtas e fixas com boost de primeiro plano (favorece o jogo).", true, false,
        {D(HKLM_, L"SYSTEM\\CurrentControlSet\\Control\\PriorityControl", L"Win32PrioritySeparation")},
        {{"0x26 (jogos)", "38"}, {"0x02 (padrão)", "2"}}, h.logCores <= 8 ? 0 : -1);
    reg(C_WIN, "MMCSS - perfil Games", "Dá mais prioridade de agendamento e GPU às threads multimídia/jogo.", true, false,
        {D(HKLM_, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile\\Tasks\\Games", L"GPU Priority", "8"),
         D(HKLM_, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile\\Tasks\\Games", L"Priority", "6"),
         S(HKLM_, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile\\Tasks\\Games", L"Scheduling Category", "High"),
         S(HKLM_, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile\\Tasks\\Games", L"SFIO Priority", "High")},
        {{"Aplicar", "1"}}, 0);
    reg(C_WIN, "SystemResponsiveness", "Reserva menos CPU para tarefas multimídia de fundo (10 = mínimo efetivo).", true, false,
        {D(HKLM_, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile", L"SystemResponsiveness")},
        {{"10 (jogos)", "10"}, {"20 (padrão)", "20"}}, 0);
    {
        Tweak& t = add(C_WIN, "Core parking desligado", "Mantém todos os núcleos acordados (menos latência de ativação). Só no plano atual, na tomada.",
                       Impl::Real, true, false, {{"100 % núcleos ativos", "100"}}, h.isLaptop ? -1 : 0);
        t.apply = [](Ctx& c, const Tweak& tw, const std::string& val) { return powerValue(c, tw, "SUB_PROCESSOR", "CPMINCORES", std::stoi(val)); };
    }
    {
        Tweak& t = add(C_WIN, "Suspensão seletiva de USB desligada", "Evita micro-travadas em mouse/teclado USB que 'dormem'.",
                       Impl::Real, true, false, {{"Desativada", "0"}}, 0);
        t.apply = [](Ctx& c, const Tweak& tw, const std::string& val) {
            return powerValue(c, tw, "2a737441-1930-4402-8d77-b2bebba308a3", "48e6b7a6-50f5-4782-a5d4-53bb8f07e226", std::stoi(val));
        };
    }
    {
        Tweak& t = add(C_WIN, "Fechar apps pesados de segundo plano", "Encerra OneDrive, Teams, Widgets e Phone Link agora. Reabra manualmente se precisar.",
                       Impl::Real, false, false, {{"Encerrar agora", "1"}}, h.ramTotalGB <= 8 ? 0 : -1);
        t.note = "Ação única, sem reversão automática.";
        t.apply = [](Ctx& c, const Tweak& tw, const std::string&) {
            for (const char* n : {"OneDrive.exe", "ms-teams.exe", "Teams.exe", "Widgets.exe", "PhoneExperienceHost.exe", "YourPhone.exe"})
                if (util::run(widen(std::string("taskkill /F /IM ") + n)) == 0) Log::info(std::string("  encerrado: ") + n);
            markDone(c, tw);
            return true;
        };
    }
    reg(C_WIN, "Apps em segundo plano", "Impede apps UWP de rodar em background.", false, false,
        {D(HKCU_, L"Software\\Microsoft\\Windows\\CurrentVersion\\BackgroundAccessApplications", L"GlobalUserDisabled")},
        {{"Desativar", "1"}, {"Permitir", "0"}}, 0);
    reg(C_WIN, "Transparência do Windows", "Desligar alivia a GPU integrada/fraca (efeito visual do sistema).", false, false,
        {D(HKCU_, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"EnableTransparency")},
        {{"Desligado", "0"}, {"Ligado", "1"}}, T == 0 ? 0 : -1);

    // =========================== 33-39  GPU e pipeline gráfico ===========================
    {
        reg(C_GPU, "Agendamento de GPU acelerado por hardware (HAGS)", "Move o agendamento de frames para a GPU. Ajuda em GPUs recentes; em antigas pode piorar.", true, true,
            {D(HKLM_, L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers", L"HwSchMode")},
            {{"Ligado", "2"}, {"Desligado", "1"}}, h.modernGpu ? 0 : -1);
    }
    {
        Tweak& t = add(C_GPU, "PCIe Link State Power Management desligado", "Impede a placa de vídeo de reduzir o link PCIe (picos de latência ao 'acordar').",
                       Impl::Real, true, false, {{"Desligado (0)", "0"}}, h.isLaptop ? -1 : 0);
        t.apply = [](Ctx& c, const Tweak& tw, const std::string& val) { return powerValue(c, tw, "SUB_PCIEXPRESS", "ASPM", std::stoi(val)); };
    }
    reg(C_GPU, "Otimizações para jogos em janela", "Habilita SwapEffectUpgrade (flip model) em jogos DX10/11 em janela/borderless.", false, false,
        {S(HKCU_, L"Software\\Microsoft\\DirectX\\UserGpuPreferences", L"DirectXUserGlobalSettings", "SwapEffectUpgradeEnable=1;")},
        {{"Ligado", "1"}}, 0);
    {
        Tweak& t = add(C_GPU, "GPU de alto desempenho para o Fortnite", "Força a GPU dedicada em notebooks híbridos (Configurações > Gráficos).",
                       Impl::Real, false, false, {{"Alto desempenho", "GpuPreference=2;"}, {"Economia", "GpuPreference=1;"}}, h.isLaptop ? 0 : -1);
        t.usesExe = true;
        t.apply = [](Ctx& c, const Tweak& tw, const std::string& val) {
            return setStr(c, tw.id, HKCU_, L"Software\\Microsoft\\DirectX\\UserGpuPreferences", c.hw.fortniteExe.wstring(), widen(val));
        };
    }
    {
        Tweak& t = add(C_GPU, "Limpar cache de shaders", "Apaga D3DSCache e caches NVIDIA/AMD. Corrige stutter após troca de driver; o 1º jogo depois recompila shaders.",
                       Impl::Real, false, false, {{"Limpar agora", "1"}}, -1);
        t.note = "Ação única, sem reversão (o cache se reconstrói sozinho).";
        t.apply = [](Ctx& c, const Tweak& tw, const std::string&) {
            fs::path la = util::localAppData();
            uintmax_t freed = 0;
            for (auto p : {la / L"D3DSCache", la / L"NVIDIA" / L"DXCache", la / L"NVIDIA" / L"GLCache", la / L"AMD" / L"DxCache"}) freed += wipeDir(p);
            Log::info("  liberado: " + mb(freed));
            markDone(c, tw);
            return true;
        };
    }
    {
        Tweak& t = add(C_GPU, "Baixa latência (NVIDIA Reflex / Anti-Lag)", "SIMULADO. Reflex/Anti-Lag são ligados no jogo ou no driver, não por registro.",
                       Impl::Simulated, false, false, {{"Ligado + Boost", "on"}}, h.modernGpu ? 0 : -1);
        t.note = "Fortnite > Vídeo > NVIDIA Reflex: Ligado + Boost (NVIDIA) ou Radeon Anti-Lag (AMD).";
    }
    {
        Tweak& t = add(C_GPU, "Energia máxima da GPU (vendor)", "SIMULADO. O modo 'prefer maximum performance' é do driver NVIDIA/AMD (painel de controle).",
                       Impl::Simulated, false, false, {{"Desempenho máximo", "max"}}, h.isLaptop ? -1 : 0);
        t.note = "Painel NVIDIA > Gerenciar configurações 3D > Modo de gerenciamento de energia: Preferir desempenho máximo (perfil do Fortnite).";
    }

    // =========================== 40-48  Memória e disco ===========================
    {
        Tweak& t = add(C_MEM, "Limpar working set de processos", "Pede ao Windows para devolver memória ociosa de apps em background (nunca do Fortnite). Ajuda com pouca RAM.",
                       Impl::Real, false, false, {{"Executar agora", "1"}}, h.ramTotalGB <= 8 ? 0 : -1);
        t.note = "Ação única. Com RAM de sobra o efeito é nulo.";
        t.apply = [](Ctx& c, const Tweak& tw, const std::string&) {
            Log::info("  processos ajustados: " + std::to_string(trimWorkingSets()));
            markDone(c, tw);
            return true;
        };
    }
    {
        Tweak& t = add(C_MEM, "Limpar arquivos temporários", "Remove o que não está em uso em %TEMP% e C:\\Windows\\Temp.",
                       Impl::Real, false, false, {{"Limpar agora", "1"}}, h.freeGB < 30 ? 0 : -1);
        t.note = "Ação única, sem reversão. Arquivos em uso são ignorados.";
        t.apply = [](Ctx& c, const Tweak& tw, const std::string&) {
            wchar_t tmp[MAX_PATH]{};
            GetTempPathW(MAX_PATH, tmp);
            uintmax_t freed = wipeDir(tmp);
            if (c.admin) freed += wipeDir(L"C:\\Windows\\Temp");
            Log::info("  liberado: " + mb(freed));
            markDone(c, tw);
            return true;
        };
    }
    {
        Tweak& t = add(C_MEM, "Limpar logs e crashes do Fortnite", "Apaga Saved\\Logs e Saved\\Crashes. Não toca em configurações.",
                       Impl::Real, false, false, {{"Limpar agora", "1"}}, -1);
        t.note = "Ação única, sem reversão.";
        t.apply = [](Ctx& c, const Tweak& tw, const std::string&) {
            fs::path s = util::localAppData() / L"FortniteGame" / L"Saved";
            uintmax_t freed = wipeDir(s / L"Logs") + wipeDir(s / L"Crashes");
            Log::info("  liberado: " + mb(freed));
            markDone(c, tw);
            return true;
        };
    }
    {
        Tweak& t = add(C_MEM, "Indexação do Windows (WSearch)", "Desligar para o indexador não competir por disco/CPU durante a partida.",
                       Impl::Real, true, false, {{"Desativar serviço", "off"}}, h.ramTotalGB <= 8 || !h.ssd ? 0 : -1);
        t.apply = [](Ctx& c, const Tweak& tw, const std::string&) {
            return cmdUndo(c, tw, "sc config WSearch start= disabled & sc stop WSearch", "sc config WSearch start= delayed-auto & sc start WSearch");
        };
    }
    {
        Tweak& t = add(C_MEM, "SysMain (Superfetch)", "Em SSD o pré-carregamento rende pouco; em HDD ele ajuda. Só desative em SSD.",
                       Impl::Real, true, false, {{"Desativar serviço", "off"}}, (h.ssd && h.ssdKnown) ? 0 : -1);
        t.apply = [](Ctx& c, const Tweak& tw, const std::string&) {
            return cmdUndo(c, tw, "sc config SysMain start= disabled & sc stop SysMain", "sc config SysMain start= auto & sc start SysMain");
        };
    }
    {
        Tweak& t = add(C_MEM, "Manutenção de SSD (retrim)", "Executa TRIM no disco do sistema para manter a escrita rápida. Só para SSD.",
                       Impl::Real, true, false, {{"Executar agora", "1"}}, (h.ssd && h.ssdKnown) ? 0 : -1);
        t.note = "Ação única. Pode levar alguns minutos.";
        t.apply = [](Ctx& c, const Tweak& tw, const std::string&) {
            if (!c.hw.ssd) { Log::err("  a unidade parece ser HDD; retrim ignorado"); return false; }
            bool ok = util::run(widen("defrag " + c.hw.gameDrive + " /L")) == 0;
            if (ok) markDone(c, tw);
            return ok;
        };
    }
    {
        Tweak& t = add(C_MEM, "NTFS: não gravar último acesso", "Evita uma escrita no disco a cada leitura de arquivo.",
                       Impl::Real, true, false, {{"Desativar last-access", "1"}}, 0);
        t.apply = [](Ctx& c, const Tweak& tw, const std::string&) {
            return cmdUndo(c, tw, "fsutil behavior set disablelastaccess 1", "fsutil behavior set disablelastaccess 2");
        };
    }
    {
        Tweak& t = add(C_MEM, "Compressão de memória", "Com 16 GB+ de RAM, desligar poupa CPU. Com pouca RAM, mantenha ligada.",
                       Impl::Real, true, true, {{"Desligar", "off"}}, h.ramTotalGB >= 15 ? 0 : -1);
        t.apply = [](Ctx& c, const Tweak& tw, const std::string&) {
            bool ok = util::powershell("Disable-MMAgent -MemoryCompression") == 0;
            if (ok) c.backup.push(tw.id, {"cmd", "", "", true, psUndoWrap("Enable-MMAgent -MemoryCompression")});
            return ok;
        };
    }
    {
        Tweak& t = add(C_MEM, "Hibernação", "Libera o hiberfil.sys (vários GB) e desativa a inicialização rápida. Útil se o disco está cheio.",
                       Impl::Real, true, false, {{"Desligar", "off"}}, h.freeGB < 40 ? 0 : -1);
        t.apply = [](Ctx& c, const Tweak& tw, const std::string&) { return cmdUndo(c, tw, "powercfg -h off", "powercfg -h on"); };
    }

    // =========================== 49-56  Rede e latência ===========================
    {
        Tweak& t = add(C_NET, "Limpar cache DNS", "Descarta resoluções antigas/ruins. Efeito pontual.", Impl::Real, false, false, {{"Executar agora", "1"}}, 0);
        t.note = "Ação única, sem reversão.";
        t.apply = [](Ctx& c, const Tweak& tw, const std::string&) {
            bool ok = util::run(L"ipconfig /flushdns") == 0;
            if (ok) markDone(c, tw);
            return ok;
        };
    }
    {
        Tweak& t = add(C_NET, "Servidor DNS", "Troca o DNS do adaptador ativo. Melhora a resolução de nomes (login/matchmaking), não a rota do jogo.",
                       Impl::Real, true, false,
                       {{"Cloudflare (1.1.1.1)", "1.1.1.1,1.0.0.1"}, {"Google (8.8.8.8)", "8.8.8.8,8.8.4.4"}, {"Quad9 (9.9.9.9)", "9.9.9.9,149.112.112.112"}}, 0);
        t.note = "O DNS anterior é salvo e restaurado ao reverter.";
        t.apply = [](Ctx& c, const Tweak& tw, const std::string& val) { return setDns(c, tw, val); };
    }
    reg(C_NET, "NetworkThrottlingIndex", "Remove o limite de pacotes/s do MMCSS (pensado para streaming de mídia).", true, false,
        {D(HKLM_, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile", L"NetworkThrottlingIndex")},
        {{"Sem limite", "4294967295"}, {"Padrão (10)", "10"}}, 0);
    {
        Tweak& t = add(C_NET, "TCP autotuning", "Garante o ajuste automático de janela TCP em 'normal'. Só corrige configurações mexidas por outros 'otimizadores'.",
                       Impl::Real, true, false, {{"normal", "normal"}, {"disabled", "disabled"}}, 0);
        t.note = "O desfazer volta para 'normal' (padrão do Windows).";
        t.apply = [](Ctx& c, const Tweak& tw, const std::string& val) {
            return cmdUndo(c, tw, "netsh int tcp set global autotuninglevel=" + val, "netsh int tcp set global autotuninglevel=normal");
        };
    }
    {
        Tweak& t = add(C_NET, "Nagle off (TcpAckFrequency/TCPNoDelay)", "Só afeta TCP e adaptadores com DHCP. O Fortnite joga em UDP, então o ganho prático é próximo de zero.",
                       Impl::Real, true, true, {{"Desativar Nagle", "1"}}, -1);
        t.apply = [](Ctx& c, const Tweak& tw, const std::string&) {
            const std::wstring base = L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces";
            bool ok = true;
            int n = 0;
            for (auto& g : util::regSubKeys(HKLM_, base)) {
                std::wstring p = base + L"\\" + g;
                auto ip = util::regGetString(HKLM_, p, L"DhcpIPAddress");
                if (!ip || ip->empty() || *ip == L"0.0.0.0") continue;
                ok &= setDw(c, tw.id, HKLM_, p, L"TcpAckFrequency", 1);
                ok &= setDw(c, tw.id, HKLM_, p, L"TCPNoDelay", 1);
                ++n;
            }
            if (!n) { Log::warn("  nenhuma interface DHCP ativa encontrada"); return false; }
            return ok;
        };
    }
    {
        Tweak& t = add(C_NET, "Economia de energia do adaptador", "Impede o Windows de desligar a placa de rede para poupar energia.",
                       Impl::Real, true, false, {{"Desativar economia", "1"}}, h.isLaptop ? -1 : 0);
        t.apply = [](Ctx& c, const Tweak& tw, const std::string&) {
            std::string cmd = "Get-NetAdapter -Physical | ForEach-Object { Set-NetAdapterPowerManagement -Name $_.Name -AllowComputerToTurnOffDevice %s -ErrorAction SilentlyContinue }";
            auto fmt = [&](const char* st) { char b[512]; snprintf(b, sizeof(b), cmd.c_str(), st); return std::string(b); };
            bool ok = util::powershell(fmt("Disabled")) == 0;
            if (ok) c.backup.push(tw.id, {"cmd", "", "", true, psUndoWrap(fmt("Enabled"))});
            return ok;
        };
    }
    {
        Tweak& t = add(C_NET, "Limpar tabela ARP", "Remove entradas ARP antigas do roteador/vizinhos. Efeito pontual.", Impl::Real, true, false, {{"Executar agora", "1"}}, -1);
        t.note = "Ação única, sem reversão.";
        t.apply = [](Ctx& c, const Tweak& tw, const std::string&) {
            bool ok = util::run(L"arp -d *") == 0;
            if (ok) markDone(c, tw);
            return ok;
        };
    }
    reg(C_NET, "Delivery Optimization (P2P)", "Impede o Windows de usar sua banda de upload para distribuir atualizações.", true, false,
        {D(HKLM_, L"SOFTWARE\\Policies\\Microsoft\\Windows\\DeliveryOptimization", L"DODownloadMode")},
        {{"Desativar P2P (0)", "0"}, {"Padrão da rede local (1)", "1"}}, 0);

    // =========================== 57-58  Avançado (simulado) ===========================
    {
        Tweak& t = add(C_ADV, "MSI mode e afinidade de interrupções", "SIMULADO. Exige alterar chaves por dispositivo (PCI) e reiniciar; risco de tela preta se errar.",
                       Impl::Simulated, true, true, {{"Orientação", "info"}}, -1);
        t.note = "Use uma ferramenta dedicada e crie ponto de restauração. O FNBoost não altera isso.";
    }
    {
        Tweak& t = add(C_ADV, "Timer resolution / standby list (kernel)", "SIMULADO. Depende de NtSetTimerResolution/NtSetSystemInformation e de privilégios de kernel; ganho incerto no Windows 10/11.",
                       Impl::Simulated, true, false, {{"Orientação", "info"}}, -1);
        t.note = "Não implementado de propósito: costuma piorar frametime e pode conflitar com o anti-cheat.";
    }

    // =========================== NOVOS: .ini extra ===========================
    ini("Aceleração do mouse (no jogo)", "Desliga a aceleração dentro do Fortnite: movimento 1:1, mira mais consistente.", FGS, "bDisableMouseAcceleration",
        {{"Desativada", "True"}, {"Padrão", "False"}}, 0);
    ini("Qualidade de sombreamento", "Custo dos shaders de material. Baixo = mais FPS.", SG, "sg.ShadingQuality", Q4, 0, 0);
    {
        Tweak& t = add(C_INI, "Travar .ini como somente leitura", "Impede o Fortnite de sobrescrever suas configurações ao sair. Os tweaks do FNBoost continuam funcionando; mudanças feitas no menu do jogo não serão salvas.",
                       Impl::Real, false, false, {{"Travar", "1"}, {"Destravar", "0"}}, -1);
        t.usesIni = true;
        t.apply = [](Ctx& c, const Tweak& tw, const std::string& val) {
            std::wstring p = c.hw.iniPath.wstring();
            DWORD at = GetFileAttributesW(p.c_str());
            if (at == INVALID_FILE_ATTRIBUTES) return false;
            c.backup.push(tw.id, {"attr", narrow(p), "", true, (at & FILE_ATTRIBUTE_READONLY) ? "1" : "0"});
            return SetFileAttributesW(p.c_str(), val == "1" ? (at | FILE_ATTRIBUTE_READONLY) : (at & ~FILE_ATTRIBUTE_READONLY)) != FALSE;
        };
    }

    // =========================== NOVOS: resolução e tela esticada ===========================
    {
        struct Res { int w, hh; };
        const int SW = h.screenW, SH = h.screenH;
        const Res cands[] = {{SW, SH}, {1920, 1080}, {1680, 1050}, {1600, 1200}, {1440, 1080}, {1400, 1050},
                             {1280, 1024}, {1280, 960}, {1152, 864}, {1024, 768}};
        auto aspect = [](int w, int hh) -> std::string {
            double r = (double)w / hh;
            if (std::abs(r - 16.0 / 9) < 0.02) return "16:9";
            if (std::abs(r - 1.6) < 0.02) return "16:10";
            if (std::abs(r - 4.0 / 3) < 0.02) return "4:3";
            if (std::abs(r - 1.25) < 0.02) return "5:4";
            return "outra";
        };
        const std::string nativeAsp = aspect(SW, SH);
        std::vector<Opt> gameOpts, deskOpts;
        std::vector<std::pair<int, int>> seen;
        int recGame = 0;
        for (const Res& r : cands) {
            if (r.w > SW || r.hh > SH) continue;
            if (std::find(seen.begin(), seen.end(), std::make_pair(r.w, r.hh)) != seen.end()) continue;
            seen.push_back({r.w, r.hh});
            const std::string asp = aspect(r.w, r.hh);
            char val[32], lab[96];
            snprintf(val, sizeof(val), "%dx%d", r.w, r.hh);
            snprintf(lab, sizeof(lab), "%s%dx%d (%s)%s", seen.size() == 1 ? "Nativa " : "", r.w, r.hh, asp.c_str(),
                     (seen.size() > 1 && asp != nativeAsp) ? " - esticada" : "");
            gameOpts.push_back({lab, val});
            if (T <= 1 && nativeAsp == "16:9" && r.w == 1440 && r.hh == 1080) recGame = (int)gameOpts.size() - 1;
            bool supported = seen.size() == 1;
            for (auto& m : h.modes) if (m.first == r.w && m.second == r.hh) supported = true;
            if (supported) deskOpts.push_back({lab, val});
        }
        auto parseWH = [](const std::string& v, int& w, int& hh) {
            auto xp = v.find('x');
            if (xp == std::string::npos) return false;
            w = std::stoi(v.substr(0, xp)); hh = std::stoi(v.substr(xp + 1));
            return true;
        };

        Tweak& tr = add(C_RES, "Resolução do jogo (inclui esticada 4:3 / 16:10)",
                        "Esticada = menos pixels (mais FPS) e modelos mais largos. Recomendado: 1440x1080 em PC básico/intermediário com monitor 16:9.",
                        Impl::Real, false, false, gameOpts, recGame);
        tr.usesIni = true;
        tr.note = "Grava ResolutionSizeX/Y, LastUserConfirmedResolutionSizeX/Y e DesiredScreenWidth/Height. Para a imagem encher a tela, use também o dimensionamento por GPU.";
        tr.apply = [parseWH](Ctx& c, const Tweak& tw, const std::string& val) {
            int w = 0, hh = 0;
            if (!parseWH(val, w, hh)) return false;
            const std::string W = std::to_string(w), H = std::to_string(hh);
            bool ok = true;
            for (const char* k : {"ResolutionSizeX", "LastUserConfirmedResolutionSizeX", "DesiredScreenWidth", "LastUserConfirmedDesiredScreenWidth"}) ok &= setIni(c, tw, FGS, k, W);
            for (const char* k : {"ResolutionSizeY", "LastUserConfirmedResolutionSizeY", "DesiredScreenHeight", "LastUserConfirmedDesiredScreenHeight"}) ok &= setIni(c, tw, FGS, k, H);
            return ok;
        };

        Tweak& tf = add(C_RES, "Modo de tela", "Tela cheia exclusiva dá menos latência e é a que respeita resolução esticada de verdade.",
                        Impl::Real, false, false, {{"Tela cheia exclusiva", "0"}, {"Janela sem borda", "1"}, {"Janela", "2"}}, 0);
        tf.usesIni = true;
        tf.apply = [](Ctx& c, const Tweak& tw, const std::string& val) {
            bool ok = true;
            for (const char* k : {"FullscreenMode", "LastConfirmedFullscreenMode", "PreferredFullscreenMode"}) ok &= setIni(c, tw, FGS, k, val);
            return ok;
        };

        Tweak& td = add(C_RES, "Resolução do Windows (área de trabalho)",
                        "Troca a resolução do desktop. É o jeito mais garantido de ter a esticada em tela cheia. Só aparecem modos que o seu monitor/driver expõem.",
                        Impl::Real, false, false, deskOpts, -1);
        td.note = "Reverter restaura a resolução anterior. Se o monitor recusar o modo, nada é alterado.";
        td.apply = [parseWH](Ctx& c, const Tweak& tw, const std::string& val) {
            int w = 0, hh = 0;
            return parseWH(val, w, hh) && setDisplay(c, tw, w, hh);
        };

        Tweak& ts = add(C_RES, "Dimensionamento por GPU (preencher a tela)", "SIMULADO. Sem isso, a resolução esticada aparece com barras pretas. É configurado no painel do driver.",
                        Impl::Simulated, false, false, {{"Tela cheia / GPU", "gpu"}}, 0);
        ts.note = "NVIDIA: Painel > Vídeo > Ajustar tamanho e posição da área de trabalho > Tela cheia, 'Realizar dimensionamento em: GPU' e marque 'Substituir o modo de dimensionamento definido por jogos'. AMD: Adrenalin > Tela > Escala de GPU ligada, modo 'Painel inteiro'.";
        Tweak& tc = add(C_RES, "Criar resolução personalizada (1440x1080, 1280x960...)", "SIMULADO. Se a resolução esticada não aparece na lista do Windows, ela precisa ser criada no driver.",
                        Impl::Simulated, false, false, {{"Orientação", "info"}}, 0);
        tc.note = "NVIDIA: Painel > Alterar resolução > Personalizar > Criar resolução personalizada. AMD: Adrenalin > Tela > Resoluções personalizadas. Alternativa: ferramenta CRU. Depois selecione-a aqui.";
    }

    // =========================== NOVOS: CPU, energia, input e serviços ===========================
    reg(C_WIN, "Precisão do ponteiro desligada", "Desliga a aceleração do ponteiro do Windows (movimento 1:1). Vale após novo login.", false, false,
        {S(HKCU_, L"Control Panel\\Mouse", L"MouseSpeed", "0"), S(HKCU_, L"Control Panel\\Mouse", L"MouseThreshold1", "0"),
         S(HKCU_, L"Control Panel\\Mouse", L"MouseThreshold2", "0")}, {{"Desligada", "0"}}, 0);
    reg(C_WIN, "Sensibilidade do ponteiro 6/11", "Valor neutro do Windows, sem multiplicar o movimento do mouse. Vale após novo login.", false, false,
        {S(HKCU_, L"Control Panel\\Mouse", L"MouseSensitivity", "10")}, {{"6/11 (neutro)", "10"}}, 0);
    reg(C_WIN, "Teclas de aderência / alternância / filtro", "Impede o popup do Shift 5x (Sticky Keys) que minimiza o jogo no meio da partida.", false, false,
        {S(HKCU_, L"Control Panel\\Accessibility\\StickyKeys", L"Flags", "506"), S(HKCU_, L"Control Panel\\Accessibility\\ToggleKeys", L"Flags", "58"),
         S(HKCU_, L"Control Panel\\Accessibility\\Keyboard Response", L"Flags", "122")}, {{"Desativar atalhos", "1"}}, 0);
    reg(C_WIN, "Notificações do Windows", "Evita toasts por cima do jogo e as pequenas travadas que causam.", false, false,
        {D(HKCU_, L"Software\\Microsoft\\Windows\\CurrentVersion\\PushNotifications", L"ToastEnabled")}, {{"Desligadas", "0"}, {"Ligadas", "1"}}, 0);
    reg(C_WIN, "Power Throttling desligado", "Impede o Windows de limitar a CPU de processos 'em segundo plano', incluindo launchers.", true, false,
        {D(HKLM_, L"SYSTEM\\CurrentControlSet\\Control\\Power\\PowerThrottling", L"PowerThrottlingOff")}, {{"Desligado", "1"}, {"Padrão", "0"}}, h.isLaptop ? -1 : 0);
    reg(C_WIN, "Inicialização rápida desligada", "Faz o 'Desligar' ser completo: limpa memória e estado de drivers (menos bugs de latência após dias ligado).", true, false,
        {D(HKLM_, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power", L"HiberbootEnabled")}, {{"Desligada", "0"}, {"Ligada", "1"}}, 0);
    {
        auto pw = [&](const char* name, const char* desc, const char* sub, const char* set, int val, int rec) {
            Tweak& t = add(C_WIN, name, desc, Impl::Real, true, false, {{"Aplicar", std::to_string(val)}}, rec);
            std::string s = sub, k = set;
            t.apply = [s, k](Ctx& c, const Tweak& tw, const std::string& v) { return powerValue(c, tw, s, k, std::stoi(v)); };
        };
        pw("Boost agressivo da CPU", "Faz a CPU subir clock mais rápido sob carga. Mais calor.", "SUB_PROCESSOR", "PERFBOOSTMODE", 2, h.isLaptop ? -1 : 0);
        pw("Estado mínimo do processador = 100 %", "Mantém o clock alto o tempo todo: menos latência de 'acordar', mais consumo.", "SUB_PROCESSOR", "PROCTHROTTLEMIN", 100, h.isLaptop ? -1 : 0);
        pw("Resfriamento ativo", "Prefere acelerar a ventoinha a reduzir o clock.", "SUB_PROCESSOR", "SYSCOOLPOL", 1, h.isLaptop ? -1 : 0);
    }
    {
        Tweak& t = add(C_WIN, "Telemetria do Windows (DiagTrack)", "Desliga o serviço de telemetria (menos disco/CPU/rede em segundo plano).",
                       Impl::Real, true, false, {{"Desativar serviço", "off"}}, 0);
        t.apply = [](Ctx& c, const Tweak& tw, const std::string&) {
            return cmdUndo(c, tw, "sc config DiagTrack start= disabled & sc stop DiagTrack", "sc config DiagTrack start= auto & sc start DiagTrack");
        };
    }
    reg(C_WIN, "Prioridade de I/O do Fortnite", "Prioriza leituras/gravações de disco do jogo (carregamento de texturas, menos stutter).", true, false,
        {D(HKLM_, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\FortniteClient-Win64-Shipping.exe\\PerfOptions", L"IoPriority")},
        {{"Alta (3)", "3"}, {"Normal (2)", "2"}}, 0);
    {
        Tweak& t = add(C_WIN, "Tick dinâmico desligado (bcdedit)", "Contestado: pode reduzir latência de timer em alguns PCs e piorar em outros. Teste e compare.",
                       Impl::Real, true, true, {{"Desligar tick dinâmico", "1"}}, -1);
        t.apply = [](Ctx& c, const Tweak& tw, const std::string&) {
            return cmdUndo(c, tw, "bcdedit /set disabledynamictick yes", "bcdedit /deletevalue disabledynamictick");
        };
    }
    reg(C_WIN, "Integridade de memória (HVCI)", "ATENÇÃO: desligar reduz a proteção contra malware no kernel. Dá ganho em algumas CPUs. Só faça se entende o risco.", true, true,
        {D(HKLM_, L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity", L"Enabled")},
        {{"Desligar (menos seguro)", "0"}, {"Ligar", "1"}}, -1);
    {
        Tweak& t = add(C_WIN, "Exclusão do Defender para a pasta do Fortnite", "Reduz varredura em tempo real nos arquivos do jogo. Troca segurança por menos I/O: só se você confia na instalação.",
                       Impl::Real, true, false, {{"Adicionar exclusão", "1"}}, -1);
        t.usesExe = true;
        t.apply = [](Ctx& c, const Tweak& tw, const std::string&) {
            fs::path p = c.hw.fortniteExe;
            for (int i = 0; i < 4; ++i) p = p.parent_path();   // ...\Fortnite
            std::string d = narrow(p.wstring());
            bool ok = util::powershell("Add-MpPreference -ExclusionPath '" + d + "'") == 0;
            if (ok) c.backup.push(tw.id, {"cmd", "", "", true, psUndoWrap("Remove-MpPreference -ExclusionPath '" + d + "'")});
            return ok;
        };
    }

    // =========================== NOVOS: GPU ===========================
    reg(C_GPU, "Multi-Plane Overlay desligado", "Só vale se você tem flicker, stutter ou tela preta com overlays (NVIDIA/Discord/OBS).", true, true,
        {D(HKLM_, L"SOFTWARE\\Microsoft\\Windows\\Dwm", L"OverlayTestMode")}, {{"Desligar MPO", "5"}}, -1);

    // =========================== NOVOS: memória e disco ===========================
    {
        Tweak& t = add(C_MEM, "Limpar cache do Windows Update", "Apaga downloads antigos de atualização (SoftwareDistribution\\Download) e libera GBs.", Impl::Real, true, false,
                       {{"Limpar agora", "1"}}, h.freeGB < 40 ? 0 : -1);
        t.note = "Ação única. Os serviços wuauserv/bits são parados e religados.";
        t.apply = [](Ctx& c, const Tweak& tw, const std::string&) {
            util::run(L"net stop wuauserv & net stop bits");
            uintmax_t f = wipeDir(L"C:\\Windows\\SoftwareDistribution\\Download");
            util::run(L"net start bits & net start wuauserv");
            Log::info("  liberado: " + mb(f));
            markDone(c, tw);
            return true;
        };
    }
    {
        Tweak& t = add(C_MEM, "Esvaziar a lixeira", "Libera espaço ocupado por arquivos já apagados.", Impl::Real, false, false, {{"Esvaziar agora", "1"}}, -1);
        t.note = "Ação única, irreversível.";
        t.apply = [](Ctx& c, const Tweak& tw, const std::string&) {
            HRESULT r = SHEmptyRecycleBinW(nullptr, nullptr, SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND);
            if (SUCCEEDED(r)) markDone(c, tw);
            return SUCCEEDED(r);
        };
    }
    {
        Tweak& t = add(C_MEM, "Limpar cache web do Epic Games Launcher", "Remove o webcache do launcher (resolve launcher lento/travado). Feche o launcher antes.", Impl::Real, false, false, {{"Limpar agora", "1"}}, -1);
        t.note = "Ação única.";
        t.apply = [](Ctx& c, const Tweak& tw, const std::string&) {
            fs::path s = util::localAppData() / L"EpicGamesLauncher" / L"Saved";
            std::error_code ec;
            uintmax_t f = 0;
            for (fs::directory_iterator it(s, ec), e; !ec && it != e; it.increment(ec))
                if (it->path().filename().wstring().rfind(L"webcache", 0) == 0) f += wipeDir(it->path());
            Log::info("  liberado: " + mb(f));
            markDone(c, tw);
            return true;
        };
    }

    // =========================== NOVOS: rede ===========================
    reg(C_NET, "Preferir IPv4 sobre IPv6", "Ajuda se o seu IPv6 é lento ou quebrado (login/matchmaking demorando).", true, true,
        {D(HKLM_, L"SYSTEM\\CurrentControlSet\\Services\\Tcpip6\\Parameters", L"DisabledComponents")}, {{"Preferir IPv4 (0x20)", "32"}, {"Padrão (0)", "0"}}, -1);
    {
        Tweak& t = add(C_NET, "Reset de Winsock e pilha IP", "Conserta pilha de rede corrompida (por VPN/otimizadores antigos). Exige reiniciar.", Impl::Real, true, true, {{"Resetar agora", "1"}}, -1);
        t.note = "Ação única, sem reversão.";
        t.apply = [](Ctx& c, const Tweak& tw, const std::string&) {
            bool ok = util::run(L"netsh winsock reset & netsh int ip reset") == 0;
            if (ok) markDone(c, tw);
            return ok;
        };
    }
    {
        Tweak& t = add(C_NET, "Large Send Offload desligado", "Alguns adaptadores geram micro-picos de latência com LSO. Teste e compare; pode reduzir throughput de download.", Impl::Real, true, false, {{"Desligar LSO", "1"}}, -1);
        t.apply = [](Ctx& c, const Tweak& tw, const std::string&) {
            bool ok = util::powershell("Disable-NetAdapterLso -Name '*' -ErrorAction SilentlyContinue") == 0;
            if (ok) c.backup.push(tw.id, {"cmd", "", "", true, psUndoWrap("Enable-NetAdapterLso -Name '*' -ErrorAction SilentlyContinue")});
            return ok;
        };
    }

    // =========================== ordena por categoria e renumera ===========================
    {
        const std::vector<std::string> order = {C_INI, C_RES, C_WIN, C_GPU, C_MEM, C_NET, C_ADV};
        auto rank = [&](const std::string& c) { for (size_t i = 0; i < order.size(); ++i) if (order[i] == c) return (int)i; return 99; };
        std::stable_sort(v.begin(), v.end(), [&](const std::unique_ptr<Tweak>& a, const std::unique_ptr<Tweak>& b) { return rank(a->category) < rank(b->category); });
        for (size_t i = 0; i < v.size(); ++i) v[i]->id = (int)i + 1;
    }

    for (auto& t : v) t->applied = bk.has(t->id);
    return v;
}

// ---------------------------------------------------------------------------------------------
// Aplicar / reverter
// ---------------------------------------------------------------------------------------------
bool applyTweak(Ctx& c, Tweak& t) {
    std::string tag = "#" + two(t.id) + " " + t.name;
    if (t.impl == Impl::Simulated) {
        Log::warn("[SIMULADO] " + tag + ": " + t.note);
        return true;
    }
    if (t.needsAdmin && !c.admin) { Log::err(tag + ": requer administrador."); return false; }
    if (t.usesIni && !c.hw.iniFound) { Log::err(tag + ": GameUserSettings.ini não encontrado."); return false; }
    if (t.usesExe && !c.hw.exeFound) { Log::err(tag + ": executável do Fortnite não encontrado."); return false; }
    if (t.options.empty() || !t.apply) return false;

    if (t.usesIni) prepareIni(c);
    const Opt& o = t.options[(size_t)std::clamp(t.choice, 0, (int)t.options.size() - 1)];
    Log::info("Aplicando " + tag + " -> " + o.label);
    bool ok = false;
    try { ok = t.apply(c, t, o.value); }
    catch (const std::exception& e) { Log::err(std::string("  exceção: ") + e.what()); }
    t.applied = c.backup.has(t.id);
    c.backup.save();
    if (ok) { Log::ok(tag + ": concluído."); if (t.needsReboot) Log::warn("  reinicie o PC para efeito completo."); }
    else Log::err(tag + ": falhou (use Reverter para desfazer o que foi alterado).");
    return ok;
}

bool revertTweak(Ctx& c, Tweak& t) {
    std::string tag = "#" + two(t.id) + " " + t.name;
    if (t.impl == Impl::Simulated) return true;
    if (t.needsAdmin && !c.admin) { Log::err(tag + ": reverter requer administrador."); return false; }
    auto items = c.backup.take(t.id);
    if (items.empty()) { Log::info(tag + ": nada para reverter."); t.applied = false; return true; }
    bool ok = true, onlyNone = true;
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
        if (it->type != "none") onlyNone = false;
        ok &= restoreOne(*it);
    }
    t.applied = false;
    c.backup.save();
    if (onlyNone) Log::info(tag + ": era uma ação única, não há o que desfazer.");
    else if (ok) Log::ok(tag + ": revertido.");
    else Log::err(tag + ": reversão parcial (veja o log).");
    return ok;
}

int applyRecommended(Ctx& c, std::vector<std::unique_ptr<Tweak>>& v) {
    int n = 0;
    for (auto& t : v) {
        if (t->impl != Impl::Real || t->recommended < 0 || t->applied) continue;
        if (t->usesIni && !c.hw.iniFound) continue;
        if (t->usesExe && !c.hw.exeFound) continue;
        if (t->needsAdmin && !c.admin) { Log::warn("Pulando #" + two(t->id) + " (requer administrador)."); continue; }
        t->choice = t->recommended;
        if (applyTweak(c, *t)) ++n;
    }
    Log::ok("Recomendados aplicados: " + std::to_string(n));
    return n;
}

int revertAll(Ctx& c, std::vector<std::unique_ptr<Tweak>>& v) {
    int n = 0;
    for (auto it = v.rbegin(); it != v.rend(); ++it)
        if ((*it)->applied && revertTweak(c, **it)) ++n;
    Log::ok("Tweaks revertidos: " + std::to_string(n));
    return n;
}

// Preset: false = Competitivo (valores recomendados), true = FPS Maximo (qualidade minima nos itens com maxFps definido)
int applyPreset(Ctx& c, std::vector<std::unique_ptr<Tweak>>& v, bool maxFps) {
    int n = 0;
    for (auto& t : v) {
        if (t->impl != Impl::Real) continue;
        int idx = t->recommended;
        if (maxFps && t->maxFps >= 0) idx = t->maxFps;
        if (idx < 0) continue;
        if (t->applied && !t->usesIni) continue;                 // acoes de sistema nao repetem
        if (t->usesIni && !c.hw.iniFound) continue;
        if (t->usesExe && !c.hw.exeFound) continue;
        if (t->needsAdmin && !c.admin) { Log::warn("Pulando #" + two(t->id) + " " + t->name + " (requer administrador)."); continue; }
        t->choice = idx;
        if (applyTweak(c, *t)) ++n;
    }
    Log::ok(std::string("Preset ") + (maxFps ? "FPS Máximo" : "Competitivo") + " concluído: " + std::to_string(n) + " tweaks aplicados.");
    return n;
}
