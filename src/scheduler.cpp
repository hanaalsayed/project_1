//
//  scheduler.cpp
//  Processor Scheduler
//
//  Created by ELMOOTAZBELLAH ELNOZAHY on 9/13/26.
//


#include <deque>
#include "scheduler.hpp"


// ---------- Tunable policy knobs ----------
static const Time_t SLEEP_AFTER_IDLE = 5000;   // us idle before a core goes to deep sleep
static const unsigned MIN_AWAKE      = 1;      // cores kept awake (never put to sleep)
static const CState_t SLEEP_STATE    = C6;     // deep idle state (never C7)
static const PState_t RUN_PSTATE     = P3;     // P-state used while running


// ---------- Per-core bookkeeping ----------
enum CoreStatus { IDLE, BUSY, WAKING, ASLEEP };


struct CoreInfo {
    CoreStatus  status   = IDLE;
    ProcessId_t proc     = InvalidProcessId();  // process currently running
    ProcessId_t reserved = InvalidProcessId();  // process waiting for wake-up
    Time_t      idleSince = 0;
};


static const unsigned NUM_CORES = 8;
static CoreInfo cores[NUM_CORES];
static std::deque<ProcessId_t> readyQ;
static bool initialized = false;


// Core ids 4-7 are small (cheaper per unit of work), 0-3 are big.
static const CPUId_t PREFERENCE_ORDER[NUM_CORES] = {0, 1, 2, 3, 4, 5, 6, 7};


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


static void StartOnCore(CPUId_t c, ProcessId_t pid) {
    // Core must be in C1 here.
    SetPState(c, RUN_PSTATE);
    LoadContext(pid, c);
    RunCore(c);
    cores[c].status = BUSY;
    cores[c].proc = pid;
    cores[c].reserved = InvalidProcessId();
}


static void Dispatch() {
    while (!readyQ.empty()) {
        int idle = -1, asleep = -1;
        for (unsigned k = 0; k < NUM_CORES; k++) {
            CPUId_t c = PREFERENCE_ORDER[k];
            if (idle < 0 && cores[c].status == IDLE) idle = c;
            if (asleep < 0 && cores[c].status == ASLEEP) asleep = c;
        }
        ProcessId_t pid = readyQ.front();
        if (idle >= 0) {
            readyQ.pop_front();
            StartOnCore(idle, pid);
        } else if (asleep >= 0) {
            readyQ.pop_front();
            cores[asleep].status = WAKING;
            cores[asleep].reserved = pid;
            SetCState(asleep, C1);          // completion reported via upcall
        } else {
            break;                          // everything busy or already waking
        }
    }
}


void CreateProcess(ProcessId_t pid) {
    EnsureInit();
    SimOutput("CreateProcess(" + std::to_string(pid) + ")", 4);
    readyQ.push_back(pid);
    Dispatch();
}


void ExitProcess(ProcessId_t pid) {
    EnsureInit();
    int found = -1;
    for (unsigned c = 0; c < NUM_CORES; c++)
        if (cores[c].status == BUSY && cores[c].proc == pid) found = c;
    if (found < 0)
        ThrowException("A process that was not running is calling exit!!!");
    cores[found].status = IDLE;
    cores[found].proc = InvalidProcessId();
    cores[found].idleSince = Now();
    Dispatch();
}


void TimerInterrupt(Time_t now) {
    EnsureInit();
    Dispatch();


    // Put long-idle cores to sleep, but only if nothing is waiting for them
    if (!readyQ.empty()) return;


    unsigned awake = 0;
    for (unsigned c = 0; c < NUM_CORES; c++)
        if (cores[c].status != ASLEEP) awake++;


    // Iterate big cores first so the cheap small cores are the ones left awake
    for (int k = NUM_CORES - 1; k >= 0; k--) {
        CPUId_t c = PREFERENCE_ORDER[k];
        if (awake <= MIN_AWAKE) break;
        if (cores[c].status == IDLE && now - cores[c].idleSince >= SLEEP_AFTER_IDLE) {
            SetCState(c, SLEEP_STATE);
            cores[c].status = ASLEEP;
            awake--;
        }
    }
}


void CStateTransitionComplete(CPUId_t core_id) {
    EnsureInit();
    CoreInfo &ci = cores[core_id];
    if (ci.status != WAKING) return;        // ignore unexpected completions
    ci.status = IDLE;
    ci.idleSince = Now();
    if (ci.reserved != InvalidProcessId()) {
        StartOnCore(core_id, ci.reserved);
    } else {
        Dispatch();
    }
}


void SimulationComplete(Time_t now) {
    std::cout << "Run stopped at " << FormatTime(now) << " after consuming "
              << GetTotalEnergyConsumed()/3600000000.0 << " kWh" << std::endl;
}

