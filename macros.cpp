#include "macros.hpp"
#include "util.hpp"
#include <mmsystem.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

bool vkIsMouse(int vk) {
    return vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
}

static bool isExtended(int vk) {
    switch (vk) {
    case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT: case VK_INSERT: case VK_DELETE:
    case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT: case VK_RCONTROL: case VK_RMENU:
    case VK_DIVIDE: case VK_NUMLOCK:
        return true;
    default: return false;
    }
}

std::string vkName(int vk) {
    switch (vk) {
    case 0: return "(nenhuma)";
    case VK_LBUTTON: return "Mouse esquerdo";
    case VK_RBUTTON: return "Mouse direito";
    case VK_MBUTTON: return "Mouse meio";
    case VK_XBUTTON1: return "Mouse lateral 1";
    case VK_XBUTTON2: return "Mouse lateral 2";
    default: break;
    }
    UINT sc = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    LONG lp = (LONG)(sc << 16);
    if (isExtended(vk)) lp |= (1 << 24);
    wchar_t b[64]{};
    if (sc && GetKeyNameTextW(lp, b, 64) > 0) return util::narrow(b);
    char f[24];
    snprintf(f, sizeof(f), "VK 0x%02X", vk);
    return f;
}

static double nowMs() {
    static const LARGE_INTEGER freq = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
}

static bool isDown(int vk) { return vk > 0 && (GetAsyncKeyState(vk) & 0x8000) != 0; }

// ------------------------------------------------------------------------------------------
void MacroEngine::start() {
    if (th_.joinable()) return;
    stop_ = false;
    th_ = std::thread([this] { loop(); });
}

void MacroEngine::stop() {
    stop_ = true;
    if (th_.joinable()) th_.join();
    releaseAll();
}

MacroSettings MacroEngine::get() { std::lock_guard<std::mutex> g(mtx_); return s_; }

void MacroEngine::set(const MacroSettings& s) {
    {
        std::lock_guard<std::mutex> g(mtx_);
        s_ = s;
    }
    killVk_ = s.killVk;
    holdMs_ = s.holdMs;
    gameFps_ = std::max(30, s.gameFps);
    sync_ = s.syncFps;
}

void MacroEngine::killNow(const char* why) {
    if (!killed_.exchange(true)) Log::warn(std::string("KILL SWITCH acionado (") + why + "): macros desativadas, teclas soltas.");
}

void MacroEngine::testRun(int idx, int delayMs) {
    if (killed_) { Log::warn("Rearme o Kill Switch antes de testar."); return; }
    pendingAt_ = nowMs() + delayMs;
    pending_ = idx;
    Log::info("Teste da macro em " + std::to_string(delayMs / 1000) + " s: troque para o jogo/Criativo agora.");
}

std::string MacroEngine::status() {
    std::lock_guard<std::mutex> g(mtx_);
    if (killed_) return "KILL SWITCH ativo - rearme para voltar a usar";
    if (pending_ >= 0) return "Teste agendado...";
    return active_.empty() ? "Pronto (aguardando tecla de disparo)" : "Executando: " + active_;
}

double MacroEngine::frameMs() const { return 1000.0 / (double)std::max(30, gameFps_.load()); }
double MacroEngine::eff(double ms) const { return sync_ ? std::max(ms, frameMs()) : ms; }
double MacroEngine::effHoldMs() const {
    double h = (double)holdMs_.load();
    return sync_ ? std::max(h, std::ceil(frameMs() * 1.5)) : h;
}

// ------------------------------------------------------------------------------------------
bool MacroEngine::aborted() {
    if (stop_ || killed_) return true;
    if (isDown(killVk_)) { killNow("tecla de emergência"); return true; }
    return false;
}

bool MacroEngine::wait(double ms) {
    const double t0 = nowMs();
    for (;;) {
        if (aborted()) return false;
        double left = ms - (nowMs() - t0);
        if (left <= 0) return true;
        if (left > 2.0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        else YieldProcessor();
    }
}

void MacroEngine::sendKey(int vk, bool down) {
    INPUT in{};
    if (vkIsMouse(vk)) {
        in.type = INPUT_MOUSE;
        switch (vk) {
        case VK_LBUTTON: in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; break;
        case VK_RBUTTON: in.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP; break;
        case VK_MBUTTON: in.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
        case VK_XBUTTON1: in.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP; in.mi.mouseData = XBUTTON1; break;
        case VK_XBUTTON2: in.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP; in.mi.mouseData = XBUTTON2; break;
        }
    } else {
        in.type = INPUT_KEYBOARD;
        WORD sc = (WORD)MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
        if (sc) {
            in.ki.wScan = sc;
            in.ki.dwFlags = KEYEVENTF_SCANCODE | (isExtended(vk) ? KEYEVENTF_EXTENDEDKEY : 0);
        } else {
            in.ki.wVk = (WORD)vk;
        }
        if (!down) in.ki.dwFlags |= KEYEVENTF_KEYUP;
    }
    SendInput(1, &in, sizeof(INPUT));
    if (down) held_.insert(vk); else held_.erase(vk);
}

bool MacroEngine::tap(int vk) {
    if (vk <= 0) return true;
    sendKey(vk, true);
    bool ok = wait(effHoldMs());   // >= 1,5 frame: o jogo precisa ver o "down"
    sendKey(vk, false);            // sempre solta, mesmo se abortou
    return ok;
}

void MacroEngine::moveRel(int dx, int dy) {
    if (!dx && !dy) return;
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof(INPUT));
}

void MacroEngine::releaseAll() {
    std::set<int> copy = held_;
    for (int vk : copy) sendKey(vk, false);
    held_.clear();
}

// ------------------------------------------------------------------------------------------
void MacroEngine::runGhost(const GhostCfg& g) {
    const int reps = std::max(1, g.repeats);
    for (int r = 0; r < reps; ++r) {
        if (!tap(g.keyA) || !wait(eff(g.delayMs)) || !tap(g.keyB)) return;
        if (g.returnToA && (!wait(eff(g.delayMs)) || !tap(g.keyA))) return;
        if (r + 1 < reps && !wait(eff(g.delayMs))) return;
    }
}

void MacroEngine::runDoubleEdit(const DoubleEditCfg& d) {
    auto edit = [&]() {
        return tap(d.editKey) && wait(eff(d.dEdit)) && tap(d.selectKey) && wait(eff(d.dSelect)) &&
               tap(d.confirmKey) && wait(eff(d.dConfirm));
    };
    if (!edit()) return;
    if (!tap(d.resetKey) || !wait(eff(d.dReset))) return;
    edit();
}

void MacroEngine::runDrag(const DragCfg& d) {
    // 1) entra no modo de edicao e espera o jogo processar
    if (d.useEdit) {
        if (!tap(d.editKey) || !wait(eff(d.preDelayMs))) return;
    }
    // 2) aperta o botao e espera o jogo ver o "down" antes de mover (causa do "as vezes nao edita")
    if (d.holdLeft) {
        sendKey(VK_LBUTTON, true);
        if (!wait(std::max(eff(d.settleMs), effHoldMs()))) { releaseAll(); return; }
    }
    // 3) movimento em passos; cada passo dura no minimo 1 frame, senao o jogo junta tudo num frame so
    const double fm = sync_ ? frameMs() : 1.0;
    int steps = std::max(1, d.steps);
    if (sync_) steps = std::max(1, std::min(steps, (int)std::floor((double)d.durationMs / fm)));
    const double per = std::max(fm, (double)d.durationMs / steps);
    int px = 0, py = 0;
    bool ok = true;
    for (int i = 1; i <= steps && ok; ++i) {
        int tx = (int)std::lround((double)d.dx * i / steps);
        int ty = (int)std::lround((double)d.dy * i / steps);
        moveRel(tx - px, ty - py);
        px = tx; py = ty;
        ok = wait(per);
    }
    // 4) fica parado com o botao ainda pressionado por >= 1 frame, depois solta
    if (ok) ok = wait(std::max(eff(d.endHoldMs), effHoldMs()));
    if (d.holdLeft) sendKey(VK_LBUTTON, false);
    if (!ok) return;
    // 5) confirmacao opcional
    if (d.confirmAfter) {
        if (!wait(eff(d.confirmDelayMs))) return;
        tap(d.confirmKey);
    }
}

void MacroEngine::runIdx(int i, const MacroSettings& s) {
    static const char* names[3] = {"Ghost bind", "Double edit", "Drag"};
    { std::lock_guard<std::mutex> g(mtx_); active_ = names[i]; }
    if (i == 0) runGhost(s.ghost);
    else if (i == 1) runDoubleEdit(s.dedit);
    else runDrag(s.drag);
    releaseAll();
    { std::lock_guard<std::mutex> g(mtx_); active_.clear(); }
}

// ------------------------------------------------------------------------------------------
void MacroEngine::loop() {
    timeBeginPeriod(1);
    bool prev[3] = {false, false, false};
    while (!stop_) {
        MacroSettings s = get();
        killVk_ = s.killVk;

        if (!killed_ && isDown(s.killVk)) killNow("tecla de emergência");
        if (killed_) {
            pending_ = -1;
            if (!held_.empty()) releaseAll();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        int p = pending_.load();
        if (p >= 0 && nowMs() >= pendingAt_.load()) {
            pending_ = -1;
            runIdx(p, s);
            continue;
        }

        const int trig[3] = {s.ghost.trigger, s.dedit.trigger, s.drag.trigger};
        const bool en[3] = {s.ghost.enabled, s.dedit.enabled, s.drag.enabled};
        for (int i = 0; i < 3 && !killed_ && !stop_; ++i) {
            bool d = isDown(trig[i]);
            bool fire = en[i] && d && !prev[i] && trig[i] != s.killVk;
            prev[i] = d;
            if (!fire) continue;
            runIdx(i, s);
            prev[i] = isDown(trig[i]);   // se ainda estiver segurando, exige soltar antes de disparar de novo
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    releaseAll();
    timeEndPeriod(1);
}
