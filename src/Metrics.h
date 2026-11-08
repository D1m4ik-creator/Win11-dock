#pragma once

#include "Common.h"

#include <pdh.h>
#include <pdhmsg.h>

namespace dock {

struct Sample {
    float cpuPercent = 0.0f;      // 0..100
    float cpuGHz = 0.0f;
    float ramPercent = 0.0f;
    float ramUsedGB = 0.0f;
    float ramTotalGB = 0.0f;
    bool gpuValid = false;
    float gpuPercent = 0.0f;
    double netInBytesPerSec = 0.0;
    double netOutBytesPerSec = 0.0;
};

enum class Series { Cpu, Ram, Gpu, NetIn, NetOut };

class Metrics {
public:
    bool Init();
    void Shutdown();

    // Call ~10x per second; sampling is throttled internally to 1 Hz.
    void Poll();
    // Force history push (called once per second from Poll).
    const Sample& Current() const { return current_; }

    const std::deque<float>& History(Series s) const;

    // Opens the relevant system dialog (task manager etc.).
    static void OpenTaskManager();
    static void OpenDateTimeSettings();

private:
    void SampleCpu();
    void SampleMemory();
    void SampleGpu();
    void SampleNetwork();
    void PushHistory();

    Sample current_{};
    Sample previous_{};

    std::deque<float> cpuHistory_;
    std::deque<float> ramHistory_;
    std::deque<float> gpuHistory_;
    std::deque<float> netInHistory_;
    std::deque<float> netOutHistory_;

    ULONGLONG lastCpuIdle_ = 0;
    ULONGLONG lastCpuKernel_ = 0;
    ULONGLONG lastCpuUser_ = 0;

    double lastNetIn_ = -1.0;
    double lastNetOut_ = -1.0;
    ULONGLONG lastNetTick_ = 0;

    PDH_HQUERY gpuQuery_ = nullptr;
    PDH_HCOUNTER gpuCounter_ = nullptr;
    bool gpuAvailable_ = false;

    ULONGLONG lastPollTick_ = 0;
};

}  // namespace dock
