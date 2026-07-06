# ADR 0004: Minimal Reproducing Schedules

## Status

Accepted.

## Context

Reports must include enough schedule information to reproduce a modeled race,
deadlock, or error. The first explored reproducing prefix can contain actions
that are irrelevant to the reported bug identity, especially unrelated actions
on other addresses or actions from threads that are not report endpoints.

`INVARIANTS.md` requires deterministic replay, and the project contract requires failure
output to include enough information to reproduce the bug. Minimization is
therefore allowed only when replay proves the reduced schedule still reproduces
the same bug identity.

## Decision

Add `ModelChecker::minimize_schedule(const Schedule&) const`.

Bug identity is defined as:

- race: same modeled address and same unordered pair of `(thread,
  action_index)` endpoints;
- deadlock: same set of `BlockedThread` entries;
- modeled error: same `(thread, action_index)` endpoint.

If replay of the input schedule reports no bug, minimization returns the input
unchanged. If replay rejects the input schedule, the replay exception is
propagated.

The minimizer uses deterministic greedy fixed-point deletion:

- Iterate threads in ascending id order.
- For each thread, try deleting the last remaining step in that thread's
  subsequence.
- Do not delete race or modeled-error endpoint steps.
- Replay every candidate schedule.
- Keep a deletion only if replay reproduces the same target bug identity.
- Repeat until a full pass keeps no deletion.

The guarantee is intentionally narrow: the returned schedule is 1-minimal with
respect to this per-thread tail-deletion operator. It is not globally minimal.
For example, exact deadlock identity can require keeping actions from a finished
thread because removing them would make that thread enabled and change the
terminal blocked-thread set.

`explore_naive` and `explore_dpor` return minimized schedules in their public
reports. The search implementations keep raw prefixes internally and normalize
the first reports immediately before returning. This avoids perturbing DFS or
DPOR backtracking logic while still making every returned report replay to the
same minimized report object, including its `schedule` field.

## Consequences

Replay remains the ground truth for every reduction; no independence reasoning
is used to justify minimization.

Returned failure output is shorter when the chosen deletion operator can remove
irrelevant tail steps, and it remains deterministic because all iteration uses
ascending thread ids and ordered report identities.

The oracle harnesses continue to assert replay identity of DPOR reports. A
minimized report schedule replayed through `ModelChecker::replay` must reproduce
the same report identity and the same minimized schedule field.
