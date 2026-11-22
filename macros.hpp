#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <thread>

// Por que macros falham: o jogo so le o input UMA vez por frame. Se uma tecla fica pressionada
// por menos que a duracao de um frame (8 ms = so 125 FPS), o jogo pode nem ver o clique.
// Por isso todos os tempos abaixo sao "esticados" para no minimo 1 frame (e o hold para 1,5 frame)
// quando "Sincronizar com o FPS do jogo" esta ligado.

// 1) Ghost bind: toca A -> espera -> toca B (opcional: volta para A), repetindo N vezes.
struct GhostCfg {
    bool enabled = false;
    int trigger = VK_XBUTTON1;
    int keyA = '1', keyB = '2';
    int delayMs = 15;
    bool returnToA = false;
    int repeats = 1;
};

// 2) Double edit: editar -> selecionar -> confirmar -> resetar -> editar -> selecionar -> confirmar.
struct DoubleEditCfg {
    bool enabled = false;
    int trigger = VK_XBUTTON2;
    int editKey = 'G', selectKey = VK_LBUTTON, confirmKey = VK_LBUTTON, resetKey = VK_RBUTTON;
    int dEdit = 15, dSelect = 15, dConfirm = 15, dReset = 15;
};

// 3) Drag edit: [tecla de editar] -> espera -> segura LMB -> espera "assentar" -> move em passos -> segura no fim -> solta
//    -> [confirmar]. Os atrasos de assentar/fim garantem que o jogo veja o clique ANTES e DEPOIS do movimento.
struct DragCfg {
    bool enabled = false;
    int trigger = VK_MBUTTON;
    bool useEdit = true;
    int editKey = 'G';
    int preDelayMs = 30;       // depois de apertar editar
    int settleMs = 25;         // depois de apertar o botao e antes de mover
    int dx = 0, dy = 120;
    int durationMs = 80;
    int steps = 8;
    int endHoldMs = 25;        // parado no fim, ainda com o botao pressionado
    bool holdLeft = true;
    bool confirmAfter = false;
    int confirmKey = VK_LBUTTON;
    int confirmDelayMs = 20;
};

struct MacroSettings {
    GhostCfg ghost;
    DoubleEditCfg dedit;
    DragCfg drag;
    int killVk = VK_F12;       // Kill Switch (global)
    int holdMs = 16;           // quanto cada tecla fica pressionada em um "tap"
    int gameFps = 144;         // FPS que voce faz no jogo (para sincronizar)
    bool syncFps = true;       // garante >= 1 frame entre as acoes
};

std::string vkName(int vk);
bool vkIsMouse(int vk);

class MacroEngine {
public:
    ~MacroEngine() { stop(); }
    void start();
    void stop();
    MacroSettings get();
    void set(const MacroSettings& s);
    void killNow(const char* why = "botão da interface");
    void rearm() { killed_ = false; }
    bool killed() const { return killed_; }
    std::string status();
    void testRun(int idx, int delayMs = 3000);   // 0 ghost, 1 double edit, 2 drag (ignora "ativado")
    double effHoldMs() const;
    double frameMs() const;

private:
    void loop();
    bool aborted();
    bool wait(double ms);
    double eff(double ms) const;
    void sendKey(int vk, bool down);
    bool tap(int vk);
    void moveRel(int dx, int dy);
    void releaseAll();
    void runIdx(int i, const MacroSettings& s);
    void runGhost(const GhostCfg& g);
    void runDoubleEdit(const DoubleEditCfg& d);
    void runDrag(const DragCfg& d);

    std::thread th_;
    std::atomic<bool> stop_{false}, killed_{false}, sync_{true};
    std::atomic<int> killVk_{VK_F12}, holdMs_{16}, gameFps_{144};
    std::atomic<int> pending_{-1};
    std::atomic<double> pendingAt_{0};
    std::mutex mtx_;
    MacroSettings s_;
    std::string active_;
    std::set<int> held_;   // so a thread do motor mexe aqui
};
