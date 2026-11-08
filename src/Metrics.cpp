#include "Metrics.h"

namespace dock {

namespace {

constexpr size_t kHistory = 60;

struct ProcessorPowerInfo {
    ULONG Number;
    ULONG MaxMhz;
    ULONG CurrentMhz;
    ULONG MhzLimit;
    ULONG MaxIdleState;
    ULONG CurrentIdleState;
};

void PushBack(std::deque<float>& q, float v) {
    q.push_back(v);
    while (q.size() > kHistory) q.pop_front();
}

}  // namespace

bool Metrics::Init() {
    // Prime the CPU counters so the first delta is meaningful.
    FILETIME idle{}, kernel{}, user{};
    if (GetSystemTimes(&idle, &kernel, &user)) {
        lastCpuIdle_ = (static_cast<ULONGLONG>(idle.dwHighDateTime) << 32) | idle.dwLowDateTime;
        lastCpuKernel_ =
            (static_cast<ULONGLONG>(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime;
        lastCpuUser_ = (static_cast<ULONGLONG>(user.dwHighDateTime) << 32) | user.dwLowDateTime;
    }

    if (PDH_CSTATUS_VALID_DATA == PdhOpenQueryW(nullptr, 0, &gpuQuery_)) {
        const wchar_t* path = L"\\GPU Engine(*engtype_3D)\\Utilization Percentage";
        if (PdhAddEnglishCounterW(gpuQuery_, path, 0, &gpuCounter_) != ERROR_SUCCESS &&
            PdhAddCounterW(gpuQuery_, path, 0, &gpuCounter_) != ERROR_SUCCESS) {
            gpuCounter_ = nullptr;
        }
        PdhCollectQueryData(gpuQuery_);
        gpuAvailable_ = gpuCounter_ != nullptr;
    }

    SampleMemory();
    return true;
}

void Metrics::Shutdown() {
    if (gpuQuery_) {
        PdhCloseQuery(gpuQuery_);
        gpuQuery_ = nullptr;
        gpuCounter_ = nullptr;
    }
}

void Metrics::SampleCpu() {
    FILETIME idle{}, kernel{}, user{};
    if (!GetSystemTimes(&idle, &kernel, &user)) return;

    const ULONGLONG nowIdle = (static_cast<ULONGLONG>(idle.dwHighDateTime) << 32) | idle.dwLowDateTime;
    const ULONGLONG nowKernel =
        (static_cast<ULONGLONG>(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime;
    const ULONGLONG nowUser =
        (static_cast<ULONGLONG>(user.dwHighDateTime) << 32) | user.dwLowDateTime;

    const ULONGLONG dIdle = nowIdle - lastCpuIdle_;
    const ULONGLONG dKernel = nowKernel - lastCpuKernel_;
    const ULONGLONG dUser = nowUser - lastCpuUser_;

    lastCpuIdle_ = nowIdle;
    lastCpuKernel_ = nowKernel;
    lastCpuUser_ = nowUser;

    // Kernel time already includes idle time.
    const ULONGLONG total = dKernel + dUser;
    if (total > 0) {
        const double busy = static_cast<double>(total - dIdle);
        current_.cpuPercent = ClampF(static_cast<float>(busy / static_cast<double>(total) * 100.0),
                                     0.0f, 100.0f);
    }

    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const DWORD cores = std::max<DWORD>(1, si.dwNumberOfProcessors);

    std::vector<ProcessorPowerInfo> info(cores);
    ULONG bytes = static_cast<ULONG>(info.size() * sizeof(ProcessorPowerInfo));
    if (CallNtPowerInformation(ProcessorInformation, nullptr, 0, info.data(), bytes) == 0) {
        double sumMhz = 0.0;
        DWORD counted = 0;
        for (const auto& p : info) {
            if (p.CurrentMhz == 0) continue;
            sumMhz += static_cast<double>(p.CurrentMhz);
            ++counted;
        }
        // Average core clock, not the sum across cores.
        if (counted > 0) current_.cpuGHz = static_cast<float>(sumMhz / counted / 1000.0);
    }
}

void Metrics::SampleMemory() {
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms)) return;

    const double totalGB = static_cast<double>(ms.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0);
    const double availGB = static_cast<double>(ms.ullAvailPhys) / (1024.0 * 1024.0 * 1024.0);
    current_.ramTotalGB = static_cast<float>(totalGB);
    current_.ramUsedGB = static_cast<float>(totalGB - availGB);
    current_.ramPercent = ClampF(static_cast<float>(ms.dwMemoryLoad), 0.0f, 100.0f);
}

void Metrics::SampleGpu() {
    if (!gpuAvailable_ || !gpuQuery_) return;
    if (PdhCollectQueryData(gpuQuery_) != ERROR_SUCCESS) return;

    DWORD bufferSize = 0;
    DWORD itemCount = 0;
    if (static_cast<DWORD>(PdhGetFormattedCounterArrayW(gpuCounter_, PDH_FMT_DOUBLE, &bufferSize,
                                                        &itemCount, nullptr)) != PDH_MORE_DATA) {
        return;
    }
    std::vector<BYTE> buffer(bufferSize + sizeof(PDH_FMT_COUNTERVALUE_ITEM_W));
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
    if (PdhGetFormattedCounterArrayW(gpuCounter_, PDH_FMT_DOUBLE, &bufferSize, &itemCount, items) !=
        ERROR_SUCCESS) {
        return;
    }

    double sum = 0.0;
    for (DWORD i = 0; i < itemCount; ++i) {
        if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA ||
            items[i].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA) {
            sum += items[i].FmtValue.doubleValue;
        }
    }
    current_.gpuPercent = ClampF(static_cast<float>(sum), 0.0f, 100.0f);
    current_.gpuValid = true;
}

void Metrics::SampleNetwork() {
    MIB_IF_TABLE2* table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR || !table) return;

    double in = 0.0, out = 0.0;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IF_ROW2& row = table->Table[i];
        if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK || row.Type == IF_TYPE_TUNNEL) continue;
        if (row.OperStatus != IfOperStatusUp) continue;
        if (row.InOctets == 0 && row.OutOctets == 0) continue;
        in += static_cast<double>(row.InOctets);
        out += static_cast<double>(row.OutOctets);
    }
    FreeMibTable(table);

    const ULONGLONG now = GetTickCount64();
    if (lastNetIn_ >= 0.0 && lastNetTick_ != 0) {
        const double dt = static_cast<double>(now - lastNetTick_) / 1000.0;
        if (dt > 0.05) {
            const double dIn = in - lastNetIn_;
            const double dOut = out - lastNetOut_;
            current_.netInBytesPerSec = dIn >= 0.0 ? dIn / dt : 0.0;
            current_.netOutBytesPerSec = dOut >= 0.0 ? dOut / dt : 0.0;
        }
    }
    lastNetIn_ = in;
    lastNetOut_ = out;
    lastNetTick_ = now;
}

void Metrics::PushHistory() {
    PushBack(cpuHistory_, current_.cpuPercent);
    PushBack(ramHistory_, current_.ramPercent);
    PushBack(gpuHistory_, current_.gpuPercent);
    // Network is scaled by a soft ceiling so the graph stays readable.
    constexpr float kCeiling = 12.5f * 1024.0f * 1024.0f;  // 12.5 MB/s ~= 100 Mbps
    PushBack(netInHistory_, ClampF(static_cast<float>(current_.netInBytesPerSec / kCeiling * 100.0), 0.0f, 100.0f));
    PushBack(netOutHistory_, ClampF(static_cast<float>(current_.netOutBytesPerSec / kCeiling * 100.0), 0.0f, 100.0f));
}

void Metrics::Poll() {
    const ULONGLONG now = GetTickCount64();
    if (lastPollTick_ != 0 && now - lastPollTick_ < 1000) return;
    const bool first = (lastPollTick_ == 0);
    lastPollTick_ = now;

    previous_ = current_;
    SampleCpu();
    SampleMemory();
    SampleGpu();
    SampleNetwork();
    if (first) {
        // Avoid a bogus spike from zero-initialized history.
        current_.netInBytesPerSec = 0.0;
        current_.netOutBytesPerSec = 0.0;
    }
    PushHistory();
}

const std::deque<float>& Metrics::History(Series s) const {
    switch (s) {
        case Series::Cpu: return cpuHistory_;
        case Series::Ram: return ramHistory_;
        case Series::Gpu: return gpuHistory_;
        case Series::NetIn: return netInHistory_;
        case Series::NetOut: return netOutHistory_;
    }
    return cpuHistory_;
}

void Metrics::OpenTaskManager() {
    ShellExecuteW(nullptr, L"open", L"taskmgr.exe", nullptr, nullptr, SW_SHOWNORMAL);
}

void Metrics::OpenDateTimeSettings() {
    ShellExecuteW(nullptr, L"open", L"ms-settings:dateandtime", nullptr, nullptr, SW_SHOWNORMAL);
}

}  // namespace dock
