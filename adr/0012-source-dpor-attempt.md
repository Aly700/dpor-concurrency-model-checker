# ADR 0012: Source-DPOR attempt — deferred

## Status

Accepted (deferral). The exploration algorithm remains backtrack-set DPOR
with sleep sets, HB-aware last-point backtracking (ADR 0006), and
enabler-chain disabled repairs (ADR 0011).

## Context

Source-DPOR with wakeup trees (Abdulla, Aronis, Jonsson, Sagonas) promises
optimality: exactly one explored schedule per Mazurkiewicz trace class. An
implementation attempt was made with a staged plan (source sets first,
wakeup trees only on a green first stage) and a quantitative bar: a
meaningful improvement on at least one verification gate, with all gate
assertions unweakened.

## What the attempt produced

Stage (i) only. The concrete change was a rename of backtrack sets to
"source sets" plus removal of the transitive dependence closure in node
initialization (dependence checked single-pass against the head only,
relying on dynamic repairs for the rest). Wakeup trees — and the paper's
actual source-set machinery of weak initials over reversible race suffixes
— were not implemented, because their interaction with this IR's blocking
semantics (locks, joins, two-phase waits, spawn-gated aliveness) is the
hard part and was not worked out.

Measured against the three gates: 2-thread sweep unchanged (37,503);
3-thread sweep 477,916 -> 477,644 (-0.06%); committed-seed fuzz
108,897 -> 108,868 (-0.03%). All gates and eight extra fuzz seeds were
green.

## Decision

Not shipped, for two reasons:

1. The success bar ("meaningful improvement on at least one gate") was not
   met; ~0.05% does not justify rewriting the most invariant-dense code in
   the project, per the ADR 0009 precedent.
2. Naming the result "Source-DPOR" without weak initials or wakeup
   sequences would misdescribe the algorithm class in the project's own
   documentation.

## What a real Source-DPOR upgrade needs

- Weak initials of the reversed race suffix as the membership test when
  repairing a race (in place of inserting the racing thread directly).
- A treatment of disabled transitions in race reversal: this project's two
  historical DPOR bugs both lived where blocking meets backtracking, so the
  design must state, per blocking kind (lock, join, sleeping wait, woken
  reacquire, unspawned thread), what the reversible-race suffix and its
  initials are.
- Wakeup trees to replace sleep-set-blocked redundant executions with
  guided replays; only then is the optimality claim (explored == class
  count, checkable by brute force on the 2-thread sweep subfamily with
  small schedule counts) on the table.

## Invariant protected

Soundness via honesty: the exploration core keeps its battle-tested
algorithm, and this record prevents a future reader from believing a
stronger claim than the code earns.
