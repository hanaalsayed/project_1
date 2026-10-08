//
//  scheduler.cpp
//  Processor Scheduler
//
//  Fast and energy-efficient: P3, SJF, small cores first, big-core spill
//  under backlog, idle cores in C6.
//

#include <deque>
#include <algorithm>
#include "scheduler.hpp"

// ---------- Tunable policy knobs ----------
static const Time_t   SLEEP_AFTER_IDLE = 1000;   // us idle before C6 (try 0, 1000, 2000)
static const CState_t SLEEP_STATE      = C6;     // never C7
static const PState_t RUN_PSTATE       = P3;     // your measured best
static const double   WAKE_LATENCY_US  = 7000;   // ASSUMED C6 -> C1 time; set the real value
static const unsigned SPILL_QLEN       = 2;      // queue length that lets big cores help
                                                 // (raise for less energy, lower for more speed)

// ---------- Per-core bookkeeping ----------
enum CoreStatus { IDLE, BUSY, WAKING, ASLEEP };

struct CoreInfo {
    CoreStatus  status    = IDLE;
    ProcessId_t proc      = InvalidProcessId();
    ProcessId_t reserved  = InvalidProcessId();
    Time_t      idleSince = 0;
};

static const unsigned NUM_CORES = 8;
static CoreInfo cores[NUM_CORES];
static std::deque<ProcessId_t> readyQ;
static bool initialized = false;

static bool IsBig(CPUId_t c) { return c < 4; }

static void EnsureInit() {
    if (initialized) return;
    initialized = true;
    for (unsigned i = 0; i < NUM_CORES; i++) {
        cores[i].status = IDLE;
        cores[i].proc = InvalidProcessId();
        cores[i].reserved = InvalidProcessId();
        cores[i].idleSince = Now();
    }
}

// Assumed speed model, used only for the wake-vs-wait decision.
static double CoreSpeed(CPUId_t c) {
    double s = IsBig(c) ? 1.0 : 0.6;
    return s * (1.0 - 0.2 * (double)RUN_PSTATE);
}

static double TimeUntilCoreFrees() {
    double best = 1e18;
    for (unsigned c = 0; c < NUM_CORES; c++) {
        if (cores[c].status != BUSY) continue;
        best = std::min(best, (double)GetRemaining(cores[c].proc) / CoreSpeed(c));
    }
    return best;
}

static int FindCore(CoreStatus s, bool wantBig) {
    for (unsigned c = 0; c < NUM_CORES; c++)
        if (cores[c].status == s && IsBig(c) == wantBig) return (int)c;
    return -1;
}

// Index in readyQ of the job with the least remaining work (SJF).
static size_t ShortestIndex() {
    size_t best = 0;
    for (size_t i = 1; i < readyQ.size(); i++)
        if (GetRemaining(readyQ[i]) < GetRemaining(readyQ[best])) best = i;
    return best;
}

// Is this job bigger than the median of the queue?
static bool IsLongInQueue(ProcessId_t pid) {
    std::vector<Time_t> r;
    for (size_t i = 0; i < readyQ.size(); i++) r.push_back(GetRemaining(readyQ[i]));
    std::sort(r.begin(), r.end());
    return GetRemaining(pid) > r[r.size() / 2];
}

// Returns -1 if the process should wait. Sets wake if the core must be woken.
static int PickCore(ProcessId_t pid, bool &wake) {
    wake = false;
    bool spill = readyQ.size() >= SPILL_QLEN;

    int s = FindCore(IDLE, false);
    int b = spill ? FindCore(IDLE, true) : -1;

    // Both free: long job takes the big core, short job takes the small one.
    if (s >= 0 && b >= 0) return IsLongInQueue(pid) ? b : s;
    if (s >= 0) return s;
    if (b >= 0) return b;

    // Nothing idle. Wake a core only if that beats waiting for a busy one.
    if (WAKE_LATENCY_US < TimeUntilCoreFrees()) {
        int c = FindCore(ASLEEP, false);
        if (c < 0 && spill) c = FindCore(ASLEEP, true);
        if (c >= 0) { wake = true; return c; }
    }
    return -1;
}

static void StartOnCore(CPUId_t c, ProcessId_t pid) {
    SetPState(c, RUN_PSTATE);
    LoadContext(pid, c);
    RunCore(c);
    cores[c].status = BUSY;
    cores[c].proc = pid;
    cores[c].reserved = InvalidProcessId();
}

static void Dispatch() {
    while (!readyQ.empty()) {
        size_t idx = ShortestIndex();
        ProcessId_t pid = readyQ[idx];
        bool wake = false;
        int c = PickCore(pid, wake);
        if (c < 0) break;
        readyQ.erase(readyQ.begin() + idx);
        if (!wake) {
            StartOnCore(c, pid);
        } else {
            cores[c].status = WAKING;
            cores[c].reserved = pid;
            SetCState(c, C1);                 // completion via upcall
        }
    }
}

// Idle cores go to C6. Busy and waking cores are never touched.
static void SleepIdleCores(Time_t now) {
    for (unsigned c = 0; c < NUM_CORES; c++) {
        if (cores[c].status != IDLE) continue;
        if (now - cores[c].idleSince < SLEEP_AFTER_IDLE) continue;
        SetCState(c, SLEEP_STATE);
        cores[c].status = ASLEEP;
    }
}

void CreateProcess(ProcessId_t pid) {
    EnsureInit();
    SimOutput("CreateProcess(" + std::to_string(pid) + ")", 4);
    readyQ.push_back(pid);
    Dispatch();
    SleepIdleCores(Now());
}

void ExitProcess(ProcessId_t pid) {
    EnsureInit();
    int found = -1;
    for (unsigned c = 0; c < NUM_CORES; c++)
        if (cores[c].status == BUSY && cores[c].proc == pid) found = c;
    if (found < 0) {
        ThrowException("A process that was not running is calling exit!!!");
        return;
    }
    cores[found].status = IDLE;
    cores[found].proc = InvalidProcessId();
    cores[found].idleSince = Now();
    Dispatch();
    SleepIdleCores(Now());
}

void TimerInterrupt(Time_t now) {
    EnsureInit();
    Dispatch();
    SleepIdleCores(now);
}

void CStateTransitionComplete(CPUId_t core_id) {
    EnsureInit();
    CoreInfo &ci = cores[core_id];
    if (ci.status != WAKING) return;
    ci.status = IDLE;
    ci.idleSince = Now();
    if (ci.reserved != InvalidProcessId()) {
        StartOnCore(core_id, ci.reserved);
    } else {
        Dispatch();
        SleepIdleCores(Now());
    }
}

void SimulationComplete(Time_t now) {
    std::cout << "Run stopped at " << FormatTime(now) << " after consuming "
              << GetTotalEnergyConsumed()/3600000000.0 << " kWh" << std::endl;
}