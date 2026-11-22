#pragma once
#include "hardware.hpp"
#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

enum class Impl { Real, Simulated };   // Real = altera .ini/registro/comandos; Simulated = so orienta (exige driver/kernel)

struct Opt { std::string label, value; };

// Uma linha de backup: o que existia antes do tweak, para poder reverter.
struct Saved {
    std::string type;   // ini | regd | regs | cmd | none
    std::string loc;    // caminho do .ini, "HKCU\\chave" ou vazio
    std::string name;   // "secao|chave", nome do valor ou vazio
    bool existed = false;
    std::string prev;   // valor antigo (ou comando de desfazer, para type=cmd)
};

class Backup {
public:
    void load();
    void save() const;
    void push(int id, Saved s) { m_[id].push_back(std::move(s)); }
    std::vector<Saved> take(int id);
    bool has(int id) const { auto it = m_.find(id); return it != m_.end() && !it->second.empty(); }
private:
    std::map<int, std::vector<Saved>> m_;
    std::filesystem::path file_;
};

struct Ctx {
    HardwareInfo hw;
    Backup backup;
    bool admin = false;
    bool iniBackedUp = false;
};

struct Tweak {
    int id = 0;
    std::string category, name, desc, note;
    Impl impl = Impl::Real;
    bool needsAdmin = false, needsReboot = false;
    bool usesIni = false, usesExe = false;
    std::vector<Opt> options;
    int recommended = -1;            // indice em options; -1 = nao recomendado para este PC
    int choice = 0;                  // escolha atual do usuario
    int maxFps = -2;                 // escolha no preset "FPS Maximo" (-2 = usa a recomendada)
    std::atomic<bool> applied{false};
    std::function<bool(Ctx&, const Tweak&, const std::string&)> apply;
};

std::vector<std::unique_ptr<Tweak>> buildTweaks(const HardwareInfo& hw, const Backup& bk);
bool applyTweak(Ctx& c, Tweak& t);
bool revertTweak(Ctx& c, Tweak& t);
int  applyRecommended(Ctx& c, std::vector<std::unique_ptr<Tweak>>& v);
int  revertAll(Ctx& c, std::vector<std::unique_ptr<Tweak>>& v);
int  applyPreset(Ctx& c, std::vector<std::unique_ptr<Tweak>>& v, bool maxFps); // false = Competitivo, true = FPS Maximo
