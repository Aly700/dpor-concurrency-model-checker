#include "model/checker.hpp"
#include "program_parser.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace cli {

model::Program parse_program_text(const std::string& text);
std::string render_program(const model::Program& program);

} // namespace cli

namespace {

enum class GenerationMode { MostlyWellFormed, Adversarial, Value };

enum class Choice {
    PlainMemory,
    AtomicMemory,
    Lock,
    TryLock,
    Unlock,
    RLock,
    RUnlock,
    WLock,
    WUnlock,
    SemPost,
    SemWait,
    BarrierWait,
    Wait,
    Signal,
    Broadcast,
    Spawn,
    Join,
    Fence,
    Yield
};

struct WeightedChoice {
    Choice choice;
    int weight{0};
};

struct FuzzStats {
    std::size_t total{0};
    std::size_t checked{0};
    std::size_t skipped{0};
    std::size_t race{0};
    std::size_t deadlock{0};
    std::size_t error{0};
    std::size_t assertion{0};
    std::size_t cycle{0};
    std::size_t bound_hit{0};
    std::size_t tso{0};
    std::size_t pso{0};
    std::size_t naive_schedules{0};
    std::size_t dpor_schedules{0};
    std::size_t naive_cycles{0};
    std::size_t dpor_cycles{0};
    std::size_t naive_fair_cycles{0};
    std::size_t dpor_fair_cycles{0};
    std::size_t naive_strongly_unfair_cycles{0};
    std::size_t dpor_strongly_unfair_cycles{0};
    std::size_t naive_unfair_cycles{0};
    std::size_t dpor_unfair_cycles{0};
    std::array<std::size_t, 4> rwlock_actions{};
    // Rows are MostlyWellFormed/Adversarial; columns are SemPost/SemWait.
    std::array<std::array<std::size_t, 2>, 2> semaphore_actions{};
    // MostlyWellFormed/Adversarial BarrierWait counts.
    std::array<std::size_t, 2> barrier_waits{};
    // MostlyWellFormed/Adversarial TryLock counts. "Compared" excludes
    // programs discarded because either explorer reached the schedule cap.
    std::array<std::size_t, 2> try_lock_actions_generated{};
    std::array<std::size_t, 2> try_lock_actions_compared{};
};

model::Action read(std::string address) {
    model::Action action;
    action.kind = model::ActionKind::Read;
    action.address = std::move(address);
    return action;
}

model::Action write(std::string address) {
    model::Action action;
    action.kind = model::ActionKind::Write;
    action.address = std::move(address);
    return action;
}

model::Action atomic_load(std::string address) {
    model::Action action;
    action.kind = model::ActionKind::AtomicLoad;
    action.address = std::move(address);
    return action;
}

model::Action atomic_store(std::string address) {
    model::Action action;
    action.kind = model::ActionKind::AtomicStore;
    action.address = std::move(address);
    return action;
}

model::Action atomic_rmw(std::string address) {
    model::Action action;
    action.kind = model::ActionKind::AtomicRmw;
    action.address = std::move(address);
    return action;
}

model::Action lock(std::string mutex) {
    model::Action action;
    action.kind = model::ActionKind::Lock;
    action.mutex = std::move(mutex);
    return action;
}

model::Action try_lock(std::string mutex, model::RegisterId destination) {
    model::Action action;
    action.kind = model::ActionKind::TryLock;
    action.mutex = std::move(mutex);
    action.destination = destination;
    return action;
}

model::Action unlock(std::string mutex) {
    model::Action action;
    action.kind = model::ActionKind::Unlock;
    action.mutex = std::move(mutex);
    return action;
}

model::Action rwlock_action(model::ActionKind kind, std::string rwlock) {
    model::Action action;
    action.kind = kind;
    action.rwlock = std::move(rwlock);
    return action;
}

model::Action rlock(std::string rwlock) {
    return rwlock_action(model::ActionKind::RLock, std::move(rwlock));
}

model::Action runlock(std::string rwlock) {
    return rwlock_action(model::ActionKind::RUnlock, std::move(rwlock));
}

model::Action wlock(std::string rwlock) {
    return rwlock_action(model::ActionKind::WLock, std::move(rwlock));
}

model::Action wunlock(std::string rwlock) {
    return rwlock_action(model::ActionKind::WUnlock, std::move(rwlock));
}

model::Action semaphore_action(model::ActionKind kind, std::string semaphore) {
    model::Action action;
    action.kind = kind;
    action.semaphore = std::move(semaphore);
    return action;
}

model::Action sem_post(std::string semaphore) {
    return semaphore_action(model::ActionKind::SemPost, std::move(semaphore));
}

model::Action sem_wait(std::string semaphore) {
    return semaphore_action(model::ActionKind::SemWait, std::move(semaphore));
}

model::Action barrier_wait(std::string barrier, std::uint32_t parties) {
    model::Action action;
    action.kind = model::ActionKind::BarrierWait;
    action.barrier = std::move(barrier);
    action.parties = parties;
    return action;
}

model::Action wait(std::string condition, std::string mutex) {
    model::Action action;
    action.kind = model::ActionKind::Wait;
    action.condition = std::move(condition);
    action.mutex = std::move(mutex);
    return action;
}

model::Action signal(std::string condition) {
    model::Action action;
    action.kind = model::ActionKind::Signal;
    action.condition = std::move(condition);
    return action;
}

model::Action broadcast(std::string condition) {
    model::Action action;
    action.kind = model::ActionKind::Broadcast;
    action.condition = std::move(condition);
    return action;
}

model::Action join(model::ThreadId target) {
    model::Action action;
    action.kind = model::ActionKind::Join;
    action.target = target;
    return action;
}

model::Action spawn(model::ThreadId target) {
    model::Action action;
    action.kind = model::ActionKind::Spawn;
    action.target = target;
    return action;
}

model::Action yield() {
    model::Action action;
    action.kind = model::ActionKind::Yield;
    return action;
}

model::Action fence() {
    model::Action action;
    action.kind = model::ActionKind::Fence;
    return action;
}

model::ValueOperand imm(model::Value value) {
    model::ValueOperand operand;
    operand.kind = model::ValueOperandKind::Immediate;
    operand.immediate = value;
    return operand;
}

model::Action set(model::RegisterId reg, model::Value value) {
    model::Action action;
    action.kind = model::ActionKind::Set;
    action.destination = reg;
    action.value = imm(value);
    return action;
}

model::Action label(std::string name) {
    model::Action action;
    action.kind = model::ActionKind::Label;
    action.label = std::move(name);
    return action;
}

model::Action bnz(model::RegisterId reg, std::string target) {
    model::Action action;
    action.kind = model::ActionKind::BranchNonzero;
    action.source_register = reg;
    action.label = std::move(target);
    return action;
}

model::Action assert_nonzero(model::RegisterId reg) {
    model::Action action;
    action.kind = model::ActionKind::Assert;
    action.source_register = reg;
    return action;
}

model::Action read_to(std::string address, model::RegisterId destination) {
    model::Action action = read(std::move(address));
    action.destination = destination;
    return action;
}

model::Action write_value(std::string address, model::ValueOperand value) {
    model::Action action = write(std::move(address));
    action.value = value;
    return action;
}

model::Action atomic_load_to(std::string address, model::RegisterId destination) {
    model::Action action = atomic_load(std::move(address));
    action.destination = destination;
    return action;
}

model::Action atomic_store_value(std::string address, model::ValueOperand value) {
    model::Action action = atomic_store(std::move(address));
    action.value = value;
    return action;
}

model::Action atomic_rmw_value(std::string address, model::ValueOperand value, model::RegisterId destination) {
    model::Action action = atomic_rmw(std::move(address));
    action.value = value;
    action.destination = destination;
    return action;
}

model::Action cas(std::string address,
                  model::ValueOperand expected,
                  model::ValueOperand desired,
                  model::RegisterId destination) {
    model::Action action;
    action.kind = model::ActionKind::CompareExchange;
    action.address = std::move(address);
    action.expected = expected;
    action.value = desired;
    action.destination = destination;
    return action;
}

std::size_t bounded(std::mt19937_64& rng, std::size_t limit) {
    assert(limit > 0);
    return std::uniform_int_distribution<std::size_t>(0, limit - 1)(rng);
}

std::string hex_seed(std::uint64_t seed) {
    std::ostringstream output;
    output << "0x" << std::hex << seed;
    return output.str();
}

const char* mode_name(GenerationMode mode) {
    switch (mode) {
    case GenerationMode::MostlyWellFormed:
        return "mostly-well-formed";
    case GenerationMode::Adversarial:
        return "adversarial";
    case GenerationMode::Value:
        return "value";
    }
    return "unknown";
}

const char* memory_model_name(model::MemoryModel memory_model) {
    switch (memory_model) {
    case model::MemoryModel::SC:
        return "sc";
    case model::MemoryModel::TSO:
        return "tso";
    case model::MemoryModel::PSO:
        return "pso";
    }
    return "unknown";
}

Choice choose(std::mt19937_64& rng, const std::vector<WeightedChoice>& choices) {
    int total = 0;
    for (const auto& choice : choices) {
        assert(choice.weight > 0);
        total += choice.weight;
    }

    int selected = std::uniform_int_distribution<int>(1, total)(rng);
    for (const auto& choice : choices) {
        selected -= choice.weight;
        if (selected <= 0) {
            return choice.choice;
        }
    }
    return choices.back().choice;
}

model::Action random_plain_memory(std::mt19937_64& rng) {
    const std::string address = bounded(rng, 2) == 0 ? "x" : "y";
    return bounded(rng, 2) == 0 ? read(address) : write(address);
}

model::Action random_atomic_memory(std::mt19937_64& rng) {
    const std::string address = bounded(rng, 2) == 0 ? "x" : "y";
    switch (bounded(rng, 3)) {
    case 0:
        return atomic_load(address);
    case 1:
        return atomic_store(address);
    default:
        return atomic_rmw(address);
    }
}

std::string random_mutex(std::mt19937_64& rng) {
    return bounded(rng, 2) == 0 ? "m" : "n";
}

std::string random_rwlock(std::mt19937_64& rng) {
    // Deliberately disjoint from the mutex namespace so generated programs
    // exercise rwlock semantics instead of being rejected by validation.
    return bounded(rng, 2) == 0 ? "rw0" : "rw1";
}

std::string random_semaphore(std::mt19937_64& rng) {
    // Keep generated semaphore names disjoint from mutexes and rwlocks so the
    // ordinary fuzz lanes exercise semaphore semantics rather than validation.
    return bounded(rng, 2) == 0 ? "sem0" : "sem1";
}

model::Action random_barrier(std::mt19937_64& rng) {
    // The name fixes the party count program-wide, so generated programs
    // exercise barrier behavior instead of the separate static-mismatch path.
    return bounded(rng, 2) == 0 ? barrier_wait("bar0", 2)
                                : barrier_wait("bar1", 3);
}

std::string random_condition(std::mt19937_64& rng, std::size_t condition_count) {
    assert(condition_count >= 1 && condition_count <= 2);
    if (condition_count == 1 || bounded(rng, condition_count) == 0) {
        return "cv0";
    }
    return "cv1";
}

model::ThreadId random_other_thread(std::mt19937_64& rng,
                                    model::ThreadId self,
                                    std::size_t thread_count) {
    assert(thread_count >= 2);
    model::ThreadId target = static_cast<model::ThreadId>(bounded(rng, thread_count - 1));
    if (target >= self) {
        ++target;
    }
    return target;
}

bool contains(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

struct HeldRwLock {
    std::string name;
    bool writer{false};
};

bool contains_rwlock(const std::vector<HeldRwLock>& held, const std::string& name) {
    return std::find_if(held.begin(), held.end(), [&](const HeldRwLock& entry) {
               return entry.name == name;
           }) != held.end();
}

std::size_t random_rwlock_with_mode(std::mt19937_64& rng,
                                    const std::vector<HeldRwLock>& held,
                                    bool writer) {
    std::vector<std::size_t> matches;
    for (std::size_t index = 0; index < held.size(); ++index) {
        if (held.at(index).writer == writer) {
            matches.push_back(index);
        }
    }
    assert(!matches.empty());
    return matches.at(bounded(rng, matches.size()));
}

void erase_one(std::vector<std::string>& values, std::size_t index) {
    assert(index < values.size());
    values.erase(values.begin() + static_cast<std::ptrdiff_t>(index));
}

model::Action acquire_rwlock(std::mt19937_64& rng,
                             std::vector<HeldRwLock>& held_rwlocks,
                             bool writer) {
    std::string rwlock = random_rwlock(rng);
    if (contains_rwlock(held_rwlocks, rwlock)) {
        rwlock = rwlock == "rw0" ? "rw1" : "rw0";
    }
    assert(!contains_rwlock(held_rwlocks, rwlock));
    held_rwlocks.push_back(HeldRwLock{rwlock, writer});
    return writer ? wlock(std::move(rwlock)) : rlock(std::move(rwlock));
}

model::Action release_rwlock(std::mt19937_64& rng,
                             std::vector<HeldRwLock>& held_rwlocks,
                             bool writer) {
    const std::size_t index = random_rwlock_with_mode(rng, held_rwlocks, writer);
    std::string rwlock = held_rwlocks.at(index).name;
    held_rwlocks.erase(held_rwlocks.begin() + static_cast<std::ptrdiff_t>(index));
    return writer ? wunlock(std::move(rwlock)) : runlock(std::move(rwlock));
}

model::Action generate_mostly_well_formed_action(std::mt19937_64& rng,
                                                  model::ThreadId tid,
                                                  std::size_t thread_count,
                                                  std::size_t condition_count,
                                                  std::size_t slots_left,
                                                  std::vector<std::string>& held_mutexes,
                                                  std::vector<HeldRwLock>& held_rwlocks) {
    const std::size_t held_count = held_mutexes.size() + held_rwlocks.size();
    if (slots_left == held_count) {
        const std::size_t selected = bounded(rng, held_count);
        if (selected < held_mutexes.size()) {
            std::string mutex = held_mutexes.at(selected);
            erase_one(held_mutexes, selected);
            return unlock(std::move(mutex));
        }
        const std::size_t rw_index = selected - held_mutexes.size();
        HeldRwLock held = held_rwlocks.at(rw_index);
        held_rwlocks.erase(
            held_rwlocks.begin() + static_cast<std::ptrdiff_t>(rw_index));
        return held.writer ? wunlock(std::move(held.name))
                           : runlock(std::move(held.name));
    }

    std::vector<WeightedChoice> choices = {
        {Choice::PlainMemory, 10},
        {Choice::AtomicMemory, 10},
        {Choice::TryLock, 18},
        {Choice::Signal, 14},
        {Choice::Broadcast, 10},
        {Choice::Spawn, 10},
        {Choice::Join, 14},
        {Choice::SemPost, 10},
        {Choice::SemWait, 10},
        {Choice::BarrierWait, 12},
        {Choice::Fence, 5},
        {Choice::Yield, 4},
    };

    if (slots_left >= held_count + 2 && held_mutexes.size() < 2) {
        choices.push_back({Choice::Lock, 25});
    }
    if (slots_left >= held_count + 2 && held_rwlocks.size() < 2) {
        choices.push_back({Choice::RLock, 12});
        choices.push_back({Choice::WLock, 12});
    }
    if (!held_mutexes.empty()) {
        choices.push_back({Choice::Unlock, 8});
        choices.push_back({Choice::Wait, 24});
    }
    if (std::any_of(held_rwlocks.begin(), held_rwlocks.end(),
                    [](const HeldRwLock& held) { return !held.writer; })) {
        choices.push_back({Choice::RUnlock, 8});
    }
    if (std::any_of(held_rwlocks.begin(), held_rwlocks.end(),
                    [](const HeldRwLock& held) { return held.writer; })) {
        choices.push_back({Choice::WUnlock, 8});
    }

    switch (choose(rng, choices)) {
    case Choice::PlainMemory:
        return random_plain_memory(rng);
    case Choice::AtomicMemory:
        return random_atomic_memory(rng);
    case Choice::Lock: {
        std::string mutex = random_mutex(rng);
        if (contains(held_mutexes, mutex)) {
            mutex = mutex == "m" ? "n" : "m";
        }
        assert(!contains(held_mutexes, mutex));
        held_mutexes.push_back(mutex);
        return lock(std::move(mutex));
    }
    case Choice::TryLock:
        return try_lock(
            random_mutex(rng),
            static_cast<model::RegisterId>(bounded(rng, model::kRegisterCount)));
    case Choice::Unlock: {
        const std::size_t index = bounded(rng, held_mutexes.size());
        std::string mutex = held_mutexes.at(index);
        erase_one(held_mutexes, index);
        return unlock(std::move(mutex));
    }
    case Choice::RLock:
        return acquire_rwlock(rng, held_rwlocks, false);
    case Choice::WLock:
        return acquire_rwlock(rng, held_rwlocks, true);
    case Choice::RUnlock:
        return release_rwlock(rng, held_rwlocks, false);
    case Choice::WUnlock:
        return release_rwlock(rng, held_rwlocks, true);
    case Choice::SemPost:
        return sem_post(random_semaphore(rng));
    case Choice::SemWait:
        return sem_wait(random_semaphore(rng));
    case Choice::BarrierWait:
        return random_barrier(rng);
    case Choice::Wait:
        return wait(random_condition(rng, condition_count),
                    held_mutexes.at(bounded(rng, held_mutexes.size())));
    case Choice::Signal:
        return signal(random_condition(rng, condition_count));
    case Choice::Broadcast:
        return broadcast(random_condition(rng, condition_count));
    case Choice::Spawn:
        return spawn(random_other_thread(rng, tid, thread_count));
    case Choice::Join:
        return join(random_other_thread(rng, tid, thread_count));
    case Choice::Fence:
        return fence();
    case Choice::Yield:
        return yield();
    }

    return yield();
}

model::Action generate_adversarial_action(std::mt19937_64& rng,
                                          model::ThreadId tid,
                                          std::size_t thread_count,
                                          std::size_t condition_count) {
    // The adversarial lane deliberately injects unbalanced mutex/rwlock
    // unlocks, waits without owning the mutex, and self-joins to exercise
    // modeled-error paths.
    const std::vector<WeightedChoice> choices = {
        {Choice::PlainMemory, 16},
        {Choice::AtomicMemory, 16},
        {Choice::Lock, 12},
        {Choice::TryLock, 40},
        {Choice::Unlock, 18},
        {Choice::RLock, 8},
        {Choice::RUnlock, 10},
        {Choice::WLock, 8},
        {Choice::WUnlock, 10},
        {Choice::Wait, 18},
        {Choice::Signal, 8},
        {Choice::Broadcast, 6},
        {Choice::Spawn, 12},
        {Choice::Join, 14},
        {Choice::SemPost, 8},
        {Choice::SemWait, 12},
        {Choice::BarrierWait, 10},
        {Choice::Fence, 5},
        {Choice::Yield, 4},
    };

    switch (choose(rng, choices)) {
    case Choice::PlainMemory:
        return random_plain_memory(rng);
    case Choice::AtomicMemory:
        return random_atomic_memory(rng);
    case Choice::Lock:
        return lock(random_mutex(rng));
    case Choice::TryLock:
        return try_lock(
            random_mutex(rng),
            static_cast<model::RegisterId>(bounded(rng, model::kRegisterCount)));
    case Choice::Unlock:
        return unlock(random_mutex(rng));
    case Choice::RLock:
        return rlock(random_rwlock(rng));
    case Choice::RUnlock:
        return runlock(random_rwlock(rng));
    case Choice::WLock:
        return wlock(random_rwlock(rng));
    case Choice::WUnlock:
        return wunlock(random_rwlock(rng));
    case Choice::SemPost:
        return sem_post(random_semaphore(rng));
    case Choice::SemWait:
        return sem_wait(random_semaphore(rng));
    case Choice::BarrierWait:
        return random_barrier(rng);
    case Choice::Wait:
        return wait(random_condition(rng, condition_count), random_mutex(rng));
    case Choice::Signal:
        return signal(random_condition(rng, condition_count));
    case Choice::Broadcast:
        return broadcast(random_condition(rng, condition_count));
    case Choice::Spawn:
        return bounded(rng, 4) == 0
            ? spawn(tid)
            : spawn(static_cast<model::ThreadId>(bounded(rng, thread_count)));
    case Choice::Join:
        return bounded(rng, 3) == 0
            ? join(tid)
            : join(static_cast<model::ThreadId>(bounded(rng, thread_count)));
    case Choice::Fence:
        return fence();
    case Choice::Yield:
        return yield();
    }

    return yield();
}

model::Program generate_value_program(std::mt19937_64& rng) {
    switch (bounded(rng, 7)) {
    case 0: {
        const bool failing_assertion = bounded(rng, 4) == 0;
        return model::Program{{
            {
                set(0, 1),
                set(1, static_cast<model::Value>(bounded(rng, 4) + 1)),
                failing_assertion ? assert_nonzero(7) : assert_nonzero(0),
            },
            {set(2, 1), bnz(2, "done"), assert_nonzero(7), label("done"), assert_nonzero(2)},
        }};
    }
    case 1:
        return model::Program{{
            {atomic_rmw_value("f", imm(1), 0)},
            {atomic_rmw_value("f", imm(1), 0)},
            {join(0), join(1), cas("f", imm(2), imm(2), 1), assert_nonzero(1)},
        }};
    case 2:
        return model::Program{{
            {atomic_store_value("f", imm(1))},
            {write_value("x", imm(1)), cas("f", imm(0), imm(2), 0)},
            {atomic_load_to("f", 1), write_value("x", imm(2))},
        }};
    case 3:
        return model::Program{{
            {write_value("x", imm(1)), cas("f", imm(0), imm(1), 0), assert_nonzero(0)},
            {atomic_load_to("f", 1), read_to("x", 2)},
        }};
    case 4:
        return model::Program{{
            {
                set(1, 1),
                label("spin"),
                atomic_load_to("f", 0),
                bnz(0, "done"),
                bnz(1, "spin"),
                label("done"),
                assert_nonzero(1),
            },
            {atomic_store_value("f", imm(1))},
        }};
    case 5:
        // Growing shared state prevents an exact cycle, so this lane keeps the
        // step bound exercised as the required non-repeating backstop.
        return model::Program{{
            {
                set(1, 1),
                label("grow"),
                atomic_rmw_value("counter", imm(1), 0),
                bnz(1, "grow"),
            },
        }};
    default:
        return model::Program{{
            {set(1, 1), label("spin"), bnz(1, "spin")},
            {yield()},
        }};
    }
}

std::size_t generated_thread_count(std::mt19937_64& rng) {
    constexpr std::size_t kThreadCounts[] = {
        2, 2, 2, 2, 2, 2, 2, 2,
        2, 2, 2, 2, 2, 2,
        3, 3, 3, 3,
        4,
        5,
    };
    return kThreadCounts[bounded(rng, std::size(kThreadCounts))];
}

std::size_t generated_action_count(std::mt19937_64& rng, std::size_t thread_count) {
    if (thread_count >= 4) {
        constexpr std::size_t kLargeThreadActionCounts[] = {
            1, 1, 1, 1, 1, 1, 1, 1,
            2, 2, 2, 2,
            3, 3,
            4,
            5,
            6,
        };
        return kLargeThreadActionCounts[bounded(rng, std::size(kLargeThreadActionCounts))];
    }

    constexpr std::size_t kActionCounts[] = {
        1, 1, 1, 1, 1, 1, 1, 1,
        2, 2, 2, 2, 2, 2,
        3, 3,
        4,
        5,
        6,
    };
    return kActionCounts[bounded(rng, std::size(kActionCounts))];
}

model::Program generate_program(std::mt19937_64& rng, GenerationMode mode) {
    if (mode == GenerationMode::Value) {
        return generate_value_program(rng);
    }

    const std::size_t thread_count = generated_thread_count(rng);
    const std::size_t condition_count = bounded(rng, 2) + 1;

    model::Program program;
    program.threads.resize(thread_count);
    for (std::size_t tid_index = 0; tid_index < thread_count; ++tid_index) {
        const auto tid = static_cast<model::ThreadId>(tid_index);
        const std::size_t action_count = generated_action_count(rng, thread_count);
        std::vector<std::string> held_mutexes;
        std::vector<HeldRwLock> held_rwlocks;
        for (std::size_t action_index = 0; action_index < action_count; ++action_index) {
            const std::size_t slots_left = action_count - action_index;
            if (mode == GenerationMode::MostlyWellFormed) {
                program.threads.at(tid).push_back(
                    generate_mostly_well_formed_action(
                        rng,
                        tid,
                        thread_count,
                        condition_count,
                        slots_left,
                        held_mutexes,
                        held_rwlocks));
            } else {
                program.threads.at(tid).push_back(
                    generate_adversarial_action(rng, tid, thread_count, condition_count));
            }
        }
        assert(mode == GenerationMode::Adversarial || held_mutexes.empty());
        assert(mode == GenerationMode::Adversarial || held_rwlocks.empty());
    }
    return program;
}

bool hit_cap(const model::CheckResult& result, std::size_t cap) {
    return result.schedules_explored >= cap;
}

bool hit_step_bound(const model::CheckResult& result) {
    return result.bound_exceeded_executions > 0;
}

bool cycle_exists(const model::CheckResult& result) {
    return result.cycles_detected > 0;
}

bool fair_cycle_exists(const model::CheckResult& result) {
    return result.fair_cycles > 0;
}

bool strongly_unfair_cycle_exists(const model::CheckResult& result) {
    return result.strongly_unfair_cycles > 0;
}

bool unfair_cycle_exists(const model::CheckResult& result) {
    return result.unfair_cycles > 0;
}

void print_failure(std::uint64_t seed,
                   std::size_t program_index,
                   GenerationMode mode,
                   model::MemoryModel memory_model,
                   const model::Program& program,
                   const model::CheckResult& naive,
                   const model::CheckResult& dpor,
                   const std::string& reason) {
    std::cerr << "differential fuzz failure: " << reason << '\n';
    std::cerr << "seed: " << hex_seed(seed) << '\n';
    std::cerr << "program_index: " << program_index << '\n';
    std::cerr << "mode: " << mode_name(mode) << '\n';
    std::cerr << "memory_model: " << memory_model_name(memory_model) << '\n';
    std::cerr << "naive schedules=" << naive.schedules_explored
              << " race=" << naive.first_race.has_value()
              << " deadlock=" << naive.first_deadlock.has_value()
              << " error=" << naive.first_error.has_value()
              << " assertion=" << naive.first_assertion.has_value()
              << " cycle=" << cycle_exists(naive)
              << " cycles_detected=" << naive.cycles_detected
              << " fair_cycle=" << fair_cycle_exists(naive)
              << " fair_cycles=" << naive.fair_cycles
              << " strongly_unfair_cycle="
              << strongly_unfair_cycle_exists(naive)
              << " strongly_unfair_cycles="
              << naive.strongly_unfair_cycles
              << " unfair_cycle=" << unfair_cycle_exists(naive)
              << " unfair_cycles=" << naive.unfair_cycles
              << " bound=" << hit_step_bound(naive) << '\n';
    std::cerr << "dpor schedules=" << dpor.schedules_explored
              << " race=" << dpor.first_race.has_value()
              << " deadlock=" << dpor.first_deadlock.has_value()
              << " error=" << dpor.first_error.has_value()
              << " assertion=" << dpor.first_assertion.has_value()
              << " cycle=" << cycle_exists(dpor)
              << " cycles_detected=" << dpor.cycles_detected
              << " fair_cycle=" << fair_cycle_exists(dpor)
              << " fair_cycles=" << dpor.fair_cycles
              << " strongly_unfair_cycle="
              << strongly_unfair_cycle_exists(dpor)
              << " strongly_unfair_cycles="
              << dpor.strongly_unfair_cycles
              << " unfair_cycle=" << unfair_cycle_exists(dpor)
              << " unfair_cycles=" << dpor.unfair_cycles
              << " bound=" << hit_step_bound(dpor) << '\n';
    std::cerr << "program.dpor:\n" << cli::render_program(program);
}

void fail_program(std::uint64_t seed,
                  std::size_t program_index,
                  GenerationMode mode,
                  model::MemoryModel memory_model,
                  const model::Program& program,
                  const model::CheckResult& naive,
                  const model::CheckResult& dpor,
                  const std::string& reason) {
    print_failure(seed, program_index, mode, memory_model, program, naive, dpor, reason);
    std::abort();
}

void assert_replays_dpor_report(std::uint64_t seed,
                                std::size_t program_index,
                                GenerationMode mode,
                                model::MemoryModel memory_model,
                                const model::Program& program,
                                const model::ModelChecker& checker,
                                const model::CheckResult& naive,
                                const model::CheckResult& dpor) {
    if (dpor.first_race.has_value()) {
        const auto replayed = checker.replay(dpor.first_race->schedule);
        if (!replayed.first_race.has_value() || *replayed.first_race != *dpor.first_race) {
            fail_program(seed, program_index, mode, memory_model, program, naive, dpor, "race replay changed report");
        }
    }
    if (dpor.first_deadlock.has_value()) {
        const auto replayed = checker.replay(dpor.first_deadlock->schedule);
        if (!replayed.first_deadlock.has_value() ||
            *replayed.first_deadlock != *dpor.first_deadlock) {
            fail_program(seed, program_index, mode, memory_model, program, naive, dpor, "deadlock replay changed report");
        }
    }
    if (dpor.first_error.has_value()) {
        const auto replayed = checker.replay(dpor.first_error->schedule);
        if (!replayed.first_error.has_value() || *replayed.first_error != *dpor.first_error) {
            fail_program(seed, program_index, mode, memory_model, program, naive, dpor, "error replay changed report");
        }
    }
    if (dpor.first_assertion.has_value()) {
        const auto replayed = checker.replay(dpor.first_assertion->schedule);
        if (!replayed.first_assertion.has_value() ||
            *replayed.first_assertion != *dpor.first_assertion) {
            fail_program(seed, program_index, mode, memory_model, program, naive, dpor, "assertion replay changed report");
        }
    }
    if (dpor.first_nontermination.has_value()) {
        const auto replayed = checker.replay(dpor.first_nontermination->schedule);
        if (!replayed.first_nontermination.has_value() ||
            *replayed.first_nontermination != *dpor.first_nontermination) {
            fail_program(seed,
                         program_index,
                         mode,
                         memory_model,
                         program,
                         naive,
                         dpor,
                         "nontermination replay changed report");
        }
    }
}

void assert_round_trips(std::uint64_t seed,
                        std::size_t program_index,
                        GenerationMode mode,
                        const model::Program& program) {
    std::vector<std::size_t> spawn_targets(program.threads.size(), 0);
    for (std::size_t tid_index = 0; tid_index < program.threads.size(); ++tid_index) {
        const auto tid = static_cast<model::ThreadId>(tid_index);
        for (const model::Action& action : program.threads.at(tid)) {
            if (action.kind != model::ActionKind::Spawn) {
                continue;
            }
            if (action.target >= program.threads.size() || action.target == tid) {
                return;
            }
            ++spawn_targets.at(action.target);
            if (spawn_targets.at(action.target) > 1) {
                return;
            }
        }
    }

    const std::string rendered = cli::render_program(program);
    const model::Program parsed = cli::parse_program_text(rendered);
    if (parsed.threads != program.threads) {
        const model::CheckResult empty_naive;
        const model::CheckResult empty_dpor;
        print_failure(seed,
                      program_index,
                      mode,
                      model::MemoryModel::SC,
                      program,
                      empty_naive,
                      empty_dpor,
                      "CLI parse/render changed program");
        std::cerr << "round_trip.dpor:\n" << cli::render_program(parsed);
        assert(false && "CLI parse/render round trip failed");
    }
}

void assert_rwlock_spellings_round_trip() {
    const model::Program program{{{
        rlock("rw0"),
        runlock("rw0"),
        wlock("rw1"),
        wunlock("rw1"),
    }}};
    const std::string rendered = cli::render_program(program);
    assert(rendered.find("  rlock rw0\n") != std::string::npos);
    assert(rendered.find("  runlock rw0\n") != std::string::npos);
    assert(rendered.find("  wlock rw1\n") != std::string::npos);
    assert(rendered.find("  wunlock rw1\n") != std::string::npos);
    assert(cli::parse_program_text(rendered).threads == program.threads);
}

void assert_semaphore_spellings_round_trip() {
    const model::Program program{{{
        sem_post("sem0"),
        sem_wait("sem1"),
    }}};
    const std::string rendered = cli::render_program(program);
    if (rendered !=
            "thread:\n"
            "  sem_post sem0\n"
            "  sem_wait sem1\n" ||
        cli::parse_program_text(rendered).threads != program.threads) {
        throw std::runtime_error("semaphore spellings did not round-trip exactly");
    }
}

void assert_barrier_spelling_round_trips() {
    const model::Program program{{{
        barrier_wait("bar0", 2),
        barrier_wait("bar1", 3),
    }}};
    const std::string rendered = cli::render_program(program);
    if (rendered !=
            "thread:\n"
            "  barrier_wait bar0 2\n"
            "  barrier_wait bar1 3\n" ||
        cli::parse_program_text(rendered).threads != program.threads) {
        throw std::runtime_error("barrier spelling did not round-trip exactly");
    }
}

void assert_try_lock_spelling_round_trips() {
    const model::Program program{{{
        try_lock("m", 3),
    }}};
    const std::string rendered = cli::render_program(program);
    if (rendered !=
            "thread:\n"
            "  try_lock m -> r3\n" ||
        cli::parse_program_text(rendered).threads != program.threads) {
        throw std::runtime_error("TryLock spelling did not round-trip exactly");
    }
}

void check_program(std::uint64_t seed,
                   std::size_t program_index,
                   GenerationMode mode,
                   model::MemoryModel memory_model,
                   const model::Program& program,
                   FuzzStats& stats) {
    constexpr std::size_t kMaxSchedules = 20000;
    constexpr std::size_t kStepBound = 40;
    const model::ModelChecker checker(program, kStepBound, memory_model);
    const model::CheckResult naive = checker.explore_naive(kMaxSchedules);
    const model::CheckResult dpor = checker.explore_dpor(kMaxSchedules);

    ++stats.total;
    std::size_t try_lock_actions_in_program = 0;
    for (const auto& thread : program.threads) {
        for (const model::Action& action : thread) {
            switch (action.kind) {
            case model::ActionKind::TryLock:
                ++try_lock_actions_in_program;
                if (mode != GenerationMode::Value) {
                    ++stats.try_lock_actions_generated[
                        mode == GenerationMode::MostlyWellFormed ? 0 : 1];
                }
                break;
            case model::ActionKind::RLock:
                ++stats.rwlock_actions[0];
                break;
            case model::ActionKind::RUnlock:
                ++stats.rwlock_actions[1];
                break;
            case model::ActionKind::WLock:
                ++stats.rwlock_actions[2];
                break;
            case model::ActionKind::WUnlock:
                ++stats.rwlock_actions[3];
                break;
            case model::ActionKind::SemPost:
                if (mode != GenerationMode::Value) {
                    ++stats.semaphore_actions[
                        mode == GenerationMode::MostlyWellFormed ? 0 : 1][0];
                }
                break;
            case model::ActionKind::SemWait:
                if (mode != GenerationMode::Value) {
                    ++stats.semaphore_actions[
                        mode == GenerationMode::MostlyWellFormed ? 0 : 1][1];
                }
                break;
            case model::ActionKind::BarrierWait:
                if (mode != GenerationMode::Value) {
                    ++stats.barrier_waits[
                        mode == GenerationMode::MostlyWellFormed ? 0 : 1];
                }
                break;
            default:
                break;
            }
        }
    }
    if (memory_model == model::MemoryModel::TSO) {
        ++stats.tso;
    } else if (memory_model == model::MemoryModel::PSO) {
        ++stats.pso;
    }
    stats.naive_schedules += naive.schedules_explored;
    stats.dpor_schedules += dpor.schedules_explored;
    stats.naive_cycles += naive.cycles_detected;
    stats.dpor_cycles += dpor.cycles_detected;
    stats.naive_fair_cycles += naive.fair_cycles;
    stats.dpor_fair_cycles += dpor.fair_cycles;
    stats.naive_strongly_unfair_cycles += naive.strongly_unfair_cycles;
    stats.dpor_strongly_unfair_cycles += dpor.strongly_unfair_cycles;
    stats.naive_unfair_cycles += naive.unfair_cycles;
    stats.dpor_unfair_cycles += dpor.unfair_cycles;

    if (program_index % 20 == 0) {
        assert_round_trips(seed, program_index, mode, program);
    }

    if (hit_cap(naive, kMaxSchedules) || hit_cap(dpor, kMaxSchedules)) {
        ++stats.skipped;
        return;
    }

    if (dpor.first_race.has_value() != naive.first_race.has_value() ||
        dpor.first_deadlock.has_value() != naive.first_deadlock.has_value() ||
        dpor.first_error.has_value() != naive.first_error.has_value() ||
        dpor.first_assertion.has_value() != naive.first_assertion.has_value() ||
        cycle_exists(dpor) != cycle_exists(naive) ||
        fair_cycle_exists(dpor) != fair_cycle_exists(naive) ||
        strongly_unfair_cycle_exists(dpor) !=
            strongly_unfair_cycle_exists(naive) ||
        unfair_cycle_exists(dpor) != unfair_cycle_exists(naive) ||
        hit_step_bound(dpor) != hit_step_bound(naive)) {
        fail_program(seed, program_index, mode, memory_model, program, naive, dpor, "verdict mismatch");
    }

    if (dpor.schedules_explored > naive.schedules_explored) {
        fail_program(
            seed, program_index, mode, memory_model, program, naive, dpor, "DPOR explored more schedules than naive");
    }

    assert_replays_dpor_report(seed, program_index, mode, memory_model, program, checker, naive, dpor);

    if (try_lock_actions_in_program != 0) {
        if (mode == GenerationMode::Value) {
            throw std::runtime_error(
                "value fuzz lane unexpectedly generated TryLock");
        }
        stats.try_lock_actions_compared[
            mode == GenerationMode::MostlyWellFormed ? 0 : 1] +=
            try_lock_actions_in_program;
    }
    ++stats.checked;
    if (naive.first_race.has_value()) {
        ++stats.race;
    }
    if (naive.first_deadlock.has_value()) {
        ++stats.deadlock;
    }
    if (naive.first_error.has_value()) {
        ++stats.error;
    }
    if (naive.first_assertion.has_value()) {
        ++stats.assertion;
    }
    if (cycle_exists(naive)) {
        ++stats.cycle;
    }
    if (hit_step_bound(naive)) {
        ++stats.bound_hit;
    }
}

void require_strong_fairness_discriminator() {
    const model::Program program{{
        {set(7, 1), lock("m"), label("retry"), unlock("m"),
         lock("m"), bnz(7, "retry")},
        {lock("m")},
    }};
    const model::ModelChecker checker(program, 20);
    const model::CheckResult naive = checker.explore_naive();
    const model::CheckResult dpor = checker.explore_dpor();
    if (!strongly_unfair_cycle_exists(naive) ||
        !strongly_unfair_cycle_exists(dpor) ||
        fair_cycle_exists(naive) || fair_cycle_exists(dpor) ||
        unfair_cycle_exists(naive) || unfair_cycle_exists(dpor)) {
        throw std::runtime_error(
            "fuzz tri-state discriminator did not isolate strong unfairness");
    }
    if (!dpor.first_nontermination.has_value()) {
        throw std::runtime_error(
            "fuzz tri-state discriminator omitted its witness");
    }
    const model::CheckResult replayed =
        checker.replay(dpor.first_nontermination->schedule);
    if (replayed.first_nontermination != dpor.first_nontermination) {
        throw std::runtime_error(
            "fuzz tri-state discriminator did not replay identically");
    }
}

std::uint64_t parse_seed(const char* text) {
    std::size_t parsed = 0;
    const std::uint64_t seed = std::stoull(text, &parsed, 0);
    if (text[parsed] != '\0') {
        throw std::invalid_argument("seed contains trailing characters");
    }
    return seed;
}

} // namespace

int main(int argc, char** argv) {
    assert_rwlock_spellings_round_trip();
    assert_semaphore_spellings_round_trip();
    assert_barrier_spelling_round_trips();
    assert_try_lock_spelling_round_trips();
    require_strong_fairness_discriminator();

    // CTest uses these fixed seeds for deterministic coverage. Supplying one
    // or more argv seeds replaces the fixed set for manual exploration, e.g.
    // `./dpor_fuzz_differential 0x1234`.
    std::vector<std::uint64_t> seeds = {
        0x6d1f0b5d8e42c71bull,
        0x2a7c9e83f4b106d5ull,
        0xa4e1d07c5198bb2full,
        0x91b52cdd334e77a1ull,
    };
    if (argc > 1) {
        seeds.clear();
        for (int index = 1; index < argc; ++index) {
            seeds.push_back(parse_seed(argv[index]));
        }
    }

    // Mode mix: deterministic mostly-well-formed coverage, adversarial
    // modeled-error coverage, and a value lane for registers, branches,
    // CAS/fetch-add, assertions, exact cycles, and growing-state step-bound
    // backstops.
    constexpr std::size_t kProgramsPerSeed = 750;
    FuzzStats stats;
    for (const std::uint64_t seed : seeds) {
        std::mt19937_64 rng(seed);
        for (std::size_t index = 0; index < kProgramsPerSeed; ++index) {
            GenerationMode mode = GenerationMode::MostlyWellFormed;
            if (index % 10 == 9) {
                mode = GenerationMode::Adversarial;
            } else if (index % 5 == 4) {
                mode = GenerationMode::Value;
            }
            model::MemoryModel memory_model = model::MemoryModel::SC;
            if (index % 4 == 3) {
                // Preserve the existing TSO lane (25% of generated programs).
                memory_model = model::MemoryModel::TSO;
            } else if (index % 8 == 2) {
                // Add a deterministic, mode-independent PSO lane (12.5%).
                memory_model = model::MemoryModel::PSO;
            }
            check_program(seed, index, mode, memory_model, generate_program(rng, mode), stats);
        }
    }

    const std::size_t try_lock_actions_generated =
        stats.try_lock_actions_generated[0] + stats.try_lock_actions_generated[1];
    const std::size_t try_lock_actions_compared =
        stats.try_lock_actions_compared[0] + stats.try_lock_actions_compared[1];
    std::cout << "fuzz_differential: programs=" << stats.total
              << " checked=" << stats.checked
              << " skipped_capped=" << stats.skipped
              << " races=" << stats.race
              << " deadlocks=" << stats.deadlock
              << " errors=" << stats.error
              << " assertions=" << stats.assertion
              << " cycle_programs=" << stats.cycle
              << " bound_hits=" << stats.bound_hit
              << " tso_programs=" << stats.tso
              << " pso_programs=" << stats.pso
              << " naive_schedules=" << stats.naive_schedules
              << " dpor_schedules=" << stats.dpor_schedules
              << " naive_cycles=" << stats.naive_cycles
              << " dpor_cycles=" << stats.dpor_cycles
              << " naive_fair_cycles=" << stats.naive_fair_cycles
              << " dpor_fair_cycles=" << stats.dpor_fair_cycles
              << " naive_strongly_unfair_cycles="
              << stats.naive_strongly_unfair_cycles
              << " dpor_strongly_unfair_cycles="
              << stats.dpor_strongly_unfair_cycles
              << " naive_unfair_cycles=" << stats.naive_unfair_cycles
              << " dpor_unfair_cycles=" << stats.dpor_unfair_cycles
              << " rlock_actions=" << stats.rwlock_actions[0]
              << " runlock_actions=" << stats.rwlock_actions[1]
              << " wlock_actions=" << stats.rwlock_actions[2]
              << " wunlock_actions=" << stats.rwlock_actions[3]
              << " sem_post_actions="
              << stats.semaphore_actions[0][0] + stats.semaphore_actions[1][0]
              << " sem_wait_actions="
              << stats.semaphore_actions[0][1] + stats.semaphore_actions[1][1]
              << " barrier_waits_generated="
              << stats.barrier_waits[0] + stats.barrier_waits[1]
              << " try_lock_actions_generated=" << try_lock_actions_generated
              << " try_lock_actions_compared=" << try_lock_actions_compared
              << " try_lock_mostly_generated="
              << stats.try_lock_actions_generated[0]
              << " try_lock_mostly_compared="
              << stats.try_lock_actions_compared[0]
              << " try_lock_adversarial_generated="
              << stats.try_lock_actions_generated[1]
              << " try_lock_adversarial_compared="
              << stats.try_lock_actions_compared[1]
              << '\n';

    assert(stats.total >= 3000 || argc > 1);
    assert(stats.skipped * 10 < stats.total * 3);
    assert(stats.cycle > 0);
    assert(stats.bound_hit > 0);
    assert(std::all_of(stats.rwlock_actions.begin(), stats.rwlock_actions.end(),
                       [](std::size_t count) { return count > 0; }));
    for (const auto& lane : stats.semaphore_actions) {
        if (!std::all_of(lane.begin(), lane.end(),
                         [](std::size_t count) { return count > 0; })) {
            throw std::runtime_error(
                "fuzz lane did not generate both semaphore actions");
        }
    }
    if (argc == 1 &&
        (stats.barrier_waits[0] == 0 || stats.barrier_waits[1] == 0 ||
         stats.barrier_waits[0] + stats.barrier_waits[1] < 200)) {
        throw std::runtime_error(
            "fuzz lanes did not generate hundreds of BarrierWait actions");
    }
    if (argc == 1) {
        for (std::size_t lane = 0; lane < stats.try_lock_actions_generated.size(); ++lane) {
            const std::size_t generated = stats.try_lock_actions_generated[lane];
            const std::size_t compared = stats.try_lock_actions_compared[lane];
            if (generated < 200) {
                throw std::runtime_error(
                    "each fuzz lane must generate hundreds of TryLock actions");
            }
            if (compared < 100 || compared * 4 < generated * 3) {
                throw std::runtime_error(
                    "each fuzz lane must compare substantial TryLock coverage");
            }
        }
    }
    return 0;
}
