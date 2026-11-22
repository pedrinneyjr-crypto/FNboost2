#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

struct HardwareInfo {
    // CPU
    std::string cpuName;
    int physCores = 0, logCores = 0;
    // GPU
    std::string gpuName, gpuVendor;
    uint64_t vramMB = 0;
    bool modernGpu = false;      // RTX / GTX 16 / RX 5000+ / Arc
    // RAM
    double ramTotalGB = 0;
    // Sistema / disco
    std::string osName;
    std::string gameDrive;       // ex: "C:"
    double freeGB = 0, totalGB = 0;
    bool ssd = true, ssdKnown = false;
    bool isLaptop = false;
    int refreshHz = 60;
    int screenW = 1920, screenH = 1080;
    std::vector<std::pair<int, int>> modes;   // resolucoes suportadas na taxa atual
    // Fortnite
    std::filesystem::path iniPath;  bool iniFound = false;
    std::filesystem::path fortniteExe; bool exeFound = false;
};

struct LiveStats { double cpu = 0; double ramUsedPct = 0; double ramUsedGB = 0; };

HardwareInfo detectHardware();
LiveStats    pollLive();          // chamar ~1x por segundo
void         setIniPath(HardwareInfo& h, const std::filesystem::path& p);
int          hardwareTier(const HardwareInfo& h); // 0 fraco, 1 medio, 2 forte
