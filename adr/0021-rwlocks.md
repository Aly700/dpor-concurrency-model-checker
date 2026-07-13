# ADR 0021: Reader-writer locks

## Status

Accepted.

## Context

The checker already models mutexes, condition variables, dynamic thread
creation, and full-fence synchronization under SC, TSO, and PSO. Reader-writer
locks add a synchronization primitive whose read mode is shared while its
write mode is exclusive. Treating a read lock as an ordinary mutex would be
semantically wrong and would also discard the central reduction opportunity:
two co-enabled read acquisitions can commute.

The happens-before relation needs equal care. Readers must acquire a prior
writer's publication, and a later writer must acquire every reader's
publication before entering after those readers drain. Readers must not acquire
one another's releases. Such an extra reader-to-reader edge would hide races
between operations that are legal concurrently in read mode and would make the
reader-commutation argument false.

## Decision

### Actions, namespace, and ownership

The IR adds `RLock`, `RUnlock`, `WLock`, and `WUnlock`. Each action names an
entry in a reader-writer-lock namespace distinct from addresses, condition
variables, and mutexes. A single name may not be used by both mutex operations
(`Lock`, `Unlock`, or the mutex operand of `Wait`) and reader-writer-lock
operations. The CLI rejects that mixed namespace while loading a program, and
the direct `Program` API rejects it in `ModelChecker` construction.

Each reader-writer lock has a canonical set of reader thread IDs and an
optional writer holder. `RLock` is enabled when there is no writer. `WLock` is
enabled when there is neither a writer nor any reader. `RUnlock` and `WUnlock`
remain enabled so an invalid unlock reaches a deterministic modeled error,
matching mutex `Unlock`.

Reentrancy and mode conversion are not supported:

- `RLock` by a thread already holding the lock in either mode executes as a
  modeled error. Its self-ownership exception therefore remains enabled long
  enough to report the error even when it holds write mode.
- `WLock` by the current writer executes as a modeled reentrancy error.
- `WLock` by a current reader is deliberately disabled. Its own reader hold can
  never drain while it waits, so a terminal execution reports a
  waiting-for-readers deadlock with `self_wait`, rather than converting the
  upgrade attempt into an immediate error.
- Unlocking without holding the named lock in the requested mode, including a
  wrong-mode unlock, is a modeled error and does not mutate ownership.

Deadlock reports add two blocker kinds. `RwLockWriter` means a read or write
acquisition is waiting for the current writer and records that owner.
`RwLockReaders` means a writer is waiting for the reader set to drain. The
latter carries `self_wait` when the blocked writer is itself among those
readers. These categories remain distinct through formatting, replay,
minimization identity, and byte-identical CLI report round trips.

Under TSO and PSO all four operations are ordered synchronization points. They
are disabled until every store buffer owned by the executing thread has
drained, following the same full-fence discipline as mutex and condition-
variable operations. No operation performs a hidden flush.

### Happens-before clock bookkeeping

Each reader-writer lock has two analysis clocks in addition to ownership:

- `writer_release` is the post-tick clock stored by the last successful
  `WUnlock`.
- `reader_releases` is the componentwise join of post-tick clocks stored by
  every successful `RUnlock` in the current reader epoch.

The four successful operations update them exactly as follows:

1. `RLock` joins the acquiring thread with `writer_release` only. It does not
   read or mutate `reader_releases`.
2. `RUnlock` joins the releasing thread's post-tick clock into
   `reader_releases`.
3. `WLock` joins the acquiring writer with both `writer_release` and
   `reader_releases`, then resets `reader_releases` to the empty clock. The
   reset starts a new reader epoch; everything summarized by the old
   accumulator is now already in the writer's clock and will be republished by
   its eventual `WUnlock`.
4. `WUnlock` replaces `writer_release` with the releasing writer's post-tick
   clock. It does not join the previous value and does not change
   `reader_releases`.

The reader accumulator is not cleared merely because the live reader count
reaches zero. A writer that acquires later must still see every reader release
since the preceding writer epoch. It is cleared only after a successful
`WLock` has consumed it.

This yields the required edges: writer release to later reader acquire, writer
release to later writer acquire, and every prior reader release to the writer
that follows their drain. It deliberately creates no reader-release to
reader-acquire edge. Fixed replay probes pin each positive direction and a
negative probe makes two serially scheduled read-lock holders write the same
plain address; that write/write race must remain visible.

Ownership is behavioral state and is included in exact lasso fingerprints.
The two release clocks are happens-before analysis instrumentation, like mutex
and atomic location clocks, and are excluded from behavioral fingerprints.
`reader_releases` is explicitly not monotone because a successful `WLock`
consumes and resets it; that reset does not change program control,
enabledness, or modeled values.

### DPOR independence

The public action relation makes two cross-thread, co-enabled `RLock(m)`
actions independent. For two successful nonterminal acquisitions, their
commuting diamond is exact: either order leaves
the same reader holder set and count, the same per-thread clocks (each joins
only the unchanged writer-release clock), the same writer and accumulated
reader-release clocks, the same shared values and race metadata, and the same
enabled set. In particular, a `WLock(m)` is disabled after either first reader
and remains disabled after both orders, so its enabledness is identical.

A reentrant `RLock` also remains enabled so it can produce its modeled error;
that terminal transition is not part of the successful-acquisition state
diamond. The checker's existing terminal safeguard clears the sleep set and
adds every enabled sibling to the backtrack set before ending an error
execution. Thus the public action classification cannot prune a sibling across
the non-commuting terminal outcome.

Operations on different reader-writer-lock names are independent, subject to
the existing cross-cutting Spawn, Join, buffered-write, and memory clauses,
because they touch disjoint ownership and synchronization clocks.

At the public action level every other same-lock pair is conservatively
dependent: writer acquisition/release against anything, `RLock` against
`RUnlock`, and two `RUnlock` actions. This protects observable ownership,
errors, happens-before clocks, and future enabledness. In particular, a local
two-action final-state diamond is insufficient when another transition can be
enabled between them. With one live reader, the sequence
`RUnlock; WLock(third); RLock` exists only in the unlock-first order. Sleeping
the reacquire across the last release would reproduce the middle-witness shape
behind ADR 0010's slept-repair bug and ADR 0017's persistent PSO flush choices.

The checker has one narrower, program-aware refinement for the pure-reader
discriminator. If the complete static program contains no `WLock` or
`WUnlock` for a given name, every cross-thread pair of reader-mode operations
(`RLock` or `RUnlock`) on that name commutes for successful transitions. There
is then no writer action
that can observe the transient zero-reader state or become newly enabled in
the middle. Per-thread ownership validity and clocks remain disjoint, and the
final reader set, reader-release join, enabled set, and shared/race state are
identical. Invalid unlock/reentrancy endpoints use the same terminal-sibling
safeguard described above. The public relation remains conservative so a
writer-bearing program cannot receive this refinement accidentally.

For three threads each executing `RLock(m); Read(shared); RUnlock(m)`, naive
exploration has

```text
9! / (3! * 3! * 3!) = 1680
```

leaf schedules, not six; six counts only the fully serial thread orders. The
program-aware reader-only relation collapses all cross-reader interleavings to
one DPOR representative. If a future writer-like observer is added to the
language, this static proof condition must be revisited before the refinement
is widened.

Disabled-transition repair follows ownership enablers. A reader blocked on a
writer is repaired through that writer; a writer blocked on a writer is
repaired through that writer; and a writer blocked on readers is repaired
through all current reader holders. Self cycles or an enabler chain that cannot
be proved use the existing all-enabled conservative fallback.

### Text format, replay, and gates

The strict text spellings are `rlock`, `runlock`, `wlock`, and `wunlock`, each
followed by one lock name. Rendering uses the same lowercase spellings. They
are ordinary source schedule steps, so the numeric schedule representation is
unchanged. Every new modeled-error and deadlock witness must replay to an
identical report, and CLI `check` output supplied to `replay` must be
byte-identical.

The naive/DPOR oracles, three-thread sweep, deterministic differential fuzz,
and SC-to-TSO-to-PSO inclusion gate include all four actions. The pre-existing
optimality corpus is intentionally unchanged; only exhaustive diagnostics know
the new kinds, preserving the committed SC/TSO/PSO meter baselines.

Focused gate results for this decision are: 21,856 two-thread programs with
63,763 naive versus 36,472 DPOR schedules; 65,543 sampled three-thread programs
with 875,398 versus 410,943; 3,000 fuzz programs with 1,228,768 versus 150,358
(34 capped programs excluded from verdict equality); and 1,704 complete
cross-model programs with zero inclusion skips. Every gate retained verdict
equality, schedule dominance, and deterministic replay. The unchanged
optimality corpus remains SC 1.067, TSO 1.152, and PSO 1.154.

## Consequences

- Correct reader/writer programs can express parallel readers and exclusive
  publication without fabricating reader-to-reader synchronization.
- Direct ownership state is slightly larger, and writer acquisition joins an
  accumulated reader frontier rather than one release clock.
- Same-lock pruning stays deliberately pessimistic in every writer-bearing
  program except the proved `RLock`/`RLock` diamond. The exact-one reader-only
  refinement is guarded by absence of writer-mode actions in the whole static
  program.
- Deadlock output distinguishes waiting on a writer from waiting for readers,
  including an explicit self-waiting upgrade.
- Existing mutex, memory, atomic, flush, Spawn, Join, and condition-variable
  independence clauses are unchanged.

## Invariants protected

- **Happens-before:** the two-clock epoch discipline adds every required
  writer/reader-to-writer edge and no reader-to-reader edge, preventing both
  missed publication ordering and races hidden by over-strengthening.
- **Independence soundness:** the direct read/read diamond and the static
  no-writer proof preserve final state and enabled sets; all middle-witness
  cases remain dependent.
- **Replay:** ownership, blocker kind, owner, and `self_wait` are deterministic
  report identity, while all four synchronization actions obey the same
  buffered ordered-point rule during exploration and replay.
- **Deadlock soundness:** disabled acquisitions name their actual writer or
  reader-set blocker; an upgrade cannot be mislabeled as reentrancy or clean
  termination.
- **Namespace validation:** rejecting a name shared with mutex operations
  prevents one textual resource from acquiring two incompatible ownership and
  clock semantics.
