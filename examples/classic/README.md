# Classic Algorithms Gallery

Each file is a `.dpor` model of a classic concurrency algorithm plus a paired
broken variant. The checker is used as-is: surprising verdicts should be
treated as findings, not as a reason to adjust core semantics.

| Algorithm | Property checked | Expected verdict | Broken variant demonstrates |
|---|---|---|---|
| Peterson counter (`peterson_counter.dpor`) | Plain critical-section counter touch is race-free | Nontermination; no race/assertion | `peterson_counter_broken_wrong_flag.dpor` reads an unused flag, so both threads enter and the counter races |
| Peterson inside assertion (`peterson_inside_assert.dpor`) | Plain `inside_free` is nonzero on entry | Clean up to bound | `peterson_inside_assert_broken_wrong_flag.dpor` races on `inside_free` before every violating schedule necessarily reaches the assertion |
| Dekker counter (`dekker_counter.dpor`) | Plain critical-section counter touch is race-free | Nontermination; no race/assertion | `dekker_counter_broken_drop_turn_wait.dpor` enters immediately after the courtesy flag reset instead of waiting for turn |
| Peterson TSO bounded entry (`peterson_tso.dpor`) | Plain flag/turn entry can admit two entrants under TSO | Race plus assertion under TSO | `peterson_tso_fenced.dpor` drains before the entry check and removes the assertion witness |
| Message passing PSO discriminator (`mp_pso.dpor`) | `data` must be visible when `flag` is observed | Race in every model; assertion only under PSO | `mp_pso_fenced.dpor` drains `data` before enqueueing `flag`, removing the PSO assertion witness |
| Dekker TSO bounded entry (`dekker_tso.dpor`) | Plain flag/turn entry can admit two entrants under TSO | Race plus assertion under TSO | `dekker_tso_fenced.dpor` drains before the entry check and removes the assertion witness |
| Lamport bakery, bounded two-thread simplification (`bakery_bounded_counter.dpor`) | Plain critical-section counter touch is race-free | Nontermination; no race/assertion | `bakery_bounded_counter_broken_no_choosing_wait.dpor` uses a bounded one-check witness that observes `number[j] == 0` while the other thread is still choosing |
| Treiber push skeleton (`treiber_push.dpor`) | Two CAS-retry pushes leave both node bits in `top` and increment `success_count` twice | Clean | `treiber_push_broken_load_store.dpor` loses an update when load+store replaces CAS |
| Failed-CAS handoff (`failed_cas_handoff.dpor`) | A failed CAS acquire orders a later plain payload read after the writer's release store | Nontermination; no race/assertion | `failed_cas_handoff_broken_no_retry.dpor` reads payload after a successful pre-publication CAS |

### SC/TSO/PSO verdicts

Plain accesses make the primary verdict `race` in every cell. The parenthetic
annotation records whether the bounded assertion witness also exists.

| Gallery program | SC | TSO | PSO |
|---|---|---|---|
| `mp_pso.dpor` | race (no assertion) | race (no assertion) | race + assertion |
| `mp_pso_fenced.dpor` | race (no assertion) | race (no assertion) | race (no assertion) |

`peterson_tso.dpor` also has a race plus assertion witness under PSO, as it
does under TSO. Its PSO run is capped at the gallery's 300,000-schedule budget,
so that line proves existence only. `peterson_tso_fenced.dpor` exhausts under
PSO with a race and no assertion, matching its TSO result: the modeled full
fences drain every pending address before the entry checks.

## Modeling Notes

- `.dpor` has `set`, `bnz`, `assert`, loads/stores, fetch-add RMW, and CAS, but
  no general arithmetic, no `max()`, no branch-on-zero, and no register-to-
  register comparison. The Peterson and Dekker models use no-op CAS
  (`cas turn X X -> rN`) as an atomic equality test for `turn`.
- The counter examples use a plain read plus a literal plain write as the
  critical-section footprint. That is the race-sensitive part of an increment,
  but it is not a faithful arithmetic increment because plain register addition
  is not available in the IR.
- The Bakery file is not the full unbounded Lamport bakery algorithm. The full
  algorithm needs `number[i] = 1 + max(number[])` and lexicographic ticket
  comparison. This gallery model is a two-thread bounded-ticket version: an
  atomic fetch-add dispenser creates tickets 1 and 2, and the thread with ticket
  2 waits for the other `number[]` cell to clear. The `choosing[]` phase is kept
  because skipping it still exposes the intended bug.
- Spin-loop examples report `nontermination` when the configured bound contains
  a complete exact state cycle. This is schedule-existence, not a fairness
  violation: a scheduler can keep selecting the spinner even while another
  enabled thread could let it finish. Peterson-inside keeps its smaller
  `clean up to bound` regression verdict because that bound ends before a full
  repeated state. Growing-state loops with no repeat also retain the bound
  backstop.
- The TSO Peterson/Dekker files are bounded entry witnesses, not full
  unbounded proofs. They intentionally use plain flag/turn cells, so the
  checker reports the coordination races in both fenced and unfenced forms.
  The useful TSO signal is assertion reachability: unfenced reaches it, fenced
  does not in the bounded witness.
- The Treiber model does not allocate nodes or follow next pointers. It models
  `top` as a bitset of node ids and separately counts successful pushes with an
  atomic fetch-add.
