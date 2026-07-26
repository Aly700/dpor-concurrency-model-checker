# ADR 0031: Pruning-soundness certificates — diagnosed and deferred

## Status

Accepted (diagnosis and deferral). No certificate emitter, certifier, public
API, CLI command, mutation hook, or new test suite ships in this campaign.
The exploration core remains unchanged.

This is the Campaign 17 stop-clause outcome. The predeclared bar was not
weakened: the proposed verifier could not establish the requested theorem for
every pruning claim made by the current DPOR variant, and an independently
reviewed partial design still contained verdict and closure soundness gaps.
Shipping that design under a weaker name would create a misleading
verification axis.

## Requested result

The campaign asked for a certifying-algorithm split:

1. the existing heuristic DPOR explorer emits deterministic evidence for one
   concrete run;
2. a smaller component, sharing only deterministic state stepping and
   canonical state encodings, checks replay, demand closure, and every
   independence claim used for pruning; and
3. each independence claim is checked semantically by executing both adjacent
   orders from the recorded claim state, without calling
   `independent()` or `transitions_independent*()`.

The intended theorem was correspondingly local to one exact
`(program, memory model, bound, run)` pair: if the certifier accepts, the
recorded explored set is closed under the DPOR demands, assuming only
semantically re-executed diamonds. It was not meant to prove optimality, the
DPOR theory itself, or anything about another program.

That theorem is attractive, but it does not match all proof decisions made by
the current reducer.

## Decisive blocker: some pruning claims are not co-enabled

`add_backtracks_for_transition_against_prefix()` scans a later executed or
terminal-blocked occurrence against earlier trace entries. At each prefix it
calls `transitions_independent_at_node()` **before**
`transition_enabled_at_node()`.

The order is semantically significant:

```text
if independent(previous, later) then
    skip this prefix
else if later is disabled at this prefix then
    remember a disabled-transition repair
...
```

Therefore a positive independence result can suppress a repair at a state
where the later exact occurrence is not enabled. This is not an exceptional
path. Terminal deadlock handling synthesizes each blocked next action and
passes it through the same scan.

A minimal shape is:

```text
T0: Write(x, 1)
T1: Lock(m); Write(y, 1)
```

On the trace `T0:Write(x), T1:Lock(m), T1:Write(y)`, the scan skips the
same-thread lock and compares the distinct-address writes at the root.
The checker-local predicate returns independent. But the exact
`T1:Write(y)` occurrence is disabled at the root because `T1:Lock(m)` is its
current action. Of the required two semantic orders,
`T1:Write(y); T0:Write(x)` cannot be executed from the claim state.

This is an occurrence-identity problem, not a serialization inconvenience.
Recording more of the state cannot make the disabled occurrence executable.
A certifier has only three choices:

- reject an otherwise ordinary current DPOR run;
- trust the explorer's syntactic independence result, which is the relation
  under audit and is expressly forbidden; or
- reconstruct an enabling sequence and prove a multi-step mover/persistent
  argument, which imports substantial DPOR reasoning into the certifier and
  is no longer the requested adjacent two-order check.

Moving the enabledness test ahead of independence, or conservatively repairing
every such prefix, would change the reducer. It can add schedules and therefore
cannot be assumed to preserve the pinned schedule counts and optimality meter.
Campaign 17 required the existing exploration results to remain
byte-identical, so that redesign was not available inside this work order.

This one counterexample is sufficient to prevent the complete requested
certificate theorem.

## Why a final-state diamond is also insufficient

Even where two occurrences are co-enabled, equality only after both orders is
too weak. ADR 0028 provides the discriminator:

```text
T0: RLock(r); Upgrade(r)
T1: RLock(r); Upgrade(r)
```

The two root `RLock` orders converge after both acquisitions, but the first
acquisition transiently enables that thread's `Upgrade`; the second
acquisition disables it again. The current reducer correctly guards the
positive `RLock`/`RLock` refinement with whole-program absence of a same-name
`Upgrade`.

This example is not evidence that the current guarded relation emits the bad
claim. It is evidence that a future semantic checker must compare exact
enabled occurrences at both intermediate states, including participant
continuations and third parties, rather than only final fingerprints. It also
shows why the certifier cannot silently replace a semantic check with the
current static guard.

## The proposed partial verifier did not survive review

The stop clause permits a smaller replay-and-closure certificate if that
subset is itself sound and precisely delimited. A design for such a subset
was drafted and then reviewed before any production or test edit. Four
soundness defects remained.

### 1. Race history is ordered, not a canonical multiset

The ordinary behavioral fingerprint intentionally omits vector clocks and
race-analysis metadata, so it cannot compare future verdict behavior.
A separate verdict-state encoding was proposed.

The draft attempted to canonicalize `plain_accesses` and `atomic_accesses` by
sorting them. That is unsound for an endpoint-sensitive theorem.
`record_read()`, `record_write()`, and `record_atomic()` iterate these vectors
in stored order and select the first conflicting access for the public
`RaceReport`. Two states with the same multiset in a different order can
therefore produce different future race endpoint identities.

There are three distinct equivalence levels:

- literal `RaceReport` equality, which also includes the schedule and therefore
  cannot hold across two differently ordered witnesses;
- the checker's normalized race identity, which retains the address and
  unordered endpoint pair; and
- existential verdict kind, which retains only that some race exists.

For either endpoint-sensitive level, a safe verdict-state encoding must retain
exact vector order (or equivalent first-match information). If two purportedly
commuting orders then differ, that certifier must reject the claim. A
verdict-kind theorem may instead quotient the order, but that quotient needs
its own explicit equivalence proof. Existing gates establish exact
self-replay for each emitted witness and verdict-kind agreement across
explorers; they do not choose a local-diamond equivalence for a future
certificate. The reviewed draft specified and proved none of these choices.

This is not merely hypothetical. Two co-enabled same-address `AtomicLoad`
actions are a current positive independence case. Their two orders append the
same accesses to `atomic_accesses` in opposite order. A later plain `Write`
scans that vector and names its first conflicting load in the public
`RaceReport`. Normalized endpoint identity therefore rejects the commutation,
while an existential “a race exists” quotient might accept it. Either theorem
could be useful, but they are different theorems and the latter needs an
explicit proof that every normalized distinction is irrelevant to the
certificate's advertised verdict. Leaf schedules must still replay to their
own exact reports under either theorem.

### 2. Suppressed-demand detection cannot trust an emitted HB label

The draft proposed reconstructing co-enabled dependent pairs but consuming the
producer's recorded happens-before classification. A forged or buggy producer
could label a pair HB-ordered and suppress the very reversal the certifier is
supposed to rediscover.

A standalone checker must reconstruct the transition clocks from the replayed
prefix and perform the HB comparison itself. Producer evidence can be checked
against that result, but cannot be an input to it.

### 3. Step-bound outcomes are not explored children

In `dpor_dfs()`, the selected endpoint is placed in `done` before the bound
check. When its thread bound is already reached, the checker:

- clears sleep;
- adds every enabled endpoint to backtrack;
- increments the explored-outcome and bound-exceeded counters without
  retaining a public schedule object or executing the attempted endpoint;
- inserts the selected endpoint into sleep; and
- executes no transition and creates no child node.

The draft coverage vocabulary allowed only an actual explored child, a
sleep proof rooted in one, or a local diamond. It would therefore either
reject legitimate bounded runs or mislabel a bound attempt as an executed
child. A future format needs distinct executed-edge, terminal-edge,
bound-attempt, cycle-cut, and semantic-prune dispositions. `DporNode::done`
must never be treated as proof of execution.

### 4. Recorded-demand closure cannot detect an omitted demand

An immutable ledger can prove that every demand the explorer chose to record
was eventually explored or properly awakened. That catches the loop-10 class
only if the demand record itself exists.

The required suppressed-race-reversal mutation removes the demand at its
source. A certifier that merely checks the producer's ledger passes
vacuously. Detecting the omission requires independent enumeration of every
candidate earlier/later occurrence, exact enabledness and occurrence matching,
semantic dependence, and HB ordering. That enumeration is possible in
principle, but it is more than recorded-demand closure and must be designed so
it does not reproduce the explorer's heuristic logic. The reviewed draft had
not established that boundary.

Sleep-set discharge has the same dependency: an inherited sleep proof is
sound only if the exact occurrence survives and every inheritance hop
commutes semantically. Replay plus event ordering alone cannot certify it.

## Additional completeness requirements discovered by the audit

Any future attempt must also account for these details explicitly:

- A persistent-set candidate can compare independent with one selected
  transition and later be retained because it is dependent with another.
  Only the final stable omitted-candidate proof, linked to the complete final
  selected set, is a pruning justification.
- A numeric `ScheduleStep` is not always an exact occurrence. Barrier
  generation, TimedWait phase and episode, and exact Signal/Broadcast wake
  targets must be retained wherever they distinguish an enabled transition.
- Per-claim cost is prefix replay plus state reconstruction/fingerprinting and
  four transition executions (`a;b` and `b;a`), unless reconstructed node
  states are safely cached. Calling it merely “two extra steps” understates
  the cost.
- A deterministic parser must distinguish malformed/version/binding failures
  from well-formed but false evidence without letting API and CLI status
  classes diverge.
- A `max_schedules` cutoff can leave ordinary backtrack obligations
  unresolved. A certifier must reject capped runs or represent an explicit
  cap-cut disposition that cannot be mistaken for closed exploration.
- A successful result needs structural feature and unsupported-capability
  fields. A status such as “verified subset” plus a numeric unsupported count
  is not enough to tell a caller which theorem was checked.

These are design requirements, not post-implementation polish. Each affects
what an accepted certificate means.

## Alternatives considered

### Change DPOR so every audited pair is co-enabled

Check enabledness first and conservatively repair every dependent or
unauditable disabled occurrence. This makes adjacent execution available but
changes the algorithm under certification. Schedule counts and the
SC/TSO/PSO meter would need a new work order and a new predeclared bar.

### Certify multi-step mover proofs

Record the exact enabler chain from the claim state to the later occurrence,
then verify every intermediate commutation and enabledness obligation. This is
the plausible route to a complete certificate for the present reducer, but it
requires a formal persistent/mover theorem and a substantially richer
certifier. It cannot be described honestly as two extra semantic steps per
claim.

### Certify only replay

A log of terminal schedules plus public replay identities is independently
useful for corruption detection, but says nothing about omitted schedules or
pruning closure. The project already exposes replayable witnesses. Calling
that artifact a pruning-soundness certificate would add nomenclature, not the
requested verification axis.

### Certify only recorded-demand closure

This can catch a recorded demand that remains asleep, but cannot catch a
suppressed demand and cannot validate sleep equivalence without semantic
commutation. It fails two of the three predeclared falsifiability mutations
and was therefore not shipped as the “largest sound subset.”

### Emit an exhaustive reference exploration

The certifier could independently traverse the complete reachable state space
and compare public outcomes. That is cross-validation against another
explorer, not a small checker of the DPOR proof object; on the large gallery it
also reproduces the cost problem that motivated certification of one run.

## Decision

Defer pruning certificates. The repository retains its existing trust base:

- exhaustive naive/DPOR comparison over enumerable deterministic corpora;
- fixed-seed differential fuzz;
- replay identity for emitted bug and lasso witnesses;
- the class-count optimality meter; and
- deterministic Release and restore-assert Debug gates.

No certificate flag or `certify` command is reserved. No public type suggests
that a subset theorem exists. `INVARIANTS.md` gains no
certificate-completeness invariant because there is no emitted certificate to
which such an invariant could truthfully apply.

The Campaign 17 acceptance items for a new 31st suite, the 606-case
certificate corpus, three standalone certificate mutations, byte-identical
certificate reports, and certificate overhead are intentionally not marked
passed. They are inapplicable to this deferral, not silently waived.

## Reopening conditions

A future campaign should begin with a paper-level certificate theorem and
small falsifiers before changing the explorer. It may proceed only after it
specifies:

1. how every non-co-enabled independence use is proved — either by changing
   the reducer or by a complete multi-step enabler/mover proof;
2. a verdict-state encoding matched to an explicit theorem: exact report
   preservation must retain ordered race history, clocks, dynamic occurrences,
   and every future public report distinction, while any existential quotient
   must prove each normalized distinction irrelevant; leaf replay remains
   exact;
3. independent reconstruction of enabled occurrences, transition clocks, HB,
   and candidate reversal obligations without calling the DPOR independence
   or repair machinery;
4. intermediate enabledness checks and exact second-occurrence survival for
   every co-enabled semantic diamond;
5. complete coverage dispositions for executed children, terminal reports,
   bound attempts, cycle cuts, schedule-cap cuts, and sleep proofs, with
   capped evidence rejected unless the advertised theorem explicitly permits
   unresolved work;
6. final persistent-set causal proofs rather than raw predicate-call logs;
7. a feature-negotiated canonical format whose accepted theorem is
   machine-visible; and
8. focused RED tests proving, before API work, that the standalone checker
   catches wrong same-mutex independence, a stale-sleep dropped demand, and an
   entirely suppressed reversal demand.

Only after those tests discriminate the checker should the project measure
per-claim cost and choose the deterministic gate slice.

## Verification evidence

The architecture audit ran at `bcf14c4`. Before any campaign edit, the exact
required Release command configured, built, and passed all 30 of 30 existing
CTest suites. After the diagnosis documentation was assembled:

- the exact Release configure/build/CTest command passed 30/30 in 24.43
  seconds;
- Debug with `DPOR_ENABLE_RESTORE_ASSERTS=ON` passed 30/30 in 431.04 seconds;
  and
- two consecutive `dpor_optimality` executions were byte-identical and
  retained SC 1.067, TSO 1.152, and PSO 1.154.

No production source, public header, CLI source, test source, CMake target,
golden, schedule count, or meter implementation changed. The like-for-like
emission-off timing bar has no candidate executable delta to measure:
documentation is the entire campaign diff and the build did not compile or
link a changed checker target. Certificate emission/certification timing is
inapplicable because no certificate path ships.

Two fresh verifier-written ordinary CLI probes satisfied the repository gate:

- the disabled-occurrence shape
  `T0: Write(x); T1: Lock(m); Write(y)` was deterministically clean at one
  DPOR schedule; and
- two same-address `AtomicLoad(x)` threads plus a third plain `Write(x)`
  deterministically reported a race at 5 DPOR schedules versus 6 naive
  schedules. The DPOR report named thread 0's load and thread 2's write.

Each DPOR probe was run twice with byte-identical output and the expected exit
status. The temporary `.dpor` files were removed. These probes exercise the
runtime shapes but do not expose the reducer's internal claim ordering; the
decisive non-co-enabled fact was therefore also checked directly against the
independence-before-enabledness call order and the current distinct-address
write relation. Certificate-specific emit/certify probes remain inapplicable
because no such command ships.

## Invariants protected

- **Soundness and independence:** no semantic certificate claim is made for a
  transition that cannot be executed at its recorded claim state.
- **Replay:** a future certificate cannot normalize away data that changes a
  public replay identity.
- **Enabledness and occurrence identity:** numeric endpoints are not confused
  with generation-, phase-, episode-, or wake-set-specific transitions.
- **Determinism:** no unreviewed observer is inserted into the exploration
  path, and no new output surface is exposed.
- **Verification honesty:** an explicit stop is preferable to a verifier whose
  passing status states a theorem its implementation cannot establish.
