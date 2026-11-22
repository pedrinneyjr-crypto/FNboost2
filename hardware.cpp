#include "hardware.hpp"
#include "util.hpp"
#include <dxgi.h>
#include <wrl/client.h>
#include <intrin.h>
#include <winioctl.h>
#include <algorithm>
#include <bitset>
#include <cstdio>
#include <cstring>
#include <vector>

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

namespace {

std::string cpuBrand() {
    int r[4];
    char b[49]{};
    __cpuid(r, (int)0x80000000u);
    if ((unsigned)r[0] < 0x80000004u) return "CPU desconhecida";
    for (int i = 0; i < 3; ++i) {
        __cpuid(r, (int)(0x80000002u + (unsigned)i));
        std::memcpy(b + 16 * i, r, 16);
    }
    return util::trim(b);
}

void coreCounts(int& phys, int& logi) {
    DWORD len = 0;
    GetLogicalProcessorInformation(nullptr, &len);
    std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> buf(len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
    phys = logi = 0;
    if (len && GetLogicalProcessorInformation(buf.data(), &len)) {
        for (auto& i : buf)
            if (i.Relationship == RelationProcessorCore) {
                ++phys;
                logi += (int)std::bitset<64>((unsigned long long)i.ProcessorMask).count();
            }
    }
    if (!phys) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        phys = logi = (int)si.dwNumberOfProcessors;
    }
}

bool containsCI(const std::string& hay, const char* needle) {
    std::string h = hay, n = needle;
    for (auto& c : h) c = (char)toupper((unsigned char)c);
    for (auto& c : n) c = (char)toupper((unsigned char)c);
    return h.find(n) != std::string::npos;
}

void detectGpu(HardwareInfo& h) {
    ComPtr<IDXGIFactory1> f;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) { h.gpuName = "GPU nao detectada"; return; }
    SIZE_T best = 0;
    bool any = false;
    ComPtr<IDXGIAdapter1> a;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && (!any || d.DedicatedVideoMemory > best)) {
            any = true;
            best = d.DedicatedVideoMemory;
            h.gpuName = util::narrow(d.Description);
            h.vramMB = (uint64_t)(d.DedicatedVideoMemory / (1024 * 1024));
            h.gpuVendor = d.VendorId == 0x10DE ? "NVIDIA" : d.VendorId == 0x1002 ? "AMD"
                        : d.VendorId == 0x8086 ? "Intel" : "Outro";
        }
        a.Reset();
    }
    const std::string& n = h.gpuName;
    h.modernGpu = containsCI(n, "RTX") || containsCI(n, "GTX 16") || containsCI(n, "RX 5") ||
                  containsCI(n, "RX 6") || containsCI(n, "RX 7") || containsCI(n, "RX 9") ||
                  containsCI(n, "ARC");
}

bool detectSsd(wchar_t drive, bool& known) {
    known = false;
    wchar_t path[] = L"\\\\.\\C:";
    path[4] = drive;
    HANDLE hd = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hd == INVALID_HANDLE_VALUE) return true;
    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageDeviceSeekPenaltyProperty;
    q.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR d{};
    DWORD ret = 0;
    BOOL ok = DeviceIoControl(hd, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), &d, sizeof(d), &ret, nullptr);
    CloseHandle(hd);
    if (!ok) return true;
    known = true;
    return !d.IncursSeekPenalty;
}

std::string osName() {
    const std::wstring key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    std::string s = util::narrow(util::regGetString(HKEY_LOCAL_MACHINE, key, L"ProductName").value_or(L"Windows"));
    int build = _wtoi(util::regGetString(HKEY_LOCAL_MACHINE, key, L"CurrentBuildNumber").value_or(L"0").c_str());
    if (build >= 22000 && s.rfind("Windows 10", 0) == 0) s.replace(0, 10, "Windows 11");
    std::string dv = util::narrow(util::regGetString(HKEY_LOCAL_MACHINE, key, L"DisplayVersion").value_or(L""));
    char b[64];
    snprintf(b, sizeof(b), " (build %d)", build);
    return s + (dv.empty() ? "" : " " + dv) + b;
}

} // namespace

void setIniPath(HardwareInfo& h, const fs::path& p) {
    h.iniPath = p;
    std::error_code ec;
    h.iniFound = fs::is_regular_file(p, ec);
}

int hardwareTier(const HardwareInfo& h) {
    int s = 0;
    if (h.vramMB >= 8000) s += 2; else if (h.vramMB >= 4000) s += 1;
    if (h.logCores >= 12) s += 2; else if (h.logCores >= 6) s += 1;
    if (h.ramTotalGB >= 15) s += 1;
    if (h.modernGpu) s += 1;
    return s >= 5 ? 2 : (s >= 3 ? 1 : 0);
}

HardwareInfo detectHardware() {
    HardwareInfo h;
    h.cpuName = cpuBrand();
    coreCounts(h.physCores, h.logCores);
    detectGpu(h);

    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);
    h.ramTotalGB = (double)ms.ullTotalPhys / (1024.0 * 1024.0 * 1024.0);

    h.osName = osName();

    fs::path la = util::localAppData();
    std::wstring root = la.root_name().wstring();
    wchar_t letter = root.empty() ? L'C' : root[0];
    h.gameDrive = util::narrow(std::wstring(1, letter) + L":");
    ULARGE_INTEGER avail{}, total{}, freeB{};
    std::wstring rootPath = std::wstring(1, letter) + L":\\";
    if (GetDiskFreeSpaceExW(rootPath.c_str(), &avail, &total, &freeB)) {
        h.freeGB = (double)avail.QuadPart / (1024.0 * 1024.0 * 1024.0);
        h.totalGB = (double)total.QuadPart / (1024.0 * 1024.0 * 1024.0);
    }
    h.ssd = detectSsd(letter, h.ssdKnown);

    SYSTEM_POWER_STATUS sps{};
    if (GetSystemPowerStatus(&sps)) h.isLaptop = sps.BatteryFlag != 128 && sps.BatteryFlag != 255;

    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm)) {
        if (dm.dmDisplayFrequency > 1) h.refreshHz = (int)dm.dmDisplayFrequency;
        if (dm.dmPelsWidth > 0) { h.screenW = (int)dm.dmPelsWidth; h.screenH = (int)dm.dmPelsHeight; }
    }
    {
        DEVMODEW m{};
        m.dmSize = sizeof(m);
        for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &m); ++i) {
            if (m.dmBitsPerPel != 32 || (int)m.dmDisplayFrequency != h.refreshHz) continue;
            std::pair<int, int> p{(int)m.dmPelsWidth, (int)m.dmPelsHeight};
            if (std::find(h.modes.begin(), h.modes.end(), p) == h.modes.end()) h.modes.push_back(p);
        }
    }

    setIniPath(h, la / L"FortniteGame" / L"Saved" / L"Config" / L"WindowsClient" / L"GameUserSettings.ini");

    // Procura o executavel do Fortnite nos HDs/SSDs fixos (instalacao padrao da Epic)
    const wchar_t* rels[] = {
        L"Program Files\\Epic Games\\Fortnite\\FortniteGame\\Binaries\\Win64\\FortniteClient-Win64-Shipping.exe",
        L"Epic Games\\Fortnite\\FortniteGame\\Binaries\\Win64\\FortniteClient-Win64-Shipping.exe",
        L"Games\\Epic Games\\Fortnite\\FortniteGame\\Binaries\\Win64\\FortniteClient-Win64-Shipping.exe"};
    for (wchar_t d = L'A'; d <= L'Z' && !h.exeFound; ++d) {
        std::wstring r = std::wstring(1, d) + L":\\";
        if (GetDriveTypeW(r.c_str()) != DRIVE_FIXED) continue;
        for (auto rel : rels) {
            fs::path p = fs::path(r) / rel;
            std::error_code ec;
            if (fs::exists(p, ec)) { h.fortniteExe = p; h.exeFound = true; break; }
        }
    }
    return h;
}

LiveStats pollLive() {
    static ULONGLONG pi = 0, pk = 0, pu = 0;
    auto q = [](const FILETIME& f) { ULARGE_INTEGER x; x.LowPart = f.dwLowDateTime; x.HighPart = f.dwHighDateTime; return x.QuadPart; };
    FILETIME i, k, u;
    LiveStats s;
    if (GetSystemTimes(&i, &k, &u)) {
        ULONGLONG I = q(i), K = q(k), U = q(u);
        if (pk) {
            ULONGLONG di = I - pi, tot = (K - pk) + (U - pu);
            if (tot) s.cpu = 100.0 * (double)(tot - di) / (double)tot;
        }
        pi = I; pk = K; pu = U;
    }
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        s.ramUsedPct = (double)ms.dwMemoryLoad;
        s.ramUsedGB = (double)(ms.ullTotalPhys - ms.ullAvailPhys) / (1024.0 * 1024.0 * 1024.0);
    }
    return s;
}
