/**
 * Ceph-like PG State Machine — Demo Driver
 *
 * Scenarios:
 *   1. Primary PG normal boot → clean
 *   2. Primary PG degraded → recovery → clean
 *   3. Primary PG degraded → backfill → clean
 *   4. Primary PG incomplete → reset
 *   5. Replica (stray) OSD
 *   6. Role switch: replica → promoted to primary
 */

#include "pg_state_machine.hpp"
#include <thread>
#include <functional>

// ─────────────────────────────────────────────────────────────
// Tiny delay so log timestamps differ visually
// ─────────────────────────────────────────────────────────────
static void ms(int n) {
    std::this_thread::sleep_for(std::chrono::milliseconds(n));
}

// ─────────────────────────────────────────────────────────────
// Banner helpers
// ─────────────────────────────────────────────────────────────
static void banner(const std::string& title) {
    std::cout << "\n" << color::BOLD << color::BLUE;
    std::cout << "╔══════════════════════════════════════════════════╗\n";
    std::cout << "║  " << std::left << std::setw(47) << title << "║\n";
    std::cout << "╚══════════════════════════════════════════════════╝\n";
    std::cout << color::RESET << "\n";
}

static void section(const std::string& msg) {
    std::cout << "\n" << color::BOLD << color::WHITE
              << "  ── " << msg << " ──"
              << color::RESET << "\n";
}

static void fire(PGStateMachine& sm, const std::string& evname,
                 std::function<void()> fn) {
    LOG_EVENT("Firing: " + evname);
    fn();
    ms(30);
}

// ─────────────────────────────────────────────────────────────
// Scenario 1: Primary PG normal boot → clean
// ─────────────────────────────────────────────────────────────
void scenario_primary_clean() {
    banner("Scenario 1 · Primary PG — Normal Boot → Clean");

    PGContext ctx;
    ctx.pg_id          = 1;
    ctx.my_osd_id      = 0;
    ctx.acting_primary = 0;     // we ARE primary
    ctx.has_missing    = false;

    PGStateMachine sm(ctx);
    sm.initiate();

    section("Phase 1: Boot");
    fire(sm, "EvInitialize",        [&]{ sm.process_event(EvInitialize{}); });
    fire(sm, "EvLoad",              [&]{ sm.process_event(EvLoad{}); });

    section("Phase 2: Peering");
    fire(sm, "EvGotInfo",           [&]{ sm.process_event(EvGotInfo{}); });
    fire(sm, "EvGotLog",            [&]{ sm.process_event(EvGotLog{}); });
    fire(sm, "EvGotMissing",        [&]{ sm.process_event(EvGotMissing{}); });

    section("Phase 3: Active → Clean");
    // Active defaults to Clean sub-state — no extra event needed
    LOG_INFO("PG 1 is now Clean. All objects replicated.");
}

// ─────────────────────────────────────────────────────────────
// Scenario 2: Primary PG degraded → recovery → clean
// ─────────────────────────────────────────────────────────────
void scenario_recovery() {
    banner("Scenario 2 · Primary PG — Degraded → Recovery → Clean");

    PGContext ctx;
    ctx.pg_id          = 2;
    ctx.my_osd_id      = 1;
    ctx.acting_primary = 1;
    ctx.has_missing    = true;
    ctx.missing_count  = 5;

    PGStateMachine sm(ctx);
    sm.initiate();

    section("Phase 1: Boot & Peering");
    fire(sm, "EvInitialize",     [&]{ sm.process_event(EvInitialize{}); });
    fire(sm, "EvLoad",           [&]{ sm.process_event(EvLoad{}); });
    fire(sm, "EvGotInfo",        [&]{ sm.process_event(EvGotInfo{}); });
    fire(sm, "EvGotLog",         [&]{ sm.process_event(EvGotLog{}); });
    fire(sm, "EvGotMissing",     [&]{ sm.process_event(EvGotMissing{}); });

    section("Phase 2: Degraded detected");
    fire(sm, "EvObjectDegraded", [&]{ sm.process_event(EvObjectDegraded{}); });

    section("Phase 3: Reserve recovery resources");
    fire(sm, "EvStartRecovery",  [&]{ sm.process_event(EvStartRecovery{}); });
    fire(sm, "EvLocalReserved",  [&]{ sm.process_event(EvLocalReserved{}); });
    fire(sm, "EvRemoteReserved", [&]{ sm.process_event(EvRemoteReserved{}); });

    section("Phase 4: Recovery completes");
    ms(50);
    LOG_INFO("Pushed 5/5 missing objects to replicas");
    fire(sm, "EvRecoveryDone",   [&]{ sm.process_event(EvRecoveryDone{}); });
    LOG_INFO("PG 2 is now Clean after recovery.");
}

// ─────────────────────────────────────────────────────────────
// Scenario 3: Primary PG needs backfill → clean
// ─────────────────────────────────────────────────────────────
void scenario_backfill() {
    banner("Scenario 3 · Primary PG — Degraded → Backfill → Clean");

    PGContext ctx;
    ctx.pg_id          = 3;
    ctx.my_osd_id      = 2;
    ctx.acting_primary = 2;
    ctx.need_backfill  = true;

    PGStateMachine sm(ctx);
    sm.initiate();

    section("Phase 1: Boot & Peering");
    fire(sm, "EvInitialize",   [&]{ sm.process_event(EvInitialize{}); });
    fire(sm, "EvLoad",         [&]{ sm.process_event(EvLoad{}); });
    fire(sm, "EvGotInfo",      [&]{ sm.process_event(EvGotInfo{}); });
    fire(sm, "EvGotLog",       [&]{ sm.process_event(EvGotLog{}); });
    fire(sm, "EvGotMissing",   [&]{ sm.process_event(EvGotMissing{}); });

    section("Phase 2: New OSD added, needs full backfill");
    fire(sm, "EvNeedBackfill", [&]{ sm.process_event(EvNeedBackfill{}); });

    section("Phase 3: Backfill completes");
    ms(80);
    LOG_INFO("Full data copy complete: 100/100 objects transferred");
    fire(sm, "EvBackfillDone", [&]{ sm.process_event(EvBackfillDone{}); });
    LOG_INFO("PG 3 is now Clean after backfill.");
}

// ─────────────────────────────────────────────────────────────
// Scenario 4: up_thru path (WaitUpThru → Active)
// ─────────────────────────────────────────────────────────────
void scenario_upthru() {
    banner("Scenario 4 · Primary PG — WaitUpThru Path");

    PGContext ctx;
    ctx.pg_id          = 4;
    ctx.my_osd_id      = 0;
    ctx.acting_primary = 0;
    ctx.up_thru_needed = true;

    PGStateMachine sm(ctx);
    sm.initiate();

    section("Phase 1: Boot");
    fire(sm, "EvInitialize",        [&]{ sm.process_event(EvInitialize{}); });
    fire(sm, "EvLoad",              [&]{ sm.process_event(EvLoad{}); });
    fire(sm, "EvGotInfo",           [&]{ sm.process_event(EvGotInfo{}); });
    fire(sm, "EvGotLog",            [&]{ sm.process_event(EvGotLog{}); });

    section("Phase 2: Need up_thru from monitor");
    // Send EvUpThruCommitted instead of EvGotMissing to trigger WaitUpThru
    fire(sm, "EvUpThruCommitted",   [&]{ sm.process_event(EvUpThruCommitted{}); });

    section("Phase 3: Monitor confirms up_thru → continue peering");
    fire(sm, "EvGotMissing",        [&]{ sm.process_event(EvGotMissing{}); });
    LOG_INFO("PG 4 Active after up_thru confirmation.");
}

// ─────────────────────────────────────────────────────────────
// Scenario 5: Incomplete PG → Reset
// ─────────────────────────────────────────────────────────────
void scenario_incomplete() {
    banner("Scenario 5 · Primary PG — Incomplete (I/O Blocked)");

    PGContext ctx;
    ctx.pg_id          = 5;
    ctx.my_osd_id      = 0;
    ctx.acting_primary = 0;

    PGStateMachine sm(ctx);
    sm.initiate();

    section("Phase 1: Boot & Peering");
    fire(sm, "EvInitialize",    [&]{ sm.process_event(EvInitialize{}); });
    fire(sm, "EvLoad",          [&]{ sm.process_event(EvLoad{}); });
    fire(sm, "EvGotInfo",       [&]{ sm.process_event(EvGotInfo{}); });
    fire(sm, "EvGotLog",        [&]{ sm.process_event(EvGotLog{}); });
    fire(sm, "EvGotMissing",    [&]{ sm.process_event(EvGotMissing{}); });

    section("Phase 2: Too many OSDs down → Incomplete");
    fire(sm, "EvObjectDegraded",[&]{ sm.process_event(EvObjectDegraded{}); });
    fire(sm, "EvGoIncomplete",  [&]{ sm.process_event(EvGoIncomplete{}); });

    section("Phase 3: Operator resets PG after OSD recovery");
    fire(sm, "EvReset",         [&]{ sm.process_event(EvReset{}); });
    LOG_INFO("PG 5 reset. Will re-peer on next map.");
}

// ─────────────────────────────────────────────────────────────
// Scenario 6: Stray (replica) OSD
// ─────────────────────────────────────────────────────────────
void scenario_stray() {
    banner("Scenario 6 · Stray (Replica) OSD");

    PGContext ctx;
    ctx.pg_id          = 6;
    ctx.my_osd_id      = 3;
    ctx.acting_primary = 0;    // OSD 0 is primary, we are replica

    PGStateMachine sm(ctx);
    sm.initiate();

    section("Phase 1: Boot as Stray");
    fire(sm, "EvInitialize",     [&]{ sm.process_event(EvInitialize{}); });
    fire(sm, "EvLoad",           [&]{ sm.process_event(EvLoad{}); });

    // When load is processed in Started, we default to Primary sub-state.
    // Force Stray role:
    fire(sm, "EvMakeStray",      [&]{ sm.process_event(EvMakeStray{}); });

    LOG_INFO("OSD 3 is a replica for PG 6. Accepting writes from OSD 0.");

    section("Phase 2: Reset on new map");
    fire(sm, "EvReset",          [&]{ sm.process_event(EvReset{}); });
}

// ─────────────────────────────────────────────────────────────
// Scenario 7: Role promotion: Stray → Primary
// ─────────────────────────────────────────────────────────────
void scenario_promotion() {
    banner("Scenario 7 · Role Promotion: Replica → Primary");

    PGContext ctx;
    ctx.pg_id          = 7;
    ctx.my_osd_id      = 2;
    ctx.acting_primary = 0;    // initially, OSD 0 is primary

    PGStateMachine sm(ctx);
    sm.initiate();

    section("Phase 1: Start as replica");
    fire(sm, "EvInitialize",   [&]{ sm.process_event(EvInitialize{}); });
    fire(sm, "EvLoad",         [&]{ sm.process_event(EvLoad{}); });
    fire(sm, "EvMakeStray",    [&]{ sm.process_event(EvMakeStray{}); });
    LOG_INFO("OSD 2 serving as replica.");

    section("Phase 2: OSD 0 crashes — OSD 2 elected primary");
    ctx.acting_primary = 2;
    fire(sm, "EvMakePrimary",  [&]{ sm.process_event(EvMakePrimary{}); });

    section("Phase 3: Re-peer as new primary");
    fire(sm, "EvGotInfo",      [&]{ sm.process_event(EvGotInfo{}); });
    fire(sm, "EvGotLog",       [&]{ sm.process_event(EvGotLog{}); });
    fire(sm, "EvGotMissing",   [&]{ sm.process_event(EvGotMissing{}); });
    LOG_INFO("OSD 2 is now active primary for PG 7.");
}

// ─────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────
int main() {
    std::cout << color::BOLD
              << "\n╔══════════════════════════════════════════════════════════╗\n"
              << "║        Ceph-like PG State Machine (boost::statechart)     ║\n"
              << "╚══════════════════════════════════════════════════════════╝\n"
              << color::RESET << "\n";

    scenario_primary_clean();   ms(100);
    scenario_recovery();        ms(100);
    scenario_backfill();        ms(100);
    scenario_upthru();          ms(100);
    scenario_incomplete();      ms(100);
    scenario_stray();           ms(100);
    scenario_promotion();

    std::cout << "\n" << color::BOLD << color::GREEN
              << "All scenarios complete.\n" << color::RESET;
    return 0;
}
