#pragma once
/**
 * Ceph-like PG State Machine using boost::statechart
 *
 * Mimics Ceph's src/osd/PeeringState.h state machine architecture.
 *
 * State Hierarchy:
 *
 *  Machine (top-level)
 *  └── Initial          (entry point)
 *  └── Reset            (default recovery point)
 *  └── Started
 *      ├── Primary      (this OSD is primary for the PG)
 *      │   ├── Peering
 *      │   │   ├── GetInfo
 *      │   │   ├── GetLog
 *      │   │   ├── GetMissing
 *      │   │   └── WaitUpThru
 *      │   └── Active
 *      │       ├── Clean
 *      │       ├── Degraded
 *      │       ├── Recovering
 *      │       ├── Backfilling
 *      │       ├── WaitLocalRecovery
 *      │       ├── WaitRemoteRecovery
 *      │       └── Incomplete
 *      └── Stray        (this OSD is a replica)
 *          └── ReplicaActive
 */

#include <boost/statechart/state_machine.hpp>
#include <boost/statechart/state.hpp>
#include <boost/statechart/simple_state.hpp>
#include <boost/statechart/transition.hpp>
#include <boost/statechart/custom_reaction.hpp>
#include <boost/statechart/event.hpp>
#include <boost/mpl/list.hpp>
#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace sc  = boost::statechart;
namespace mpl = boost::mpl;

// ─────────────────────────────────────────────────────────────
// Color / Logging helpers
// ─────────────────────────────────────────────────────────────
namespace color {
    const char* RESET   = "\033[0m";
    const char* BOLD    = "\033[1m";
    const char* RED     = "\033[31m";
    const char* GREEN   = "\033[32m";
    const char* YELLOW  = "\033[33m";
    const char* BLUE    = "\033[34m";
    const char* MAGENTA = "\033[35m";
    const char* CYAN    = "\033[36m";
    const char* WHITE   = "\033[37m";
    const char* GRAY    = "\033[90m";
}

inline std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    std::ostringstream oss;
    oss << std::put_time(std::localtime(&t), "%H:%M:%S");
    return oss.str();
}

#define PG_LOG(level, color_code, msg) \
    std::cout << color::GRAY << "[" << timestamp() << "] " \
              << color_code << level << color::RESET \
              << " " << msg << "\n"

#define LOG_STATE(msg)  PG_LOG("STATE", color::CYAN,    msg)
#define LOG_EVENT(msg)  PG_LOG("EVENT", color::YELLOW,  msg)
#define LOG_INFO(msg)   PG_LOG("INFO ",  color::GREEN,  msg)
#define LOG_WARN(msg)   PG_LOG("WARN ",  color::RED,    msg)
#define LOG_TRANS(a, b) PG_LOG("TRANS", color::MAGENTA, \
    std::string(a) + " ──► " + std::string(b))

// ─────────────────────────────────────────────────────────────
// PG Context  (shared data passed to all states)
// ─────────────────────────────────────────────────────────────
struct PGContext {
    int  pg_id          = 0;
    int  acting_primary = 0;      // which OSD is primary
    int  my_osd_id      = 0;      // this OSD's id
    bool up_thru_needed = false;
    bool has_missing    = false;
    bool need_backfill  = false;
    int  missing_count  = 0;
    int  total_objects  = 100;
    int  recovered      = 0;

    bool is_primary() const { return my_osd_id == acting_primary; }
};

// ─────────────────────────────────────────────────────────────
// Forward declarations
// ─────────────────────────────────────────────────────────────
struct Initial;
struct Reset;
struct Started;
struct Primary;
struct Stray;
struct Peering;
struct Active;
struct GetInfo;
struct GetLog;
struct GetMissing;
struct WaitUpThru;
struct Clean;
struct Degraded;
struct WaitLocalRecovery;
struct WaitRemoteRecovery;
struct Recovering;
struct Backfilling;
struct Incomplete;
struct ReplicaActive;

// ─────────────────────────────────────────────────────────────
//  E V E N T S
//  (mirroring Ceph's event naming convention)
// ─────────────────────────────────────────────────────────────

// Lifecycle
struct EvInitialize        : sc::event<EvInitialize>        {};
struct EvLoad              : sc::event<EvLoad>              {};
struct EvDoRecovery        : sc::event<EvDoRecovery>        {};
struct EvReset             : sc::event<EvReset>             {};

// Role assignment
struct EvMakePrimary       : sc::event<EvMakePrimary>       {};
struct EvMakeStray         : sc::event<EvMakeStray>         {};   // replica

// Peering sub-events
struct EvGotInfo           : sc::event<EvGotInfo>           {};
struct EvGotLog            : sc::event<EvGotLog>            {};
struct EvGotMissing        : sc::event<EvGotMissing>        {};
struct EvUpThruCommitted   : sc::event<EvUpThruCommitted>   {};
struct EvAllReplicasActive : sc::event<EvAllReplicasActive> {};

// Active sub-events
struct EvGoClean           : sc::event<EvGoClean>           {};
struct EvObjectDegraded    : sc::event<EvObjectDegraded>    {};
struct EvStartRecovery     : sc::event<EvStartRecovery>     {};
struct EvLocalReserved     : sc::event<EvLocalReserved>     {};
struct EvRemoteReserved    : sc::event<EvRemoteReserved>    {};
struct EvRecoveryDone      : sc::event<EvRecoveryDone>      {};
struct EvNeedBackfill      : sc::event<EvNeedBackfill>      {};
struct EvBackfillDone      : sc::event<EvBackfillDone>      {};
struct EvGoIncomplete      : sc::event<EvGoIncomplete>      {};

// Replica events
struct EvReplicaActivate   : sc::event<EvReplicaActivate>   {};

// ─────────────────────────────────────────────────────────────
//  S T A T E   M A C H I N E
// ─────────────────────────────────────────────────────────────
struct PGStateMachine
    : sc::state_machine<PGStateMachine, Initial>
{
    PGContext ctx;

    explicit PGStateMachine(PGContext c) : ctx(std::move(c)) {
        LOG_INFO("PGStateMachine created for pg_id=" + std::to_string(ctx.pg_id)
                 + "  osd=" + std::to_string(ctx.my_osd_id));
    }

    // Helper: collect current state name (walks inner-states)
    std::vector<std::string> active_state_names() const;
    void print_state() const;
};

// ─────────────────────────────────────────────────────────────
//  S T A T E S
// ─────────────────────────────────────────────────────────────

// ── Initial ──────────────────────────────────────────────────
struct Initial
    : sc::simple_state<Initial, PGStateMachine>
{
    using reactions = sc::transition<EvInitialize, Reset>;
    Initial()  { LOG_STATE("► Initial"); }
    ~Initial() { LOG_STATE("◄ Initial"); }
};

// ── Reset ─────────────────────────────────────────────────────
struct Reset
    : sc::simple_state<Reset, PGStateMachine>
{
    using reactions = sc::transition<EvLoad, Started>;
    Reset()  { LOG_STATE("► Reset – waiting for map/epoch"); }
    ~Reset() { LOG_STATE("◄ Reset"); }
};

// ── Started ───────────────────────────────────────────────────
//    Orthogonal region that dispatches to Primary or Stray
struct Started
    : sc::simple_state<Started, PGStateMachine, Primary>
{
    using reactions = mpl::list<
        sc::transition<EvReset, Reset>
    >;
    Started()  { LOG_STATE("► Started"); }
    ~Started() { LOG_STATE("◄ Started"); }
};

// ─────────────────────────────────────────────────────────────
//  PRIMARY branch
// ─────────────────────────────────────────────────────────────
struct Primary
    : sc::simple_state<Primary, Started, Peering>
{
    using reactions = mpl::list<
        sc::transition<EvMakeStray, Stray>
    >;
    Primary()  {
        LOG_STATE("► Primary – this OSD is now PG primary");
    }
    ~Primary() { LOG_STATE("◄ Primary"); }
};

// ── Peering ───────────────────────────────────────────────────
struct Peering
    : sc::simple_state<Peering, Primary, GetInfo>
{
    Peering()  { LOG_STATE("  ► Peering – collecting replica info"); }
    ~Peering() { LOG_STATE("  ◄ Peering"); }
};

// ── GetInfo ───────────────────────────────────────────────────
struct GetInfo
    : sc::simple_state<GetInfo, Peering>
{
    using reactions = sc::transition<EvGotInfo, GetLog>;
    GetInfo()  { LOG_STATE("    ► GetInfo – querying replicas for pg_info"); }
    ~GetInfo() { LOG_STATE("    ◄ GetInfo"); }
};

// ── GetLog ────────────────────────────────────────────────────
struct GetLog
    : sc::simple_state<GetLog, Peering>
{
    using reactions = sc::transition<EvGotLog, GetMissing>;
    GetLog()  { LOG_STATE("    ► GetLog – fetching authoritative log"); }
    ~GetLog() { LOG_STATE("    ◄ GetLog"); }
};

// ── GetMissing ────────────────────────────────────────────────
struct GetMissing
    : sc::simple_state<GetMissing, Peering>
{
    using reactions = mpl::list<
        sc::transition<EvGotMissing,      Active>,
        sc::transition<EvUpThruCommitted, WaitUpThru>
    >;
    GetMissing()  {
        LOG_STATE("    ► GetMissing – scanning for missing objects");
    }
    ~GetMissing() { LOG_STATE("    ◄ GetMissing"); }
};

// ── WaitUpThru ────────────────────────────────────────────────
struct WaitUpThru
    : sc::simple_state<WaitUpThru, Peering>
{
    using reactions = sc::transition<EvGotMissing, Active>;
    WaitUpThru()  {
        LOG_STATE("    ► WaitUpThru – waiting for MON to update up_thru");
    }
    ~WaitUpThru() { LOG_STATE("    ◄ WaitUpThru"); }
};

// ─────────────────────────────────────────────────────────────
//  ACTIVE branch
// ─────────────────────────────────────────────────────────────
struct Active
    : sc::simple_state<Active, Primary, Clean>
{
    using reactions = mpl::list<
        sc::transition<EvReset, Reset>
    >;
    Active()  { LOG_STATE("  ► Active – PG is active"); }
    ~Active() { LOG_STATE("  ◄ Active"); }
};

// ── Clean ─────────────────────────────────────────────────────
struct Clean
    : sc::simple_state<Clean, Active>
{
    using reactions = mpl::list<
        sc::transition<EvObjectDegraded, Degraded>,
        sc::transition<EvNeedBackfill,   Backfilling>
    >;
    Clean() {
        LOG_STATE("    ► Clean – all objects fully replicated ✓");
        LOG_INFO("PG health: OK");
    }
    ~Clean() { LOG_STATE("    ◄ Clean"); }
};

// ── Degraded ──────────────────────────────────────────────────
struct Degraded
    : sc::simple_state<Degraded, Active>
{
    using reactions = mpl::list<
        sc::transition<EvStartRecovery, WaitLocalRecovery>,
        sc::transition<EvGoClean,       Clean>,
        sc::transition<EvGoIncomplete,  Incomplete>
    >;
    Degraded() {
        LOG_STATE("    ► Degraded – some replicas missing objects");
        LOG_WARN("PG health: DEGRADED");
    }
    ~Degraded() { LOG_STATE("    ◄ Degraded"); }
};

// ── WaitLocalRecovery ─────────────────────────────────────────
struct WaitLocalRecovery
    : sc::simple_state<WaitLocalRecovery, Active>
{
    using reactions = sc::transition<EvLocalReserved, WaitRemoteRecovery>;
    WaitLocalRecovery() {
        LOG_STATE("    ► WaitLocalRecovery – reserving local recovery resources");
    }
    ~WaitLocalRecovery() { LOG_STATE("    ◄ WaitLocalRecovery"); }
};

// ── WaitRemoteRecovery ────────────────────────────────────────
struct WaitRemoteRecovery
    : sc::simple_state<WaitRemoteRecovery, Active>
{
    using reactions = sc::transition<EvRemoteReserved, Recovering>;
    WaitRemoteRecovery() {
        LOG_STATE("    ► WaitRemoteRecovery – reserving remote (replica) resources");
    }
    ~WaitRemoteRecovery() { LOG_STATE("    ◄ WaitRemoteRecovery"); }
};

// ── Recovering ────────────────────────────────────────────────
struct Recovering
    : sc::simple_state<Recovering, Active>
{
    using reactions = mpl::list<
        sc::transition<EvRecoveryDone, Clean>,
        sc::transition<EvGoClean,      Clean>
    >;
    Recovering() {
        LOG_STATE("    ► Recovering – pushing missing objects to replicas");
        LOG_INFO("Recovery in progress…");
    }
    ~Recovering() { LOG_STATE("    ◄ Recovering"); }
};

// ── Backfilling ───────────────────────────────────────────────
struct Backfilling
    : sc::simple_state<Backfilling, Active>
{
    using reactions = mpl::list<
        sc::transition<EvBackfillDone, Clean>,
        sc::transition<EvGoClean,      Clean>
    >;
    Backfilling() {
        LOG_STATE("    ► Backfilling – full data copy to new/restarted OSD");
        LOG_WARN("Backfill in progress – high I/O");
    }
    ~Backfilling() { LOG_STATE("    ◄ Backfilling"); }
};

// ── Incomplete ────────────────────────────────────────────────
struct Incomplete
    : sc::simple_state<Incomplete, Active>
{
    using reactions = sc::transition<EvReset, Reset>;
    Incomplete() {
        LOG_STATE("    ► Incomplete – cannot satisfy min_size constraint");
        LOG_WARN("PG INCOMPLETE – I/O BLOCKED");
    }
    ~Incomplete() { LOG_STATE("    ◄ Incomplete"); }
};

// ─────────────────────────────────────────────────────────────
//  STRAY branch  (this OSD is a replica)
// ─────────────────────────────────────────────────────────────
struct Stray
    : sc::simple_state<Stray, Started, ReplicaActive>
{
    using reactions = sc::transition<EvMakePrimary, Primary>;
    Stray() {
        LOG_STATE("► Stray – this OSD is a replica for this PG");
    }
    ~Stray() { LOG_STATE("◄ Stray"); }
};

struct ReplicaActive
    : sc::simple_state<ReplicaActive, Stray>
{
    using reactions = mpl::list<
        sc::transition<EvReset, Reset>
    >;
    ReplicaActive() {
        LOG_STATE("  ► ReplicaActive – serving reads, accepting writes from primary");
    }
    ~ReplicaActive() { LOG_STATE("  ◄ ReplicaActive"); }
};
