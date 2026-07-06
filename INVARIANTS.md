# Invariants — DPOR Model Checker

## Soundness

- The checker may explore too many schedules, but must not skip a schedule class that can expose a distinct bug.
- Independence may only be claimed for actions whose ordering cannot affect future enabledness or observable state.
- A report of safe means no explored state and no pruned equivalent class contains a bug under the modeled semantics.

## Replay

- A schedule is a deterministic sequence of thread IDs and action indexes.
- Replaying a schedule must produce the same state, report, and trace.
- Test programs must not depend on OS thread scheduling.

## Happens-before

- Release/acquire edges must update vector clocks consistently.
- Atomic operations model acquire loads, release stores, acquire-release RMW,
  and SC-per-location only; no relaxed orderings exist in this IR.
- `AtomicStore(address)` must replace that address's atomic location clock with
  the storing thread's post-tick clock and must not acquire or join the prior
  location clock. Extra happens-before edges can hide real races.
- `AtomicLoad(address)` must join the loading thread with that address's atomic
  location clock and must not mutate the location clock.
- `AtomicRmw(address)` must join the thread with the location clock and then
  replace the location clock with the joined thread clock, preserving the
  modeled release sequence without accumulating unrelated stores.
- A successful `Join(target)` must join the caller with the target thread's
  final vector clock before any post-join action can execute.
- `Wait(cv, mutex)` must release `mutex` with the same clock update as
  `Unlock`; after wakeup it must reacquire `mutex` with the same clock join as
  `Lock` before advancing past the wait action.
- `Signal(cv)` and `Broadcast(cv)` must add a happens-before edge from the
  signaling thread to every waiter they wake. They must not queue permits when
  no waiter exists.
- Conflicting memory accesses unordered by happens-before are races.
- Atomic/atomic accesses are never races. Mixed plain/atomic same-address
  accesses are races when unordered by happens-before and at least one side is
  write-like: plain write, atomic store, or atomic RMW.
- Deadlock detection must distinguish a true cycle from a voluntarily finished
  thread, and must identify whether each unfinished disabled thread is waiting
  on a mutex, a join target, or a condition variable.
