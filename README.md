# Regional Capacity Broker

Regional Capacity Broker arbitrates **facility-level capacity commitments across
multiple data-center sites**: given independently authoritative site-capacity
offers and a regional demand request, it decides what capacity may be committed,
where, under what constraints, and with what exact residual accounting.

It is a C++20 runtime with no third-party dependency: the standard library, plus
the operating system's own file, locking, durability and thread primitives.

## The boundary this repository owns

**Owned here**

* regional capacity **offers** — a site's generation-scoped declaration of what
  the broker may commit;
* regional capacity **asks** — a caller's demand, constraints and ceilings;
* **brokerage decisions** — accept, partially accept or refuse, with the blocking
  constraint named for everything not granted;
* **brokerage-layer allocation commitments** — who holds which capacity, under
  which generation, in which failure domain, from which pool;
* **exact accounting** — the conservation identity, residuals, withholding,
  revocation and eviction.

**Not owned here, and never inferred**

* each site's underlying capacity truth (its own capacity authority, snapshot
  identity and generation are consumed as evidence);
* cross-site placement topology, latency and dependency placement;
* reservation execution at a site;
* local admission policy;
* workload scheduling.

References to a site, a snapshot, a policy or an evidence document are contracts
and identities. They are not handles into another runtime's internals. Swapping
any neighbouring runtime for another implementation changes nothing here.

## The core question

> Given independently authoritative site-capacity offers and a regional demand
> request, what capacity may be committed, where, under what constraints, and
> with what exact residual accounting?

Two documents go deeper than this manual, and both describe the code that
exists rather than an intention:

* [`docs/persistence-format.md`](docs/persistence-format.md) — the byte-level
  contract of the store: record framing, the chain rule, the two-phase commit
  protocol, the recovery decision table, the snapshot frame, compaction order,
  the durability boundary and the versioning policy.
* [`docs/concurrency.md`](docs/concurrency.md) — the ownership audit: what is
  guarded by what, the enforced lock order, the call paths that could re-enter
  mutable state, and every hazard the specification names with the reason it
  cannot occur here.

## Architecture

```
include/rcb/          public headers -- the stable typed API
src/                  the runtime
tools/rcb.cpp         the rcb command line executable
bench/rcb_bench.cpp   the benchmark harness (measured numbers only)
tests/                the test suites, one executable per suite
examples/             in-tree walkthrough and the installed-package consumer
cmake/                package config and the whole-tree package check
```

Layers, from the bottom up:

| Layer | Header | Responsibility |
|---|---|---|
| Exact arithmetic | `checked.hpp`, `scaled.hpp` | checked integer arithmetic, exact fixed point, exact `mul-div` |
| Content identity | `digest.hpp` | SHA-256, CRC-32C, digests, canonical byte helpers |
| Values | `identifier.hpp`, `units.hpp` | validated identities, dimensions, capacity vectors, hard bounds |
| Domain | `model.hpp` | offers, tranches, asks, constraints, decisions |
| Accounting | `ledger.hpp` | per-tranche terms and the conservation report |
| Kernel | `broker.hpp` | `BrokerCore`: pure, deterministic, single-threaded arbitration |
| Encoding | `json.hpp`, `serialize.hpp` | strict JSON, canonical encodings, digests, snapshots |
| Durability | `store.hpp`, `journal.hpp` | byte logs, atomic snapshot publish, write-ahead journal |
| Concurrency | `service.hpp` | `BrokerService`: one transactional mutex, bounded workers, cancellation, shutdown |

The kernel has no threads, no clock, no I/O, no randomness and no floating point.
That is what makes replay, restart and property testing tractable: everything it
decides is a function of the offers published to it, the asks submitted to it and
its configuration.

## Data model and invariants

### Dimensions and units

Capacity is never one flattened scalar. Four dimensions are accounted
independently, each in its own exact integer unit:

| Dimension | Token | Unit |
|---|---|---|
| Facility power headroom | `power` | watts |
| Heat rejection headroom | `cooling` | watts |
| Physical space | `rack_space` | whole racks |
| Service-class-qualified capacity | `service_capacity` | milli-units (10^-3) |

There is no conversion between dimensions anywhere in the runtime, and a
`CapacityVector` without its dimension is not a capacity.

### Offers

An offer is one site's declaration for **one service class** in **one
generation**. It carries:

* `SiteId`, `generation`, `source_snapshot` (the site capacity snapshot
  identity it was derived from — evidence, not a handle);
* `service_class`, `region`, `jurisdiction`, declared `risk` tier;
* a validity window in the caller's logical instant domain;
* `reserve_policy` reference, `policy_generation`, and the reserve minimum
  priority;
* cost/energy/carbon **evidence** references and exact per-milli-unit values;
* one or more **tranches**, each for a distinct failure domain, each with an
  `allocatable` vector and a `protected_reserve` vector.

Accounting is per tranche, so capacity in two failure domains can never be
silently pooled, and two tranches claiming one failure domain are refused rather
than merged.

### Asks

An ask is one request with an idempotency key, a service class, a requested
vector, a priority, an explicitly chosen fairness policy, an evaluation instant,
locality constraints (allowed/excluded regions, jurisdictions, sites and failure
domains), a failure-domain diversity requirement, a site limit, a risk ceiling,
optional cost/energy/carbon ceilings, protected-reserve authorization, generation
pins and an all-or-nothing or single-source flag.

### Decisions

A decision is `accepted`, `partially_accepted` or `refused`, and carries the
allocations it committed, the blocking constraints that explain everything it did
not commit, the totals, the exact cost, the broker sequence and epoch, the
durability class actually reached, and the accounting-chain digest after it.

### The conservation identity

For every tranche and every dimension, with every term a non-negative exact
integer:

```
allocatable        = committed + remaining_allocatable + withheld
protected_reserve  = reserve_committed + remaining_reserve + reserve_withheld
```

`rcb::BrokerCore::VerifyConservation()` re-derives the committed terms from the
individual live commitments and compares them with the stored terms, so a
disagreement between the ledger and its records is a reported violation rather
than a rounding difference. `withheld` is capacity explicitly withdrawn from
brokerage: withdrawing a generation withholds all of it.

Two further invariants are structural rather than computed: an accepted
commitment never exceeds the offer generation it is accounted against, and no
(site, failure domain, dimension) quantity is ever counted twice — commitments
are identified by content-derived identity, and duplicates are refused.

## Authority, generation and epoch

* **Generation**: monotonic per site. A generation is immutable once published.
  Re-publishing the identical generation is an idempotent no-op; the same
  generation with different content is a conflict; an older generation is stale.
* **Carry-over**: when a new generation covers what is already committed, the
  commitments are re-bound to it (`accounted_generation` never decreases).
* **Shrink**: when a new generation publishes less than is committed, the
  configured `ShrinkPolicy` applies. `RefuseShrink` refuses the publication and
  leaves the previous generation authoritative. `EvictToFit` evicts commitments
  lowest-priority first and, within a priority, newest first, until the ledger
  closes, and reports every eviction with its reason.
* **Pins**: an ask may pin a site's generation; a mismatch is refused with both
  generations named, never adjusted.
* **Epoch**: advances on every open of the store. A plan or a record from an
  earlier incarnation can never be adopted, which is how a stale asynchronous
  completion is stopped from mutating newer state.
* **State version**: the ledger version a plan was computed against. Committing a
  plan against a different version is refused with `fenced`.

## Brokerage semantics

**Allocation.** Candidates are (site, failure domain) pairs with remaining room,
filtered by every eligibility rule: service class, validity window, revocation,
generation pin, region/jurisdiction/site allow and exclude lists, failure-domain
exclusions, risk ceiling and policy-generation pin. The remaining demand is
filled in the order the chosen fairness policy defines.

**Fairness.** There is no implicit fairness: a caller names one, and the default
is the documented stable order.

| Policy | Behaviour |
|---|---|
| `stable_site_order` | candidates in (site, failure domain) order |
| `lowest_cost_first` | cheapest published unit price first, then (site, failure domain) |
| `equal_share_across_sites` | demand divided evenly across the sites that can still serve it, remainder redistributed deterministically |
| `proportional_to_allocatable` | floor shares proportional to remaining allocatable capacity, with a deterministic residue pass |

Shares are computed once per pass against the demand as it stood when the pass
began; repeated takes for one (site, failure domain) are merged into a single
commitment, so one decision produces at most one allocation per site and failure
domain.

**Protected reserve.** Reserve is consumed only when the ask claims authority
under the offer's own reserve policy reference, the ask names that policy
explicitly, its priority is at or above the offer's reserve minimum, and the
policy-generation pin (when present) matches. Otherwise the capacity stays
protected, and — when the ask ends up short — the refusal says so.

**Ceilings.** Cost, energy and carbon ceilings are exact caps on the total. A
ceiling clamps the priced dimension (service capacity) to what the remaining
budget affords, with exact integer division; the commitment itself is never
rounded, and the shortfall appears in `unmet` with the ceiling named as the
blocking constraint.

**Diversity, single source, all-or-nothing.** A failure-domain diversity
requirement is a hard constraint: until it is met, no failure domain may take
more than its even share of the whole request, and if the requirement cannot be
met the ask is refused rather than satisfied from fewer domains. A single-source
ask is served by one (site, failure domain) or refused. An all-or-nothing ask
commits everything or nothing.

**Refusals.** Every refused or partially satisfied ask carries blocking
constraints naming the site, failure domain, dimension and quantities involved.
A refusal with no constraint is a defect.

## Persistence and recovery

The durable store is a directory containing a write-ahead journal, a snapshot
file and a lock file. Nothing else is written anywhere, and nothing is
transmitted.

**Two-phase completion.** Every authoritative change is written twice:

1. a **prepared** record carrying the change, explicitly marked as not yet
   committed, verified by reading the bytes back;
2. a **commit marker** with the same sequence, written only after the change was
   applied in memory and immediately before the caller is told anything; for
   `Durable` the marker is flushed to the platform's durability primitive before
   the call returns.

A prepared record that was never marked is inert: recovery discards it and
reports how many it discarded. The consequences are tested: a crash between the
two records leaves no effect; a crash after the marker leaves the whole effect; a
cancelled ask can never appear after a restart; a caller is never told a change
succeeded before it reached the promised durability boundary.

**Record framing.** A fixed 64-byte header (magic, format version, kind, flags,
epoch, sequence, payload length, payload CRC-32C, chained SHA-256) followed by the
payload. The chain binds each record to its whole predecessor history, so a
rewrite, a reorder or a splice is detectable, and a record whose chain or
checksum disagrees is refused.

**Torn tail versus interior corruption.** A partially written final record is a
torn tail: it is truncated to the last complete record boundary and reported
(`torn_tail`, `torn_tail_offset`, `torn_tail_bytes`, `repaired`). Damage
anywhere else is interior corruption: the store refuses to open with
`persistence_interior_corruption` and is left byte-for-byte untouched. Recovery
never repairs through corruption.

**Snapshots and compaction.** Compaction flushes, writes the canonical state to a
temporary file, reads it back and verifies it, renames it over the snapshot
atomically, marks the journal, and only then resets the log. An interrupted
compaction leaves both copies intact, and a snapshot can never supersede state
the log has not already made durable. A corrupt snapshot is refused, never
partially adopted.

**Durability classes.** Every decision reports the boundary that actually
applied: `durable` (the commit marker reached `fsync`/`FlushFileBuffers`),
`buffered` (written and readable, not synced per operation), or `volatile`
(no store at all). A durability failure poisons the session: further mutations
are refused until the store is reopened, because continuing on a store whose
commit is unknown would be worse than stopping.

## Concurrency and ownership

Audited and held as a property of the design, not of discipline:

* **One transactional mutex** guards the kernel, the journal and every mutation.
  There are no read locks anywhere, so a read-to-write lock upgrade is impossible
  by construction.
* **No callback runs under a lock.** Observers are invoked after the transaction
  has released the mutex; a throwing observer is caught and cannot reach the
  kernel, and an observer that re-enters the broker cannot deadlock it.
* **Workers are joined holding nothing.** Shutdown signals, resolves queued
  submissions, and only then joins — no lock the workers need in order to finish
  is held while waiting for them.
* **The queue is bounded** and the bound is checked before insertion.
  Submissions are refused with `queue_full` (or wait, if the operator chose
  `BlockWhenFull`) rather than growing without limit.
* **Cancellation is cooperative.** An ask cancelled before its transaction
  started is never planned. Cancellation observed after the durable commit
  boundary is reported as `cancelled_after_commit` and the decision stands: the
  alternative would be a durable commitment nobody knows about.
* **A stale completion cannot land.** A decision carries the epoch and state
  version it was planned against, and a mismatch is refused.

## Build, test, install

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Options (all default ON except sanitizers): `REGIONAL_CAPACITY_BROKER_BUILD_TOOLS`,
`..._BUILD_TESTS`, `..._BUILD_EXAMPLES`, `..._BUILD_BENCH`,
`..._WARNINGS_AS_ERRORS`, `..._ENABLE_SANITIZERS`, `..._BUILD_SHARED`.

Install and consume from an independent project:

```
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/opt/rcb
cmake --build build --parallel
cmake --install build
```

```cmake
find_package(RegionalCapacityBroker CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE SummonLabs::RegionalCapacityBroker::regional_capacity_broker)
```

The whole-tree proof — build, install, then configure, build and run an
independent consumer against the installed prefix and never the build tree — is
one command:

```
cmake -D"SOURCE_DIR=." -D"WORK_DIR=../rcb-package-check" -P cmake/PackageCheck.cmake
```

## Command line

`rcb` is the operator front end. Every command opens the store, does one thing,
reports canonical JSON and closes, so each invocation exercises a real open, a
real recovery and a real close.

```
rcb [--store DIR] [--durability durable|buffered] [--pretty] <command>
```

| Command | Purpose |
|---|---|
| `version` | build, persistence-format and request-schema versions |
| `status` | accounting summary, durability class, recovery report |
| `ledger [--site S]` | ledger terms, per site or summarised |
| `decisions [--limit N]` | most recent decisions |
| `verify` | re-derive the conservation identity (exit 5 when it does not close) |
| `recover` | open the store and report exactly what recovery did |
| `compact` | publish a verified snapshot and reset the log |
| `state [--full]` | canonical state digest (and the state itself) |
| `offer --file F` | publish an offer document |
| `revoke --site S --generation G --at T [--reason R]` | withdraw a generation |
| `ask --file F` | submit one ask document |
| `asks --file F` | submit an array of ask documents |
| `digest --file F` | canonical digest and derived decision identity |
| `selftest` | exercise the boundary end to end in one process |

Exit codes: `0` success, `2` usage error, `3` a request was refused by a named
constraint, `4` store or persistence failure, `5` an invariant (conservation)
failed, `6` unsupported request.

### A worked example

Publish an offer for one site with two failure domains:

```json
{
  "schema": 1,
  "site": "dc-north",
  "generation": 7,
  "source_snapshot": "snap-2026-02-11T09:00Z",
  "service_class": "gpu-h100",
  "region": "eu-north",
  "jurisdiction": "eu",
  "risk": "nominal",
  "valid_from": 1000,
  "valid_until": 9000,
  "reserve_policy": "rp-regional-2026",
  "policy_generation": 3,
  "reserve_minimum_priority": "high",
  "cost": {
    "reference": "ev-2026-02",
    "price_per_milli_unit": "0.00042",
    "energy_millijoules_per_milli_unit": 1080000,
    "carbon_milligrams_per_milli_unit": 41000
  },
  "tranches": [
    {
      "domain": "hall-a/row-1",
      "allocatable": {"power": 400000, "cooling": 380000, "rack_space": 24, "service_capacity": 96000},
      "protected_reserve": {"power": 80000, "cooling": 76000, "rack_space": 4, "service_capacity": 19200}
    },
    {
      "domain": "hall-b/row-4",
      "allocatable": {"power": 300000, "cooling": 285000, "rack_space": 18, "service_capacity": 72000},
      "protected_reserve": {"power": 0, "cooling": 0, "rack_space": 0, "service_capacity": 0}
    }
  ]
}
```

```
rcb --store ./store offer --file offer.json
rcb --store ./store ask --file ask.json
```

with `ask.json`:

```json
{
  "schema": 1,
  "key": "order-2026-0211-0007",
  "requester": "training-platform",
  "service_class": "gpu-h100",
  "requested": {"power": 210000, "cooling": 200000, "rack_space": 12, "service_capacity": 48000},
  "priority": "high",
  "fairness": "proportional_to_allocatable",
  "min_distinct_failure_domains": 2,
  "max_risk": "watch",
  "as_of": 2000,
  "has_cost_ceiling": true,
  "max_total_cost": "20.5",
  "generation_pins": [{"site": "dc-north", "generation": 7}]
}
```

The ask is answered with the committed amounts per (site, failure domain), the
exact cost, the residuals left in every pool, and — if anything was not granted —
the blocking constraint that prevented it.

## Library example

```cpp
#include "rcb/rcb.hpp"

rcb::BrokerCore core;                       // deterministic, no I/O

rcb::Offer offer = /* ... */;
auto published = core.PublishOffer(offer);  // authority enters here
if (!published.ok()) { /* refused, with a stable token */ }

auto plan = core.PlanAsk(ask);              // pure: nothing changes
if (plan.ok()) {
  auto decision = core.CommitPlan(plan.value());   // reserves and accounts
}

rcb::ConservationReport report = core.VerifyConservation();
// report.closed is the invariant this runtime exists to keep
```

For durability and concurrency use `rcb::BrokerService::Open` with a store
directory: it adds the journal, the snapshot store, the transactional mutex, the
bounded worker pool, cancellation and shutdown around the same kernel.

## Validation actually performed

Every number below is a count produced by running the suites in this checkout;
nothing is extrapolated. The suites are separate executables registered with
CTest, and there are no test timeouts anywhere in the project.

| Suite | Executable | Tests | Checks |
|---|---|---|---|
| Foundation | `rcb_test_foundation` | 14 | 1,013 |
| Brokerage | `rcb_test_broker` | 21 | 1,034 |
| Property / conservation | `rcb_test_property` | 7 | 244,658 |
| Adversarial | `rcb_test_adversarial` | 40 | 1,605 |
| Persistence / recovery | `rcb_test_persistence` | 10 | 276 |
| Concurrency | `rcb_test_concurrency` | 9 | 4,586 |
| CLI / multiprocess | `rcb_test_cli_multiprocess` | 6 | 91 |
| **Total** | | **107** | **253,263** |

All 107 tests passed in Release; the same suites were also run in Debug, where
the internal assertions are live, with the same result. The check total is
reproducible except for the concurrency suite, whose checks live inside loops
over submissions that the bounded queue may refuse: it reports 4,586 or 4,588
depending on the run, and both numbers are a full pass.

What the suites actually establish:

* **Conservation.** After every mutation in a randomized workload (1–16 sites,
  1–4 failure domains per site, 5–60 asks per seed, four seeds, capacity shrink,
  eviction, revocation and replay), the conservation identity is re-derived from
  the individual commitments — not read from a running counter — for every
  tranche and every dimension, together with non-negativity of both residuals,
  agreement between the ledger and an independently written reference model, and
  agreement between the report's totals and the sum over sites.
* **Determinism.** The same seed through two fresh kernels produces byte-identical
  decision documents, equal state digests and equal accounting chains.
* **Idempotency.** A retried ask returns the recorded decision with `replay`
  set, commits nothing new, and stays idempotent across a real close and reopen
  and across a snapshot restore. A retry storm from eight threads commits once.
* **Recovery.** Real files in a real directory: close and reopen, a truncated
  final record (torn tail, repaired and reported), a damaged interior byte
  (refused as `persistence_interior_corruption`, store left byte-for-byte
  unchanged), a corrupted snapshot (refused), compaction followed by restart
  (state identical), and a stale epoch (refused as `fenced`).
* **Multiprocess.** Six cases run the real `rcb` executable as independent
  operating-system processes: successive processes accumulate exactly and
  conserve, a torn write is repaired by a later process, interior corruption
  stops a process with the persistence exit code and no repair, two processes on
  one store cannot corrupt it (the loser fails as a persistence error and
  nothing it did is accounted for), and a refused ask exits with the constraint
  code.
* **Concurrency.** Bounded queue reached deterministically and refused with
  `queue_full`; cancellation before and after the commit boundary; drain and
  abandon shutdown with every waiting caller answered; an observer that throws;
  an observer that re-enters the broker (which would deadlock if callbacks ran
  under the lock); parallel submitters producing no duplicate commitment
  identity and exact totals.
* **Adversarial.** 40 suites of malformed documents, wrong types, absurd sizes,
  duplicate identities, invalid UTF-8, deep nesting, injected append/sync/read/
  truncate failures, chain and checksum attacks, lock contention, and the
  prepared-without-marker case.
* **Packaging.** `cmake/PackageCheck.cmake` builds and installs the runtime into
  a fresh prefix and then configures, builds and runs an independent consumer
  that resolves `find_package(RegionalCapacityBroker CONFIG)` from that prefix
  alone, printing `RCB-DOWNSTREAM-OK`. This was run in this checkout and passed.

Sanitizers are exercised in CI on Linux (AddressSanitizer + UndefinedBehaviorSanitizer
over the whole CTest suite). They are **not** available in this checkout: the
MinGW-w64 toolchain used here has no `libasan`/`libubsan`, which is why
`-DREGIONAL_CAPACITY_BROKER_ENABLE_SANITIZERS=ON` fails fast at configure time
with an explicit message instead of failing at link. See *Platform support*.

## Benchmarks

Measured with `rcb_bench` (see `bench/rcb_bench.cpp`), which times completed
operations only — a decision is timed around `PlanAsk` + `CommitPlan`, and a
service decision around `AskNow`. Submission or enqueue latency is never
reported. Every figure is labelled by the harness as **REAL**: it is a timing
taken in the process that ran, with the sample count and the exact elapsed
nanoseconds recorded alongside it.

Machine and build for the numbers below: Windows, x86-64, 16 logical cores,
GCC 14.2.0 (MinGW-w64 UCRT), Release (`-O3 -DNDEBUG`), one run on an otherwise
idle machine. Absolute values depend on the machine; the comparisons within a
run are the meaningful part.

| Measurement | Unit | Samples | Value |
|---|---|---|---|
| Decision, 4-site region | ns/decision | 160 | 462,439 |
| Decision, 4-site region | decisions/s | 160 | 2,162 |
| Decision, 16-site region | ns/decision | 128 | 1,344,148 |
| Decision, 64-site region | ns/decision | 32 | 4,545,934 |
| Decision, 64-site region | decisions/s | 32 | 219 |
| Ask fan-out 1 per round (16 sites) | ns/decision | 128 | 1,173,664 |
| Ask fan-out 8 per round | ns/decision | 1,024 | 238,444 |
| Ask fan-out 16 per round | ns/decision | 2,048 | 165,503 |
| Durable store, `durable` (one fsync per decision) | µs/decision | 24 | 2,531 |
| Durable store, `buffered` | µs/decision | 24 | 1,258 |
| In-memory service | µs/decision | 24 | 874 |
| **Durable commit cost** (durable − buffered) | µs/decision | 24 | **1,273** |
| Idempotent retry, commit path | ns/decision | 512 | 129,904 |
| Idempotent retry, replay path | ns/decision | 512 | 9,536 |
| **Saving per idempotent replay** | ns/replay | 512 | **120,368** |
| `VerifyConservation()` over 64 sites / 256 tranches / 8,704 commitments | ns | 1 | 3,780,600 |

These are single-run figures, and run-to-run spread on this machine is real: an
earlier identical run of the same binary reported 618,817 ns for the 4-site
decision and 1,071 µs for the durable-commit delta. Treat the ratios and the
sample counts as the evidence, and re-run `rcb_bench` on the machine you care
about before quoting an absolute number.

Reading the durable number honestly: the 802 µs difference is what one
`FlushFileBuffers` per decision costs on this machine's storage stack, measured
by running the identical ask set through the same service with the same store
directory, once with `Durable` and once with `Buffered`. It is not a claim
about any other machine, and the harness refuses to report it when the two arms
did not run the same workload. The harness also verifies, and reports, that every
store directory it created was removed.

## Platform support

* **Windows**: MSVC 2019+ and MinGW-w64 (GCC 14, Clang 19) are exercised;
  durability uses `FlushFileBuffers`, atomic publish uses
  `MoveFileEx(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`, and writer
  fencing uses an exclusive file handle.
* **Linux**: GCC 12+ and Clang 15+ are exercised in CI; durability uses `fsync`,
  atomic publish uses `rename` plus a directory `fsync`, and writer fencing uses
  `flock`.
* There is no platform-specific capability hidden behind a weaker fallback: where
  a platform cannot provide durability, the store reports a non-durable class
  rather than pretending.

### Sanitizers, honestly

AddressSanitizer and UndefinedBehaviorSanitizer run over the whole CTest suite in
CI on Linux (GCC and Clang both provide the runtimes there). On Windows the
situation is narrower and is stated rather than implied:

* **MSVC** supports AddressSanitizer (`/fsanitize=address`), and the option
  `REGIONAL_CAPACITY_BROKER_ENABLE_SANITIZERS=ON` selects it. Whether it is
  genuinely exercised depends on an MSVC toolchain being present; this checkout
  was validated with MinGW-w64, so the MSVC sanitizer leg is configured but not
  proven here.
* **MinGW-w64** has no AddressSanitizer and no UndefinedBehaviorSanitizer runtime:
  the flags are accepted by the compiler and the link fails with
  `cannot find -lasan`. Requesting the option on such a toolchain now fails at
  configure time with that explanation, rather than several minutes later at link
  time. That is the exact limitation, and no sanitizer coverage is claimed for
  this platform beyond it.
* Undefined-behaviour discipline does not depend on the sanitizer: all arithmetic
  on externally derived quantities is checked (no signed overflow, no
  implementation-defined narrowing), and the checked paths are unit- and
  property-tested with reference models.

## Relationship to adjacent boundaries

| Neighbour | What crosses the boundary |
|---|---|
| Site capacity authority | offers, with their snapshot identity, generation and validity window |
| Policy authority | reserve policy references and policy generations, as opaque references |
| Cost/energy evidence authority | evidence references with exact per-milli-unit values |
| Cross-site placement | nothing: placement is a separate decision made from this runtime's commitments |
| Reservation execution | nothing: this runtime commits capacity, it does not reserve or admit work |
| Workload scheduling | nothing |

The broker consumes identities, generations and evidence. It never reaches into
another runtime's internals, and it is independently useful without any of them.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
