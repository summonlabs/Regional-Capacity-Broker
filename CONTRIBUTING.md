# Contributing

Regional Capacity Broker decides what capacity a region may commit, at which
site and failure domain, under which constraints, and what is left afterwards. A
defect here is not cosmetic: it is a wrong answer about committed facility
capacity, and somebody will build against that answer. Contributions are judged
first on whether they preserve the invariants below, and only then on style.

## Build and test

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Useful variants:

```
# Debug, with assertions enabled
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug --parallel
ctest --test-dir build-debug --output-on-failure

# AddressSanitizer + UndefinedBehaviorSanitizer over the whole suite
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DREGIONAL_CAPACITY_BROKER_ENABLE_SANITIZERS=ON
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure

# Whole-tree packaging proof: build, test, install, then configure, build and
# run an independent downstream consumer against the installed package
cmake -D"SOURCE_DIR=." -D"WORK_DIR=../rcb-package-check" -P cmake/PackageCheck.cmake
```

Warnings are errors by default (`REGIONAL_CAPACITY_BROKER_WARNINGS_AS_ERRORS=ON`,
`/W4 /WX /permissive-` on MSVC). A first-party warning is a defect: fix it rather
than suppressing it. If a suppression is genuinely unavoidable it must be local
and must carry a comment naming the construct that cannot be expressed
otherwise; there is exactly one such suppression in the library, for the
compiler extension used to obtain a 128-bit intermediate in `src/checked.cpp`.

There are no test timeouts, and none may be added: not a CTest `TIMEOUT`
property, not a shell timeout wrapper, not a watchdog. A test that hangs is a
defect to diagnose and fix. A test is never allowed to pass by killing a
process, and no CI job may set `timeout-minutes`.

## Invariants a change must not weaken

1. **The conservation identity is the ledger.**
   `allocatable = committed + remaining_allocatable + withheld` and
   `protected_reserve = reserve_committed + remaining_reserve + reserve_withheld`,
   per tranche and per dimension, with every term a non-negative exact integer.
   There is no balancing, clamping or tolerance step anywhere. A commit that
   would exceed a pool is an error, never a silent cap.

2. **No floating point in authoritative accounting.** Capacities are integers in
   declared units; money, energy and carbon evidence is exact fixed point in
   micro-units; costs are exact products with no division and therefore no
   rounding. A quantity that cannot be represented exactly is refused.

3. **Stale authority is refused, not merged.** Every offer carries a generation,
   every decision carries the state version it was planned against, every plan
   carries the broker epoch, and every acceptance carries the generation it was
   accounted under. A mismatch is an error, never a repair.

4. **Submission is not completion.** Nothing is published to a caller before its
   commit marker reached the durability boundary the configuration promises. A
   prepared record with no commit marker is inert: recovery discards it. A
   cancelled ask may not later appear as a commitment.

5. **A refusal names its constraint.** Every refused or partially satisfied ask
   carries at least one `BlockingConstraint` with the site, failure domain,
   dimension and quantities that blocked it. A refusal without a constraint is a
   defect.

6. **Determinism is a property, not a hope.** The same offers, asks and
   configuration produce byte-identical decisions, state digests and accounting
   chains, across processes and across restarts. Identities are derived from
   content, never random.

7. **Zero is not unknown.** Unknown, stale, conflicting, unsupported, invalid and
   indeterminate states have distinct error codes and distinct stable tokens.
   Collapsing them loses semantics callers act on.

8. **Bounds are checked before allocation.** Every externally supplied size,
   count, duration, cost and capacity is validated against `rcb::Limits` or the
   relevant limit structure before anything is allocated or read.

9. **The boundary is not widened.** This runtime arbitrates capacity commitments.
   It does not read a site's internals, decide placement, execute reservations,
   apply local admission policy or schedule workloads. A reference to ASI, DFI or
   another DCCP runtime is a contract, an identity or a snapshot — never a
   handle.

10. **One lock, no callbacks under it.** Authoritative state and the journal are
    guarded by one transactional mutex. There are no read locks, so a
    read-to-write upgrade is impossible. Observers run outside every lock.

## Tests a change must bring

* a unit test for the new behaviour;
* a property test when the behaviour is arithmetic or ordering-related, seeded so
  that a failure is reproducible;
* an adversarial test when the behaviour parses, persists or trusts input;
* a recovery test when the behaviour touches the journal, the snapshot or the
  store;
* a concurrency test when the behaviour touches the worker pool, cancellation,
  shutdown or observers;
* an installed-consumer check when the change touches a public header, the
  exported target or the CMake package.

The suites are `foundation`, `broker`, `persistence`, `property`,
`adversarial`, `concurrency`, `cli` and `multiprocess`. Put a test in the suite
that owns the property, not in whichever file was open.

## Style

* Portable C++20, standard library only. A new third-party dependency needs a
  correctness justification that outweighs the audit cost, and it will usually be
  refused.
* `Result<T>` / `Status` for fallible operations. The library does not throw for
  control flow, and callers must be able to distinguish invalid, stale, conflict,
  not found, unsupported, limit exceeded, corruption, permission, I/O failure and
  invariant violation.
* Comments explain *why*, especially where the code deliberately refuses to do
  the obvious thing.
* Canonical encodings are part of the contract: adding a field to a persisted or
  digested structure is a format change and needs a version decision.

## License and contributions

Contributions are accepted under the Apache License 2.0, the same terms as this
project. There is no Contributor License Agreement and none will be required. By
opening a pull request you confirm that you wrote the contribution, or that you
have the right to submit it under those terms.

Commit messages are neutral and describe the change. Do not add co-author
trailers, AI attribution or internal workflow references to commits, tags, code,
comments or documentation.
