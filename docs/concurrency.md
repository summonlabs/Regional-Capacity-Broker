# Concurrency and ownership audit

This document records the ownership audit of the runtime: what is guarded by
what, the lock ordering that is enforced, the call paths that could re-enter
mutable state, and the specific hazards the specification names, each with the
reason it cannot occur here and the test that would catch a regression.

The audit is deliberately about *design* rather than about tests. Tests can show
that a race did not happen on one machine; the argument below is what shows it
cannot happen at all.

## Ownership

| State | Owner | Guard |
|---|---|---|
| Ledger, offers, decisions, idempotency index, accounting chain | `rcb::BrokerCore` | the service's transactional mutex; the kernel itself has no locks and is documented as single-threaded |
| Journal (framing, chain, pending prepared record) | `rcb::Journal` | the same transactional mutex |
| Byte log and snapshot store | `rcb::FileByteLog` / `rcb::FileSnapshotStore` | the same transactional mutex, and, across processes, the store lock file |
| Submission queue and stopping flag | `rcb::BrokerService` | `queue_mutex_` for the queue; `stopping_` is atomic |
| Observer callable | `rcb::BrokerService` | `observer_mutex_`, held only to copy the `std::function` |
| Shutdown sequencing | `rcb::BrokerService` | `shutdown_mutex_` |
| `AskHandle` completion | the handle | `AskHandle::mutex_` |
| `CancelToken` | the caller | one atomic flag |

There is exactly **one** mutex on the authoritative path
(`txn_mutex_`). Every mutation, every query and every journal operation takes
it. It is held across the durable commit, which is a deliberate trade: it makes
the lock ordering a single element, and it is what allows the runtime to promise
that nothing is published before it is durable.

## Lock ordering

Locks are always taken in this order, and never in the reverse:

```
shutdown_mutex_  ->  queue_mutex_        (Submit, worker dequeue, ResolvePending)
shutdown_mutex_  ->  txn_mutex_          (the close/compact phase of Shutdown)
txn_mutex_       ->  (no other lock)     (transactions and queries)
observer_mutex_  ->  (no other lock)     (only to copy the observer)
AskHandle::mutex_ -> (no other lock)
```

`txn_mutex_` is never held while `queue_mutex_` or `observer_mutex_` is taken,
and `queue_mutex_` is never held while `txn_mutex_` is taken. That is what makes
the order a hierarchy rather than a hope.

## The audit checklist

**Read-lock to write-lock upgrades.** There are no read locks in the runtime.
`std::shared_mutex` is not used anywhere, so an upgrade attempt cannot be
written, let alone deadlock. Readers (`Summary`, `VerifyConservation`,
`CanonicalState`, `FindSite`, `FindDecision`, `RecentDecisions`) take the same
`txn_mutex_` a writer takes and return values, never references into guarded
state.

**Write locks held while calling code that can acquire the same state.** The
transaction calls the journal, the byte log and the snapshot store while holding
`txn_mutex_`. None of them calls back into the kernel: the journal's replay
entry points are only used during recovery, before any worker exists, and the
snapshot encoders are pure functions of the kernel's public queries. There is no
path from the store back into `BrokerCore`.

**Mutex re-entry through callbacks.** There are no callbacks on the
authoritative path. The one observer hook is invoked after the transaction has
released `txn_mutex_`, outside every lock; it is copied under
`observer_mutex_` first, precisely so that a slow or blocking observer does not
hold that mutex either. `an_observer_may_re_enter_the_broker_without_deadlocking`
calls `Summary()` and `VerifyConservation()` from inside an observer and
requires both to return.

**Event or callback emission while internal locks are held.** The only emission
is the decision observer, and it happens after `ExecuteAsk` has returned, which
means after `txn_mutex_` was released.

**Shutdown while holding locks needed by workers.** `Shutdown` takes
`shutdown_mutex_`, sets the atomic stopping flag, notifies the queue, resolves
what is still queued (for the abandon mode), then **releases `queue_mutex_`**
before joining. Workers need `queue_mutex_` to observe the stop; if shutdown
held it while joining, they could never finish.

**Joining workers while holding state they require.** The join happens with no
lock held at all; `txn_mutex_` is only taken afterwards, to flush and close the
journal. A worker still inside a transaction therefore completes normally and
the join returns.

**Reversed lock ordering in cancellation or recovery.** Cancellation is an
atomic flag with no lock. Recovery runs in `BrokerService::Open` before any
worker is started, so there is no second thread to order against.

**Callbacks re-entering mutable state.** See above: the observer may re-enter
the broker and cannot observe or mutate state mid-transaction, because the
transaction has already completed and released the mutex.

**Shutdown waiting on work while preventing its completion.** This was a real
defect found by the concurrency suite: the first shutdown implementation resolved
every queued submission as `shutting_down` even in drain mode, so work that had
been accepted was answered as failed instead of being finished. The fix is
explicit: in `Drain` the workers keep taking work until the queue is empty, the
sweep after the join is a safety net for anything that arrived in the gap, and
only `Abandon` cancels queued work up front. `shutdown_drains_accepted_work`
asserts that every accepted submission gets a real outcome and that the totals
match exactly what was accepted.

**Inconsistent nested acquisition ordering.** Encoded above. The whole runtime
has four mutexes in the service (`shutdown_mutex_`, `queue_mutex_`,
`txn_mutex_`, `observer_mutex_`), one per `AskHandle`, and three atomics
(`stopping_`, `failed_` and each `CancelToken`'s flag). No cycle exists among
them, and every one of them is listed in the ordering table above.

**Stale asynchronous completion mutating newer state.** Every plan carries the
broker epoch and the ledger state version it was computed against, and
`CommitPlan` refuses a mismatch with `fenced` before touching anything. Restarts
advance the epoch, so a plan made by a previous incarnation can never be
committed by the next one.
`a_stale_epoch_can_never_commit` and `a_plan_from_another_ledger_version_is_refused`
are the covering tests. The same fence protects the journal: a record written
under an older epoch is only ever replayed as history, never adopted as current
authority.

**Races between authority revocation, recovery and publication.** Revocation,
publication, recovery and decisions are all serialised by `txn_mutex_`. A
revocation that arrives while an ask is being planned cannot interleave: the ask
either precedes it (and its commitment is then revoked with the generation) or
follows it (and the site is already withdrawn, so the constraint is named).
Recovery happens entirely before the service accepts or runs anything.

**Two writers on one store.** Across processes the store lock file is the fence;
in-process, `txn_mutex_` is. The lock is taken in `BrokerService::Open` before
the journal is read, so a loser never even observes the winner's partial state.

## Hazards that are accepted, and why

* **One mutex, no read concurrency.** Queries serialise behind mutations. The
  alternative — a read/write lock — would introduce exactly the upgrade hazard
  this design removes, and would let a reader observe a state that is mid-commit.
  The measured cost is in the README's benchmark table.
* **The transactional mutex is held across the durability call.** Throughput
  under a durable configuration is therefore bounded by the flush latency of the
  storage stack (measured: see the durable-commit row). Group commit was not
  implemented; adding it would require releasing the mutex between apply and
  flush, which is a different and much harder correctness argument.
* **A durability failure poisons the session.** After a failed commit marker the
  service refuses further mutations until reopened rather than continuing on a
  store whose commit state is unknown. This is a deliberate fail-stop.
