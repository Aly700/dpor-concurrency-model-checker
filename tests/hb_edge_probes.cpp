#include "model/checker.hpp"

#include <cassert>
#include <iostream>
#include <stdexcept>

using namespace model;

static Action R(std::string a) { Action x; x.kind = ActionKind::Read; x.address = std::move(a); return x; }
static Action W(std::string a) { Action x; x.kind = ActionKind::Write; x.address = std::move(a); return x; }
static Action L(std::string m) { Action x; x.kind = ActionKind::Lock; x.mutex = std::move(m); return x; }
static Action TL(std::string m, RegisterId destination) { Action x; x.kind = ActionKind::TryLock; x.mutex = std::move(m); x.destination = destination; return x; }
static Action U(std::string m) { Action x; x.kind = ActionKind::Unlock; x.mutex = std::move(m); return x; }
static Action WAIT(std::string cv, std::string m) { Action x; x.kind = ActionKind::Wait; x.condition = std::move(cv); x.mutex = std::move(m); return x; }
static Action SIG(std::string cv) { Action x; x.kind = ActionKind::Signal; x.condition = std::move(cv); return x; }
static Action BCAST(std::string cv) { Action x; x.kind = ActionKind::Broadcast; x.condition = std::move(cv); return x; }
static Action BW(std::string b, std::uint32_t parties) { Action x; x.kind = ActionKind::BarrierWait; x.barrier = std::move(b); x.parties = parties; return x; }
static Action RL(std::string rw) { Action x; x.kind = ActionKind::RLock; x.rwlock = std::move(rw); return x; }
static Action RU(std::string rw) { Action x; x.kind = ActionKind::RUnlock; x.rwlock = std::move(rw); return x; }
static Action WL(std::string rw) { Action x; x.kind = ActionKind::WLock; x.rwlock = std::move(rw); return x; }
static Action WU(std::string rw) { Action x; x.kind = ActionKind::WUnlock; x.rwlock = std::move(rw); return x; }
static Action UP(std::string rw) { Action x; x.kind = ActionKind::Upgrade; x.rwlock = std::move(rw); return x; }
static Action DOWN(std::string rw) { Action x; x.kind = ActionKind::Downgrade; x.rwlock = std::move(rw); return x; }

static ScheduleStep S(ThreadId thread, std::uint32_t action) {
    return ScheduleStep{thread, action, std::nullopt};
}

static void require_probe(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

static void require_clean_replay(const Program& program,
                                 const Schedule& schedule,
                                 const char* message) {
    const ModelChecker checker(program);
    const auto replay = checker.replay(schedule);
    require_probe(!replay.first_race.has_value() &&
                      !replay.first_deadlock.has_value() &&
                      !replay.first_error.has_value(),
                  message);
}

static void require_racy_replay(const Program& program,
                                const Schedule& schedule,
                                const char* message) {
    const ModelChecker checker(program);
    const auto replay = checker.replay(schedule);
    require_probe(replay.first_race.has_value(), message);
    const auto reproduced = checker.replay(replay.first_race->schedule);
    require_probe(reproduced.first_race.has_value() &&
                      *reproduced.first_race == *replay.first_race,
                  "negative HB probe did not replay identically");
}

static void require_cv_deadlock_replay(const Program& program,
                                       const Schedule& schedule,
                                       ThreadId waiter,
                                       const std::string& condition,
                                       const char* message) {
    const ModelChecker checker(program);
    const auto replay = checker.replay(schedule);
    require_probe(replay.first_deadlock.has_value(), message);

    bool found_waiter = false;
    for (const BlockedThread& blocked : replay.first_deadlock->blocked_threads) {
        found_waiter = found_waiter ||
                       (blocked.thread == waiter &&
                        blocked.kind == BlockedOnKind::ConditionVariable &&
                        blocked.condition == condition);
    }
    require_probe(found_waiter,
                  "lost Broadcast wakeup did not report the parked condition waiter");

    const auto reproduced = checker.replay(replay.first_deadlock->schedule);
    require_probe(reproduced.first_deadlock.has_value() &&
                      *reproduced.first_deadlock == *replay.first_deadlock,
                  "Broadcast lost-wakeup deadlock did not replay identically");
}

static void require_barrier_agreement(const Program& program, const char* message) {
    const ModelChecker checker(program);
    const auto naive = checker.explore_naive();
    const auto dpor = checker.explore_dpor();
    require_probe(naive.first_race.has_value() == dpor.first_race.has_value() &&
                      naive.first_deadlock.has_value() == dpor.first_deadlock.has_value() &&
                      naive.first_error.has_value() == dpor.first_error.has_value() &&
                      dpor.schedules_explored <= naive.schedules_explored,
                  message);
}

static void require_trylock_agreement(const Program& program, const char* message) {
    const ModelChecker checker(program);
    const auto naive = checker.explore_naive();
    const auto dpor = checker.explore_dpor();
    require_probe(naive.first_race.has_value() == dpor.first_race.has_value() &&
                      naive.first_deadlock.has_value() == dpor.first_deadlock.has_value() &&
                      naive.first_error.has_value() == dpor.first_error.has_value() &&
                      naive.first_assertion.has_value() == dpor.first_assertion.has_value() &&
                      (naive.cycles_detected > 0) == (dpor.cycles_detected > 0) &&
                      (naive.fair_cycles > 0) == (dpor.fair_cycles > 0) &&
                      (naive.strongly_unfair_cycles > 0) ==
                          (dpor.strongly_unfair_cycles > 0) &&
                      (naive.unfair_cycles > 0) == (dpor.unfair_cycles > 0) &&
                      dpor.schedules_explored <= naive.schedules_explored,
                  message);
    for (const CheckResult* result : {&naive, &dpor}) {
        if (result->first_race.has_value()) {
            const auto replayed = checker.replay(result->first_race->schedule);
            require_probe(replayed.first_race.has_value() &&
                              *replayed.first_race == *result->first_race,
                          "TryLock HB race report did not replay identically");
        }
    }
}

static void check_agreement(const Program& p, const char* name) {
    const ModelChecker c(p);
    const auto n = c.explore_naive();
    const auto d = c.explore_dpor();
    assert(n.first_race.has_value() == d.first_race.has_value());
    assert(n.first_deadlock.has_value() == d.first_deadlock.has_value());
    assert(n.first_error.has_value() == d.first_error.has_value());
    std::cout << name << ": naive/dpor agree (race=" << n.first_race.has_value()
              << " deadlock=" << n.first_deadlock.has_value() << ")\n";
}

int main() {
    {
        // Each waiter joins the broadcaster at the wake point. The broadcaster
        // deliberately does not hold m, so this join is the only HB path from
        // its write to the post-wake read. Mirror the broadcaster across
        // thread ids so deterministic iteration order cannot mask one
        // directional vector-clock defect.
        const Program low_to_high{{
            {W("x"), BCAST("cv")},
            {L("m"), WAIT("cv", "m"), R("x"), U("m")},
        }};
        require_clean_replay(
            low_to_high,
            {S(1, 0), S(1, 1), S(0, 0), S(0, 1),
             S(1, 1), S(1, 2), S(1, 3)},
            "Broadcast lost the low-to-high broadcaster-to-waiter wake edge");

        const Program high_to_low{{
            {L("m"), WAIT("cv", "m"), R("x"), U("m")},
            {W("x"), BCAST("cv")},
        }};
        require_clean_replay(
            high_to_low,
            {S(0, 0), S(0, 1), S(1, 0), S(1, 1),
             S(0, 1), S(0, 2), S(0, 3)},
            "Broadcast lost the high-to-low broadcaster-to-waiter wake edge");
    }
    {
        // A shared Broadcast fans one broadcaster edge out to both waiters;
        // it must not order the waiters with each other. Both accesses occur
        // after their mutex-protected reacquisition sections, so the ordinary
        // mutex handoff also cannot order the first access before the second.
        // Mirror the writer across waiter ids to guard both clock directions.
        const Program low_to_high{{
            {L("m"), WAIT("cv", "m"), U("m"), W("x")},
            {L("m"), WAIT("cv", "m"), U("m"), R("x")},
            {BCAST("cv")},
        }};
        require_racy_replay(
            low_to_high,
            {S(0, 0), S(0, 1), S(1, 0), S(1, 1), S(2, 0),
             S(0, 1), S(0, 2), S(0, 3),
             S(1, 1), S(1, 2), S(1, 3)},
            "Broadcast introduced a low-to-high waiter-waiter edge");

        const Program high_to_low{{
            {L("m"), WAIT("cv", "m"), U("m"), R("x")},
            {L("m"), WAIT("cv", "m"), U("m"), W("x")},
            {BCAST("cv")},
        }};
        require_racy_replay(
            high_to_low,
            {S(0, 0), S(0, 1), S(1, 0), S(1, 1), S(2, 0),
             S(1, 1), S(1, 2), S(1, 3),
             S(0, 1), S(0, 2), S(0, 3)},
            "Broadcast introduced a high-to-low waiter-waiter edge");
    }
    {
        // An empty Broadcast stores neither a permit nor a clock. A later
        // Signal wakes the eventual waiter, but it cannot carry the earlier
        // broadcaster's write into that waiter's read.
        const Program no_clock_trace{{
            {W("x"), BCAST("cv")},
            {L("m"), WAIT("cv", "m"), R("x"), U("m")},
            {SIG("cv")},
        }};
        require_racy_replay(
            no_clock_trace,
            {S(0, 0), S(0, 1), S(1, 0), S(1, 1),
             S(2, 0), S(1, 1), S(1, 2), S(1, 3)},
            "empty Broadcast leaked a clock into a future waiter");

        const Program no_clock_trace_mirrored{{
            {L("m"), WAIT("cv", "m"), R("x"), U("m")},
            {SIG("cv")},
            {W("x"), BCAST("cv")},
        }};
        require_racy_replay(
            no_clock_trace_mirrored,
            {S(2, 0), S(2, 1), S(0, 0), S(0, 1),
             S(1, 0), S(0, 1), S(0, 2), S(0, 3)},
            "empty Broadcast leaked a mirrored clock into a future waiter");

        // The same no-permit rule leaves a waiter issued after an empty
        // Broadcast parked forever.
        const Program lost_wakeup{{
            {BCAST("cv")},
            {L("m"), WAIT("cv", "m")},
        }};
        require_cv_deadlock_replay(
            lost_wakeup,
            {S(0, 0), S(1, 0), S(1, 1)},
            1,
            "cv",
            "empty Broadcast queued a permit for a future waiter");

        const Program lost_wakeup_mirrored{{
            {L("m"), WAIT("cv", "m")},
            {BCAST("cv")},
        }};
        require_cv_deadlock_replay(
            lost_wakeup_mirrored,
            {S(1, 0), S(0, 0), S(0, 1)},
            0,
            "cv",
            "mirrored empty Broadcast queued a permit for a future waiter");
    }
    {
        // Wake-edge isolation: T1 signals WITHOUT holding the mutex, so the
        // only happens-before path from T1's unprotected write to T0's
        // post-wake read is the signaler->woken clock join. A missing wake
        // edge would surface here as a false race.
        Program p;
        p.threads = {
            {L("m"), WAIT("cv", "m"), R("x"), U("m")},
            {W("x"), SIG("cv")},
        };
        const ModelChecker c(p);
        const auto r = c.explore_naive();
        assert(!r.first_race.has_value());      // wake edge orders write -> read
        assert(r.first_deadlock.has_value());   // lost-wakeup class exists
        const auto rep = c.replay(r.first_deadlock->schedule);
        assert(rep.first_deadlock.has_value() && *rep.first_deadlock == *r.first_deadlock);
        check_agreement(p, "wake-edge isolation");
    }
    {
        // Mutex-protected write on both sides of a wait: no race anywhere,
        // lost-wakeup deadlock in the signal-first class.
        Program p;
        p.threads = {
            {L("m"), WAIT("cv", "m"), W("x"), U("m")},
            {L("m"), W("x"), SIG("cv"), U("m")},
        };
        const ModelChecker c(p);
        const auto r = c.explore_naive();
        assert(!r.first_race.has_value());
        assert(r.first_deadlock.has_value());
        check_agreement(p, "protected handoff");
    }
    {
        // Wait-release window with an unprotected writer: T1's unlocked write
        // races with T0's lock-held write because wait releases m in between.
        Program p;
        p.threads = {
            {L("m"), W("x"), WAIT("cv", "m"), U("m")},
            {W("x"), SIG("cv")},
        };
        const ModelChecker c(p);
        const auto r = c.explore_naive();
        assert(r.first_race.has_value());
        const auto rep = c.replay(r.first_race->schedule);
        assert(rep.first_race.has_value() && *rep.first_race == *r.first_race);
        check_agreement(p, "unprotected writer through window");
    }
    {
        // Join edge vs cv: T1 waits, T0 signals then finishes; T2 joins T0 and
        // writes what T0 wrote pre-signal. Join must order T0's write before
        // T2's write; no race between them.
        Program p;
        Action j0; j0.kind = ActionKind::Join; j0.target = 0;
        p.threads = {
            {W("x"), SIG("cv")},
            {L("m"), WAIT("cv", "m"), U("m")},
            {j0, W("x")},
        };
        const ModelChecker c(p);
        const auto r = c.explore_naive();
        assert(!r.first_race.has_value());
        assert(r.first_deadlock.has_value());   // signal-before-wait class
        check_agreement(p, "join orders across threads");
    }
    {
        // Parked-to-last arrival edge. T0's publication must reach the last
        // arriver T1 before T1 continues past this generation.
        Program p{{
            {W("x"), BW("phase", 2)},
            {BW("phase", 2), R("x")},
        }};
        require_clean_replay(p,
                             {S(0, 0), S(0, 1), S(1, 0), S(1, 1)},
                             "barrier last arriver did not join the parked arrival clock");
        require_barrier_agreement(p, "barrier parked-to-last oracle mismatch");
    }
    {
        // Last-to-parked arrival edge. The participant that arrived first is
        // released only after joining the last arriver's publication.
        Program p{{
            {BW("phase", 2), R("x")},
            {W("x"), BW("phase", 2)},
        }};
        require_clean_replay(p,
                             {S(0, 0), S(1, 0), S(1, 1), S(0, 1)},
                             "barrier parked participant did not join the last arrival clock");
        require_barrier_agreement(p, "barrier last-to-parked oracle mismatch");
    }
    {
        // Generation 0 must not leak directly into a disjoint generation 1.
        // Schedule order alone creates no HB edge from T0's write to T2's read.
        Program p{{
            {W("x"), BW("phase", 2)},
            {BW("phase", 2)},
            {BW("phase", 2), R("x")},
            {BW("phase", 2)},
        }};
        require_racy_replay(p,
                            {S(0, 0), S(0, 1), S(1, 0),
                             S(2, 0), S(3, 0), S(2, 1)},
                            "prior-generation arrival clock leaked into the next release");
        require_barrier_agreement(p, "barrier forward generation-leak oracle mismatch");
    }
    {
        // Old participants must not be released a second time. If T0 were
        // retained in generation 1, T2's write would falsely order T0's read.
        Program p{{
            {BW("phase", 2), R("x")},
            {BW("phase", 2)},
            {W("x"), BW("phase", 2)},
            {BW("phase", 2)},
        }};
        require_racy_replay(p,
                            {S(0, 0), S(1, 0), S(2, 0),
                             S(2, 1), S(3, 0), S(0, 1)},
                            "later generation re-released an earlier participant");
        require_barrier_agreement(p, "barrier reverse generation-leak oracle mismatch");
    }
    {
        // Resetting the object accumulator must not erase a real program-order
        // chain: T0 carries generation 0 into generation 1 through its own
        // joined thread clock, so T2's post-release read is ordered.
        Program p{{
            {W("x"), BW("phase", 2), BW("phase", 2)},
            {BW("phase", 2)},
            {BW("phase", 2), R("x")},
        }};
        require_clean_replay(p,
                             {S(0, 0), S(0, 1), S(1, 0),
                              S(0, 2), S(2, 0), S(2, 1)},
                             "actual cross-generation participant chain was lost");
        require_barrier_agreement(p, "barrier chained-generation oracle mismatch");
    }
    {
        // A successful TryLock acquires the preceding mutex release just like
        // Lock. Exercise both thread-id directions so the verdict cannot rely
        // on one deterministic exploration order.
        const Program low_to_high{{
            {L("m"), W("x"), U("m")},
            {TL("m", 0), R("x"), U("m")},
        }};
        require_clean_replay(low_to_high,
                             {S(0, 0), S(0, 1), S(0, 2),
                              S(1, 0), S(1, 1), S(1, 2)},
                             "successful TryLock lost the low-to-high release/acquire edge");
        require_trylock_agreement(low_to_high,
                                  "low-to-high TryLock HB oracle mismatch");

        const Program high_to_low{{
            {TL("m", 0), R("x"), U("m")},
            {L("m"), W("x"), U("m")},
        }};
        require_clean_replay(high_to_low,
                             {S(1, 0), S(1, 1), S(1, 2),
                              S(0, 0), S(0, 1), S(0, 2)},
                             "successful TryLock lost the high-to-low release/acquire edge");
        require_trylock_agreement(high_to_low,
                                  "high-to-low TryLock HB oracle mismatch");
    }
    {
        // The release clock remains stored while another thread holds m. A
        // failed TryLock must not acquire that stale prior release frontier.
        const Program p{{
            {L("m"), W("x"), U("m")},
            {L("m")},
            {TL("m", 0), R("x")},
        }};
        require_racy_replay(p,
                            {S(0, 0), S(0, 1), S(0, 2),
                             S(1, 0), S(2, 0), S(2, 1)},
                            "failed TryLock acquired a stale mutex release clock");
        require_trylock_agreement(p, "stale-release TryLock HB oracle mismatch");
    }
    {
        // Nor may failure acquire the current owner's live thread clock. The
        // holder has published nothing: schedule order alone leaves this read
        // unordered with its lock-held write.
        const Program p{{
            {L("m"), W("x"), U("m")},
            {TL("m", 0), R("x")},
        }};
        require_racy_replay(p,
                            {S(0, 0), S(0, 1), S(1, 0), S(1, 1)},
                            "failed TryLock leaked the live holder clock");
        require_trylock_agreement(p, "live-holder TryLock HB oracle mismatch");
    }
    {
        // Downgrade publishes the writer section at the conversion point.
        // The retained read hold overlaps the subsequent reader, so that
        // publication is the only HB path from the write to the peer's read.
        // Mirror the producer across thread ids to prevent deterministic
        // schedule order from masking a directional clock bug.
        const Program low_to_high{{
            {WL("rw"), W("x"), DOWN("rw"), RU("rw")},
            {RL("rw"), R("x"), RU("rw")},
        }};
        require_clean_replay(
            low_to_high,
            {S(0, 0), S(0, 1), S(0, 2),
             S(1, 0), S(1, 1), S(1, 2), S(0, 3)},
            "Downgrade lost the low-to-high writer publication edge");

        const Program high_to_low{{
            {RL("rw"), R("x"), RU("rw")},
            {WL("rw"), W("x"), DOWN("rw"), RU("rw")},
        }};
        require_clean_replay(
            high_to_low,
            {S(1, 0), S(1, 1), S(1, 2),
             S(0, 0), S(0, 1), S(0, 2), S(1, 3)},
            "Downgrade lost the high-to-low writer publication edge");
    }
    {
        // Upgrade consumes every other reader's release accumulator before
        // entering writer mode. The upgrader's retained read hold contributes
        // no release of its own, leaving this reader-release join as the only
        // HB path from the peer access to the upgrader's conflicting write.
        const Program low_to_high{{
            {RL("rw"), R("x"), RU("rw")},
            {RL("rw"), UP("rw"), W("x"), WU("rw")},
        }};
        require_clean_replay(
            low_to_high,
            {S(1, 0), S(0, 0), S(0, 1), S(0, 2),
             S(1, 1), S(1, 2), S(1, 3)},
            "Upgrade lost the low-to-high reader-accumulator edge");

        const Program high_to_low{{
            {RL("rw"), UP("rw"), W("x"), WU("rw")},
            {RL("rw"), R("x"), RU("rw")},
        }};
        require_clean_replay(
            high_to_low,
            {S(0, 0), S(1, 0), S(1, 1), S(1, 2),
             S(0, 1), S(0, 2), S(0, 3)},
            "Upgrade lost the high-to-low reader-accumulator edge");
    }
    {
        // A plain reader release must still not synchronize a later reader.
        // Schedule order alone creates no HB edge, so this directly guards
        // against leaking the reader-release accumulator into RLock.
        const Program p{{
            {RL("rw"), W("x"), RU("rw")},
            {RL("rw"), R("x"), RU("rw")},
        }};
        require_racy_replay(
            p,
            {S(0, 0), S(0, 1), S(0, 2),
             S(1, 0), S(1, 1), S(1, 2)},
            "rwlock conversion clocks introduced a phantom reader-reader edge");
    }
    std::cout << "ALL CV PROBES PASSED\n";
    return 0;
}
