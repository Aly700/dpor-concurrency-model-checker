#include "model/checker.hpp"

#include <cassert>
#include <iostream>

using namespace model;

static Action R(std::string a) { Action x; x.kind = ActionKind::Read; x.address = std::move(a); return x; }
static Action W(std::string a) { Action x; x.kind = ActionKind::Write; x.address = std::move(a); return x; }
static Action L(std::string m) { Action x; x.kind = ActionKind::Lock; x.mutex = std::move(m); return x; }
static Action U(std::string m) { Action x; x.kind = ActionKind::Unlock; x.mutex = std::move(m); return x; }
static Action WAIT(std::string cv, std::string m) { Action x; x.kind = ActionKind::Wait; x.condition = std::move(cv); x.mutex = std::move(m); return x; }
static Action SIG(std::string cv) { Action x; x.kind = ActionKind::Signal; x.condition = std::move(cv); return x; }

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
    std::cout << "ALL CV PROBES PASSED\n";
    return 0;
}
