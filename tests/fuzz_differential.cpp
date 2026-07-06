#include "model/checker.hpp"
#include "program_parser.hpp"

#include <algorithm>
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

enum class GenerationMode { MostlyWellFormed, Adversarial };

enum class Choice {
    PlainMemory,
    AtomicMemory,
    Lock,
    Unlock,
    Wait,
    Signal,
    Broadcast,
    Join,
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
    std::size_t naive_schedules{0};
    std::size_t dpor_schedules{0};
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

model::Action unlock(std::string mutex) {
    model::Action action;
    action.kind = model::ActionKind::Unlock;
    action.mutex = std::move(mutex);
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

model::Action yield() {
    model::Action action;
    action.kind = model::ActionKind::Yield;
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

void erase_one(std::vector<std::string>& values, std::size_t index) {
    assert(index < values.size());
    values.erase(values.begin() + static_cast<std::ptrdiff_t>(index));
}

model::Action generate_mostly_well_formed_action(std::mt19937_64& rng,
                                                  model::ThreadId tid,
                                                  std::size_t thread_count,
                                                  std::size_t condition_count,
                                                  std::size_t slots_left,
                                                  std::vector<std::string>& held) {
    if (slots_left == held.size()) {
        std::string mutex = held.back();
        held.pop_back();
        return unlock(std::move(mutex));
    }

    std::vector<WeightedChoice> choices = {
        {Choice::PlainMemory, 10},
        {Choice::AtomicMemory, 10},
        {Choice::Signal, 14},
        {Choice::Broadcast, 10},
        {Choice::Join, 14},
        {Choice::Yield, 4},
    };

    if (slots_left >= held.size() + 2 && held.size() < 2) {
        choices.push_back({Choice::Lock, 25});
    }
    if (!held.empty()) {
        choices.push_back({Choice::Unlock, 8});
        choices.push_back({Choice::Wait, 24});
    }

    switch (choose(rng, choices)) {
    case Choice::PlainMemory:
        return random_plain_memory(rng);
    case Choice::AtomicMemory:
        return random_atomic_memory(rng);
    case Choice::Lock: {
        std::string mutex = random_mutex(rng);
        if (contains(held, mutex)) {
            mutex = mutex == "m" ? "n" : "m";
        }
        assert(!contains(held, mutex));
        held.push_back(mutex);
        return lock(std::move(mutex));
    }
    case Choice::Unlock: {
        const std::size_t index = bounded(rng, held.size());
        std::string mutex = held.at(index);
        erase_one(held, index);
        return unlock(std::move(mutex));
    }
    case Choice::Wait:
        return wait(random_condition(rng, condition_count), held.at(bounded(rng, held.size())));
    case Choice::Signal:
        return signal(random_condition(rng, condition_count));
    case Choice::Broadcast:
        return broadcast(random_condition(rng, condition_count));
    case Choice::Join:
        return join(random_other_thread(rng, tid, thread_count));
    case Choice::Yield:
        return yield();
    }

    return yield();
}

model::Action generate_adversarial_action(std::mt19937_64& rng,
                                          model::ThreadId tid,
                                          std::size_t thread_count,
                                          std::size_t condition_count) {
    // The adversarial lane deliberately injects unbalanced unlocks, waits
    // without owning the mutex, and self-joins to exercise modeled-error paths.
    const std::vector<WeightedChoice> choices = {
        {Choice::PlainMemory, 16},
        {Choice::AtomicMemory, 16},
        {Choice::Lock, 12},
        {Choice::Unlock, 18},
        {Choice::Wait, 18},
        {Choice::Signal, 8},
        {Choice::Broadcast, 6},
        {Choice::Join, 14},
        {Choice::Yield, 4},
    };

    switch (choose(rng, choices)) {
    case Choice::PlainMemory:
        return random_plain_memory(rng);
    case Choice::AtomicMemory:
        return random_atomic_memory(rng);
    case Choice::Lock:
        return lock(random_mutex(rng));
    case Choice::Unlock:
        return unlock(random_mutex(rng));
    case Choice::Wait:
        return wait(random_condition(rng, condition_count), random_mutex(rng));
    case Choice::Signal:
        return signal(random_condition(rng, condition_count));
    case Choice::Broadcast:
        return broadcast(random_condition(rng, condition_count));
    case Choice::Join:
        return bounded(rng, 3) == 0
            ? join(tid)
            : join(static_cast<model::ThreadId>(bounded(rng, thread_count)));
    case Choice::Yield:
        return yield();
    }

    return yield();
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
    const std::size_t thread_count = generated_thread_count(rng);
    const std::size_t condition_count = bounded(rng, 2) + 1;

    model::Program program;
    program.threads.resize(thread_count);
    for (std::size_t tid_index = 0; tid_index < thread_count; ++tid_index) {
        const auto tid = static_cast<model::ThreadId>(tid_index);
        const std::size_t action_count = generated_action_count(rng, thread_count);
        std::vector<std::string> held;
        for (std::size_t action_index = 0; action_index < action_count; ++action_index) {
            const std::size_t slots_left = action_count - action_index;
            if (mode == GenerationMode::MostlyWellFormed) {
                program.threads.at(tid).push_back(
                    generate_mostly_well_formed_action(
                        rng, tid, thread_count, condition_count, slots_left, held));
            } else {
                program.threads.at(tid).push_back(
                    generate_adversarial_action(rng, tid, thread_count, condition_count));
            }
        }
        assert(mode == GenerationMode::Adversarial || held.empty());
    }
    return program;
}

bool hit_cap(const model::CheckResult& result, std::size_t cap) {
    return result.schedules_explored >= cap;
}

void print_failure(std::uint64_t seed,
                   std::size_t program_index,
                   GenerationMode mode,
                   const model::Program& program,
                   const model::CheckResult& naive,
                   const model::CheckResult& dpor,
                   const std::string& reason) {
    std::cerr << "differential fuzz failure: " << reason << '\n';
    std::cerr << "seed: " << hex_seed(seed) << '\n';
    std::cerr << "program_index: " << program_index << '\n';
    std::cerr << "mode: "
              << (mode == GenerationMode::MostlyWellFormed ? "mostly-well-formed" : "adversarial")
              << '\n';
    std::cerr << "naive schedules=" << naive.schedules_explored
              << " race=" << naive.first_race.has_value()
              << " deadlock=" << naive.first_deadlock.has_value()
              << " error=" << naive.first_error.has_value() << '\n';
    std::cerr << "dpor schedules=" << dpor.schedules_explored
              << " race=" << dpor.first_race.has_value()
              << " deadlock=" << dpor.first_deadlock.has_value()
              << " error=" << dpor.first_error.has_value() << '\n';
    std::cerr << "program.dpor:\n" << cli::render_program(program);
}

void fail_program(std::uint64_t seed,
                  std::size_t program_index,
                  GenerationMode mode,
                  const model::Program& program,
                  const model::CheckResult& naive,
                  const model::CheckResult& dpor,
                  const std::string& reason) {
    print_failure(seed, program_index, mode, program, naive, dpor, reason);
    assert(false && "differential fuzz mismatch");
}

void assert_replays_dpor_report(std::uint64_t seed,
                                std::size_t program_index,
                                GenerationMode mode,
                                const model::Program& program,
                                const model::ModelChecker& checker,
                                const model::CheckResult& naive,
                                const model::CheckResult& dpor) {
    if (dpor.first_race.has_value()) {
        const auto replayed = checker.replay(dpor.first_race->schedule);
        if (!replayed.first_race.has_value() || *replayed.first_race != *dpor.first_race) {
            fail_program(seed, program_index, mode, program, naive, dpor, "race replay changed report");
        }
    }
    if (dpor.first_deadlock.has_value()) {
        const auto replayed = checker.replay(dpor.first_deadlock->schedule);
        if (!replayed.first_deadlock.has_value() ||
            *replayed.first_deadlock != *dpor.first_deadlock) {
            fail_program(seed, program_index, mode, program, naive, dpor, "deadlock replay changed report");
        }
    }
    if (dpor.first_error.has_value()) {
        const auto replayed = checker.replay(dpor.first_error->schedule);
        if (!replayed.first_error.has_value() || *replayed.first_error != *dpor.first_error) {
            fail_program(seed, program_index, mode, program, naive, dpor, "error replay changed report");
        }
    }
}

void assert_round_trips(std::uint64_t seed,
                        std::size_t program_index,
                        GenerationMode mode,
                        const model::Program& program) {
    const std::string rendered = cli::render_program(program);
    const model::Program parsed = cli::parse_program_text(rendered);
    if (parsed.threads != program.threads) {
        const model::CheckResult empty_naive;
        const model::CheckResult empty_dpor;
        print_failure(seed, program_index, mode, program, empty_naive, empty_dpor, "CLI parse/render changed program");
        std::cerr << "round_trip.dpor:\n" << cli::render_program(parsed);
        assert(false && "CLI parse/render round trip failed");
    }
}

void check_program(std::uint64_t seed,
                   std::size_t program_index,
                   GenerationMode mode,
                   const model::Program& program,
                   FuzzStats& stats) {
    constexpr std::size_t kMaxSchedules = 20000;
    const model::ModelChecker checker(program);
    const model::CheckResult naive = checker.explore_naive(kMaxSchedules);
    const model::CheckResult dpor = checker.explore_dpor(kMaxSchedules);

    ++stats.total;
    stats.naive_schedules += naive.schedules_explored;
    stats.dpor_schedules += dpor.schedules_explored;

    if (program_index % 20 == 0) {
        assert_round_trips(seed, program_index, mode, program);
    }

    if (hit_cap(naive, kMaxSchedules) || hit_cap(dpor, kMaxSchedules)) {
        ++stats.skipped;
        return;
    }

    if (dpor.first_race.has_value() != naive.first_race.has_value() ||
        dpor.first_deadlock.has_value() != naive.first_deadlock.has_value() ||
        dpor.first_error.has_value() != naive.first_error.has_value()) {
        fail_program(seed, program_index, mode, program, naive, dpor, "verdict mismatch");
    }

    if (dpor.schedules_explored > naive.schedules_explored) {
        fail_program(seed, program_index, mode, program, naive, dpor, "DPOR explored more schedules than naive");
    }

    assert_replays_dpor_report(seed, program_index, mode, program, checker, naive, dpor);

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

    // Mode mix: 7 of every 8 generated programs are mostly well-formed
    // (balanced per-thread locks, waits under held mutexes, valid non-self
    // joins); 1 of every 8 is adversarial to hit modeled-error paths.
    constexpr std::size_t kProgramsPerSeed = 750;
    FuzzStats stats;
    for (const std::uint64_t seed : seeds) {
        std::mt19937_64 rng(seed);
        for (std::size_t index = 0; index < kProgramsPerSeed; ++index) {
            const GenerationMode mode = index % 8 == 7
                ? GenerationMode::Adversarial
                : GenerationMode::MostlyWellFormed;
            check_program(seed, index, mode, generate_program(rng, mode), stats);
        }
    }

    std::cout << "fuzz_differential: programs=" << stats.total
              << " checked=" << stats.checked
              << " skipped_capped=" << stats.skipped
              << " races=" << stats.race
              << " deadlocks=" << stats.deadlock
              << " errors=" << stats.error
              << " naive_schedules=" << stats.naive_schedules
              << " dpor_schedules=" << stats.dpor_schedules << '\n';

    assert(stats.total >= 3000 || argc > 1);
    assert(stats.skipped * 10 < stats.total * 3);
    return 0;
}
