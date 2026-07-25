#include "model/checker.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

enum class BugKind : std::size_t { Race, Deadlock, Error, Assertion, Nontermination, Count };

model::ValueOperand imm(model::Value value) {
    model::ValueOperand operand;
    operand.kind = model::ValueOperandKind::Immediate;
    operand.immediate = value;
    return operand;
}

model::Action read(std::string address, model::RegisterId destination = 0) {
    model::Action action;
    action.kind = model::ActionKind::Read;
    action.address = std::move(address);
    action.destination = destination;
    return action;
}

model::Action write(std::string address, model::Value value = 1) {
    model::Action action;
    action.kind = model::ActionKind::Write;
    action.address = std::move(address);
    action.value = imm(value);
    return action;
}

model::Action atomic_load(std::string address, model::RegisterId destination = 0) {
    model::Action action;
    action.kind = model::ActionKind::AtomicLoad;
    action.address = std::move(address);
    action.destination = destination;
    return action;
}

model::Action atomic_store(std::string address, model::Value value = 1) {
    model::Action action;
    action.kind = model::ActionKind::AtomicStore;
    action.address = std::move(address);
    action.value = imm(value);
    return action;
}

model::Action lock(std::string mutex) {
    model::Action action;
    action.kind = model::ActionKind::Lock;
    action.mutex = std::move(mutex);
    return action;
}

model::Action try_lock(std::string mutex, model::RegisterId destination = 0) {
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

model::Action upgrade(std::string rwlock) {
    return rwlock_action(model::ActionKind::Upgrade, std::move(rwlock));
}

model::Action downgrade(std::string rwlock) {
    return rwlock_action(model::ActionKind::Downgrade, std::move(rwlock));
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

model::Action join(model::ThreadId target) {
    model::Action action;
    action.kind = model::ActionKind::Join;
    action.target = target;
    return action;
}

model::Action fence() {
    model::Action action;
    action.kind = model::ActionKind::Fence;
    return action;
}

model::Action yield() {
    model::Action action;
    action.kind = model::ActionKind::Yield;
    return action;
}

model::Action set(model::RegisterId reg, model::Value value) {
    model::Action action;
    action.kind = model::ActionKind::Set;
    action.destination = reg;
    action.value = imm(value);
    return action;
}

model::Action assertion(model::RegisterId reg) {
    model::Action action;
    action.kind = model::ActionKind::Assert;
    action.source_register = reg;
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

const std::array<model::Action, 22> kEnumeratedActions{
    read("x"),
    write("x"),
    write("y"),
    atomic_load("f"),
    atomic_store("f"),
    lock("m"),
    lock("n"),
    try_lock("m", 0),
    unlock("m"),
    unlock("n"),
    join(0),
    join(1),
    fence(),
    rlock("rw"),
    runlock("rw"),
    wlock("rw"),
    wunlock("rw"),
    upgrade("rw"),
    downgrade("rw"),
    sem_post("sem"),
    sem_wait("sem"),
    barrier_wait("bar", 2),
};

const std::array<model::Action, 26> kFuzzActions{
    read("x", 0),
    read("y", 1),
    write("x", 1),
    write("y", 1),
    atomic_load("f", 0),
    atomic_store("f", 1),
    lock("m"),
    lock("n"),
    try_lock("m", 2),
    unlock("m"),
    unlock("n"),
    join(0),
    join(1),
    fence(),
    yield(),
    set(0, 1),
    assertion(0),
    rlock("rw"),
    runlock("rw"),
    wlock("rw"),
    wunlock("rw"),
    upgrade("rw"),
    downgrade("rw"),
    sem_post("sem"),
    sem_wait("sem"),
    barrier_wait("bar", 2),
};

using BugVector = std::array<bool, static_cast<std::size_t>(BugKind::Count)>;

BugVector bugs_of(const model::CheckResult& result) {
    return {
        result.first_race.has_value(),
        result.first_deadlock.has_value(),
        result.first_error.has_value(),
        result.first_assertion.has_value(),
        result.first_nontermination.has_value(),
    };
}

const char* bug_name(std::size_t index) {
    constexpr const char* kNames[] = {
        "race", "deadlock", "error", "assertion", "nontermination",
    };
    return kNames[index];
}

const char* model_name(model::MemoryModel memory_model) {
    switch (memory_model) {
    case model::MemoryModel::SC:
        return "SC";
    case model::MemoryModel::TSO:
        return "TSO";
    case model::MemoryModel::PSO:
        return "PSO";
    }
    return "unknown";
}

std::string action_text(const model::Action& action) {
    std::ostringstream out;
    if (action.kind == model::ActionKind::TryLock) {
        out << "try_lock " << action.mutex << " -> r"
            << static_cast<unsigned>(action.destination.value_or(0));
        return out.str();
    }
    out << static_cast<int>(action.kind);
    if (!action.address.empty()) {
        out << " address=" << action.address;
    }
    if (!action.mutex.empty()) {
        out << " mutex=" << action.mutex;
    }
    if (!action.rwlock.empty()) {
        out << " rwlock=" << action.rwlock;
    }
    if (!action.semaphore.empty()) {
        out << " semaphore=" << action.semaphore;
    }
    if (!action.barrier.empty()) {
        out << " barrier=" << action.barrier << " parties=" << action.parties;
    }
    if (!action.label.empty()) {
        out << " label=" << action.label;
    }
    if (action.kind == model::ActionKind::Join) {
        out << " target=" << action.target;
    }
    return out.str();
}

void assert_try_lock_action_text() {
    if (action_text(try_lock("m", 3)) != "try_lock m -> r3") {
        throw std::runtime_error(
            "model inclusion TryLock diagnostic spelling changed");
    }
}

void print_program(const model::Program& program) {
    for (std::size_t tid = 0; tid < program.threads.size(); ++tid) {
        std::cerr << "thread " << tid << ":\n";
        for (const model::Action& action : program.threads.at(tid)) {
            std::cerr << "  " << action_text(action) << '\n';
        }
    }
}

[[noreturn]] void inclusion_failure(std::size_t program_index,
                                    const char* source,
                                    std::size_t bug_index,
                                    model::MemoryModel stronger,
                                    model::MemoryModel weaker,
                                    const model::Program& program,
                                    const std::array<model::CheckResult, 3>& results) {
    std::cerr << "MODEL INCLUSION VIOLATION: program=" << program_index
              << " source=" << source
              << " bug=" << bug_name(bug_index)
              << " found_under=" << model_name(stronger)
              << " missing_under=" << model_name(weaker) << '\n';
    print_program(program);
    for (std::size_t model_index = 0; model_index < results.size(); ++model_index) {
        const BugVector bugs = bugs_of(results[model_index]);
        std::cerr << "  " << model_name(static_cast<model::MemoryModel>(model_index))
                  << " schedules=" << results[model_index].schedules_explored
                  << " race=" << bugs[0]
                  << " deadlock=" << bugs[1]
                  << " error=" << bugs[2]
                  << " assertion=" << bugs[3]
                  << " nontermination=" << bugs[4] << '\n';
    }
    throw std::runtime_error("cross-model behavior inclusion violation");
}

struct InclusionStats {
    std::size_t attempted{0};
    std::size_t compared{0};
    std::size_t skipped{0};
    std::size_t inclusion_checks{0};
    std::size_t enumerated{0};
    std::size_t fuzz{0};
    std::size_t hand_picked{0};
    std::size_t barrier_attempted{0};
    std::size_t barrier_compared{0};
    std::size_t barrier_skipped{0};
    std::size_t try_lock_attempted{0};
    std::size_t try_lock_compared{0};
    std::size_t try_lock_skipped{0};
    std::size_t conversion_attempted{0};
    std::size_t conversion_compared{0};
    std::size_t conversion_skipped{0};
    std::array<std::size_t, static_cast<std::size_t>(BugKind::Count)> sc_antecedents{};
    std::array<std::size_t, static_cast<std::size_t>(BugKind::Count)> tso_antecedents{};
};

void compare_program(const model::Program& program,
                     const char* source,
                     std::size_t program_index,
                     InclusionStats& stats) {
    constexpr std::size_t kStepBound = 24;
    constexpr std::size_t kMaxSchedules = 50000;
    ++stats.attempted;
    const bool barrier_gate = std::string(source) == "barrier";
    const bool try_lock_gate = std::string(source) == "try-lock";
    const bool conversion_gate = std::string(source) == "rwlock-conversion";
    if (barrier_gate) {
        ++stats.barrier_attempted;
    }
    if (try_lock_gate) {
        ++stats.try_lock_attempted;
    }
    if (conversion_gate) {
        ++stats.conversion_attempted;
    }

    std::array<model::CheckResult, 3> results;
    for (std::size_t index = 0; index < results.size(); ++index) {
        results[index] = model::ModelChecker(
            program, kStepBound, static_cast<model::MemoryModel>(index)).explore_dpor(kMaxSchedules);
    }

    for (const model::CheckResult& result : results) {
        if (result.exploration_capped || result.bound_exceeded_executions > 0) {
            ++stats.skipped;
            if (barrier_gate) {
                ++stats.barrier_skipped;
            }
            if (try_lock_gate) {
                ++stats.try_lock_skipped;
            }
            if (conversion_gate) {
                ++stats.conversion_skipped;
            }
            return;
        }
    }

    const BugVector sc = bugs_of(results[0]);
    const BugVector tso = bugs_of(results[1]);
    const BugVector pso = bugs_of(results[2]);
    for (std::size_t bug = 0; bug < sc.size(); ++bug) {
        if (sc[bug]) {
            ++stats.sc_antecedents[bug];
        }
        if (tso[bug]) {
            ++stats.tso_antecedents[bug];
        }
        if (sc[bug] && !tso[bug]) {
            inclusion_failure(program_index,
                              source,
                              bug,
                              model::MemoryModel::SC,
                              model::MemoryModel::TSO,
                              program,
                              results);
        }
        ++stats.inclusion_checks;
        if (tso[bug] && !pso[bug]) {
            inclusion_failure(program_index,
                              source,
                              bug,
                              model::MemoryModel::TSO,
                              model::MemoryModel::PSO,
                              program,
                              results);
        }
        ++stats.inclusion_checks;
    }

    ++stats.compared;
    if (barrier_gate) {
        ++stats.barrier_compared;
    }
    if (try_lock_gate) {
        ++stats.try_lock_compared;
    }
    if (conversion_gate) {
        ++stats.conversion_compared;
    }
    if (std::string(source) == "enumerated") {
        ++stats.enumerated;
    } else if (std::string(source) == "fuzz") {
        ++stats.fuzz;
    } else if (std::string(source) == "hand-picked") {
        ++stats.hand_picked;
    }
}

std::uint64_t pow_actions(std::size_t exponent) {
    std::uint64_t result = 1;
    for (std::size_t i = 0; i < exponent; ++i) {
        result *= kEnumeratedActions.size();
    }
    return result;
}

model::Program enumerated_program(std::uint64_t encoded,
                                  std::size_t lhs_length,
                                  std::size_t rhs_length) {
    model::Program program;
    program.threads.resize(2);
    for (std::size_t index = 0; index < lhs_length; ++index) {
        program.threads[0].push_back(kEnumeratedActions.at(encoded % kEnumeratedActions.size()));
        encoded /= kEnumeratedActions.size();
    }
    for (std::size_t index = 0; index < rhs_length; ++index) {
        program.threads[1].push_back(kEnumeratedActions.at(encoded % kEnumeratedActions.size()));
        encoded /= kEnumeratedActions.size();
    }
    return program;
}

model::Program fuzz_program(std::mt19937_64& rng) {
    model::Program program;
    program.threads.resize(2);
    for (auto& thread : program.threads) {
        const std::size_t length = 1 + static_cast<std::size_t>(rng() % 4);
        for (std::size_t index = 0; index < length; ++index) {
            thread.push_back(kFuzzActions.at(rng() % kFuzzActions.size()));
        }
    }
    return program;
}

std::vector<model::Program> hand_picked_programs() {
    return {
        model::Program{{{write("x", 1)}, {write("x", 2)}}},
        model::Program{{{lock("m"), lock("n")}, {lock("n"), lock("m")}}},
        model::Program{{{unlock("m")}, {yield()}}},
        model::Program{{{assertion(0)}, {yield()}}},
        model::Program{{{set(0, 1), label("spin"), bnz(0, "spin")}, {yield()}}},
        model::Program{{{write("data", 1), write("flag", 1)},
                        {read("flag", 0), read("data", 1)}}},
        model::Program{{{wlock("rw"), write("data", 1), wunlock("rw")},
                        {rlock("rw"), read("data", 0), runlock("rw")}}},
        model::Program{{{sem_wait("sem")}, {yield()}}},
        model::Program{{{write("data", 1), sem_post("sem")},
                        {sem_wait("sem"), read("data", 0)}}},
        model::Program{{{write("x", 1), sem_post("sem")},
                        {sem_post("sem")},
                        {sem_wait("sem"), read("x", 0)}}},
    };
}

std::vector<model::Program> barrier_programs() {
    return {
        // A complete generation drains buffered publications and joins both
        // arrivals before either participant continues.
        model::Program{{{write("x", 1), barrier_wait("bar", 2), read("y", 0)},
                        {write("y", 1), barrier_wait("bar", 2), read("x", 1)}}},
        // The same object resets and releases a second generation.
        model::Program{{{barrier_wait("bar", 2), barrier_wait("bar", 2)},
                        {barrier_wait("bar", 2), barrier_wait("bar", 2)}}},
        // An incomplete generation is a model-independent barrier deadlock.
        model::Program{{{barrier_wait("bar", 3)}, {barrier_wait("bar", 3)}}},
        // Exercise three-party last arrival under every memory model.
        model::Program{{{barrier_wait("bar", 3)},
                        {barrier_wait("bar", 3)},
                        {barrier_wait("bar", 3)}}},
    };
}

std::vector<model::Program> try_lock_programs() {
    return {
        // Either thread may win. The result register captures both outcomes.
        model::Program{{{try_lock("m", 0)}, {try_lock("m", 1)}}},
        // Pin the three-thread state-dependent DPOR case: once thread 2 owns
        // m, both sibling TryLock actions fail without changing the owner.
        model::Program{{{try_lock("m", 0)},
                        {try_lock("m", 1)},
                        {lock("m")}}},
        // Exercise the success path as a full ordered point under TSO/PSO.
        model::Program{{{write("x", 1), try_lock("m", 0), unlock("m")},
                        {lock("m"), read("x", 1), unlock("m")}}},
        // Feed the result into the existing branch/value machinery without a
        // backward edge, so this corpus remains bounded in every model.
        model::Program{{{lock("m")},
                        {try_lock("m", 3), bnz(3, "acquired"), yield(),
                         label("acquired")}}},
    };
}

std::vector<model::Program> rwlock_conversion_programs() {
    return {
        // Successful upgrade consumes a preceding reader epoch.
        model::Program{{{rlock("rw"), read("x", 0), runlock("rw")},
                        {rlock("rw"), upgrade("rw"), write("x", 1),
                         wunlock("rw")}}},
        // Downgrade publishes the writer section while retaining read mode.
        model::Program{{{wlock("rw"), write("x", 1), downgrade("rw"),
                         runlock("rw")},
                        {rlock("rw"), read("x", 0), runlock("rw")}}},
        // Both retained readers wait permanently on the other conversion.
        model::Program{{{rlock("rw"), barrier_wait("ready", 2), upgrade("rw")},
                        {rlock("rw"), barrier_wait("ready", 2), upgrade("rw")}}},
        // Both wrong-mode conversions are deterministic modeled errors.
        model::Program{{{upgrade("rw")}, {downgrade("rw")}}},
    };
}

} // namespace

int main() {
    assert_try_lock_action_text();
    InclusionStats stats;
    std::size_t program_index = 0;

    constexpr std::uint64_t kProgramsPerLengthPairCap = 256;
    for (std::size_t lhs_length = 0; lhs_length <= 2; ++lhs_length) {
        for (std::size_t rhs_length = 0; rhs_length <= 2; ++rhs_length) {
            const std::uint64_t count = pow_actions(lhs_length + rhs_length);
            const std::uint64_t samples =
                std::min<std::uint64_t>(count, kProgramsPerLengthPairCap);
            for (std::uint64_t sample = 0; sample < samples; ++sample) {
                const std::uint64_t encoded =
                    count == samples ? sample : (sample * count) / samples;
                compare_program(enumerated_program(encoded, lhs_length, rhs_length),
                                "enumerated",
                                program_index++,
                                stats);
            }
        }
    }

    constexpr std::uint64_t kFuzzSeed = 0x7b83d52fa9614c0dull;
    std::mt19937_64 rng(kFuzzSeed);
    constexpr std::size_t kFuzzPrograms = 128;
    for (std::size_t index = 0; index < kFuzzPrograms; ++index) {
        compare_program(fuzz_program(rng), "fuzz", program_index++, stats);
    }

    for (const model::Program& program : hand_picked_programs()) {
        compare_program(program, "hand-picked", program_index++, stats);
    }
    for (const model::Program& program : barrier_programs()) {
        compare_program(program, "barrier", program_index++, stats);
    }
    for (const model::Program& program : try_lock_programs()) {
        compare_program(program, "try-lock", program_index++, stats);
    }
    for (const model::Program& program : rwlock_conversion_programs()) {
        compare_program(
            program, "rwlock-conversion", program_index++, stats);
    }

    if (stats.skipped != 0) {
        throw std::runtime_error("model inclusion corpus must have zero skips");
    }
    if (stats.barrier_attempted == 0 ||
        stats.barrier_compared != stats.barrier_attempted ||
        stats.barrier_skipped != 0) {
        throw std::runtime_error("model inclusion barrier corpus was skipped");
    }
    if (stats.try_lock_attempted == 0 ||
        stats.try_lock_compared != stats.try_lock_attempted ||
        stats.try_lock_skipped != 0) {
        throw std::runtime_error("model inclusion TryLock corpus was skipped");
    }
    if (stats.conversion_attempted != rwlock_conversion_programs().size() ||
        stats.conversion_compared != stats.conversion_attempted ||
        stats.conversion_skipped != 0) {
        throw std::runtime_error(
            "model inclusion rwlock conversion corpus was skipped");
    }
    for (std::size_t bug = 0; bug < static_cast<std::size_t>(BugKind::Count); ++bug) {
        if (stats.sc_antecedents[bug] == 0 || stats.tso_antecedents[bug] == 0) {
            throw std::runtime_error(
                std::string("model inclusion did not exercise antecedent for ") + bug_name(bug));
        }
    }
    std::cout << "model_inclusion: programs compared=" << stats.compared
              << " skips=" << stats.skipped
              << " inclusion checks passed=" << stats.inclusion_checks
              << " attempted=" << stats.attempted
              << " enumerated=" << stats.enumerated
              << " fuzz=" << stats.fuzz
              << " hand_picked=" << stats.hand_picked
              << " barrier_attempted=" << stats.barrier_attempted
              << " barrier_compared=" << stats.barrier_compared
              << " barrier_skipped=" << stats.barrier_skipped
              << " try_lock_attempted=" << stats.try_lock_attempted
              << " try_lock_compared=" << stats.try_lock_compared
              << " try_lock_skipped=" << stats.try_lock_skipped
              << " conversion_attempted=" << stats.conversion_attempted
              << " conversion_compared=" << stats.conversion_compared
              << " conversion_skipped=" << stats.conversion_skipped
              << " bug_kinds_exercised=5"
              << " seed=0x7b83d52fa9614c0d\n";
    return 0;
}
