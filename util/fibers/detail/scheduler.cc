// Copyright 2023, Roman Gershman.  All rights reserved.
// See LICENSE for licensing terms.
//
#include "util/fibers/detail/scheduler.h"

#include <condition_variable>
#include <mutex>

#include "base/logging.h"

namespace util {
namespace fb2 {
namespace detail {

namespace ctx = boost::context;

using namespace std;

namespace {

constexpr size_t kSizeOfCtx = sizeof(FiberInterface);  // because of the virtual +8 bytes.
constexpr size_t kSizeOfSH = sizeof(FI_SleepHook);
constexpr size_t kSizeOfLH = sizeof(FI_ListHook);


#if PARKING_ENABLED
template <typename T> void WriteOnce(T src, T* dest) {
  std::atomic_store_explicit(reinterpret_cast<std::atomic<T>*>(dest), src,
                             std::memory_order_relaxed);
}

template <typename T> T ReadOnce(T* src) {
  return std::atomic_load_explicit(reinterpret_cast<std::atomic<T>*>(src),
                                   std::memory_order_relaxed);
}

// Thomas Wang's 64 bit Mix Function.
inline uint64_t MixHash(uint64_t key) {
  key += ~(key << 32);
  key ^= (key >> 22);
  key += ~(key << 13);
  key ^= (key >> 8);
  key += (key << 3);
  key ^= (key >> 15);
  key += ~(key << 27);
  key ^= (key >> 31);
  return key;
}

using WaitQueue = boost::intrusive::slist<
    detail::FiberInterface,
    boost::intrusive::member_hook<detail::FiberInterface, detail::FI_ListHook,
                                  &detail::FiberInterface::list_hook>,
    boost::intrusive::constant_time_size<false>, boost::intrusive::cache_last<true>>;

struct ParkingBucket {
  SpinLockType lock;
  WaitQueue waiters;
  bool was_rehashed = 0;
};

constexpr size_t kSzPB = sizeof(ParkingBucket);

class ParkingHT {
  struct SizedBuckets {
    unsigned num_buckets;
    ParkingBucket* arr;

    SizedBuckets(unsigned shift) : num_buckets(1 << shift) {
      arr = new ParkingBucket[num_buckets];
    }

    unsigned GetBucket(uint64_t hash) const {
      return hash & bucket_mask();
    }

    unsigned bucket_mask() const {
      return num_buckets - 1;
    }
  };

 public:
  ParkingHT();
  ~ParkingHT();

  // if validate returns true, the fiber is not added to the queue.
  bool Emplace(uint64_t token, FiberInterface* fi, absl::FunctionRef<bool()> validate);

  FiberInterface* Remove(uint64_t token, absl::FunctionRef<void(FiberInterface*)> on_hit,
                         absl::FunctionRef<void()> on_miss);
  void RemoveAll(uint64_t token, WaitQueue* wq);

 private:
  void TryRehash(SizedBuckets* cur_sb);

  atomic<SizedBuckets*> buckets_;
  atomic_uint32_t num_entries_{0};
  atomic_bool rehashing_{false};
};

#endif

// ParkingHT* g_parking_ht = nullptr;

using QsbrEpoch = uint32_t;
constexpr QsbrEpoch kEpochInc = 2;
atomic<QsbrEpoch> qsbr_global_epoch{1};  // global is always non-zero.

#if PARKING_ENABLED
// TODO: we could move this checkpoint to the proactor loop.
void qsbr_checkpoint() {
  atomic_thread_fence(memory_order_seq_cst);

  // syncs the local_epoch with the global_epoch.
  WriteOnce(qsbr_global_epoch.load(memory_order_relaxed), &FbInitializer().local_epoch);
}

void qsbr_worker_fiber_offline() {
  atomic_thread_fence(memory_order_release);
  WriteOnce(0U, &FbInitializer().local_epoch);
}

void qsbr_worker_fiber_online() {
  WriteOnce(qsbr_global_epoch.load(memory_order_relaxed), &FbInitializer().local_epoch);
  atomic_thread_fence(memory_order_seq_cst);
}

bool qsbr_sync(uint64_t target) {
  unique_lock lk(g_scheduler_lock, try_to_lock);

  if (!lk)
    return false;

  FbInitializer().local_epoch = target;
  for (TL_FiberInitializer* p = g_fiber_thread_list; p != nullptr; p = p->next) {
    auto local_epoch = ReadOnce(&p->local_epoch);
    if (local_epoch && local_epoch != target) {
      return false;
    }
  }

  return true;
}

ParkingHT::ParkingHT() {
  SizedBuckets* sb = new SizedBuckets(6);
  buckets_.store(sb, memory_order_release);
}

ParkingHT::~ParkingHT() {
  SizedBuckets* sb = buckets_.load(memory_order_relaxed);

  DVLOG(1) << "Destroying ParkingHT with " << sb->num_buckets << " buckets";

  for (unsigned i = 0; i < sb->num_buckets; ++i) {
    ParkingBucket* pb = sb->arr + i;
    SpinLockHolder h(&pb->lock);
    CHECK(pb->waiters.empty());
  }
  delete[] sb->arr;
  delete sb;
}

// if validate returns true we do not park
bool ParkingHT::Emplace(uint64_t token, FiberInterface* fi, absl::FunctionRef<bool()> validate) {
  uint32_t num_items = 0;
  unsigned bucket = 0;
  SizedBuckets* sb = nullptr;
  uint64_t hash = MixHash(token);
  bool res = false;

  while (true) {
    sb = buckets_.load(memory_order_acquire);
    DCHECK(sb);
    bucket = sb->GetBucket(hash);
    VLOG(1) << "Emplace: token=" << token << " bucket=" << bucket;

    ParkingBucket* pb = sb->arr + bucket;
    {
      SpinLockHolder h(&pb->lock);

      if (!pb->was_rehashed) {  // has grown
        if (validate()) {
          break;
        }

        fi->set_park_token(token);
        pb->waiters.push_front(*fi);
        num_items = num_entries_.fetch_add(1, memory_order_relaxed);
        res = true;
        break;
      }
    }
  }

  if (res) {
    DVLOG(2) << "EmplaceEnd: token=" << token << " bucket=" << bucket;

    if (num_items > sb->num_buckets) {
      TryRehash(sb);
    }
  } else {
    qsbr_checkpoint();
  }

  // we do not call qsbr_checkpoint here because we are going to park
  // and call qsbr_worker_fiber_offline.
  return res;
}

FiberInterface* ParkingHT::Remove(uint64_t token, absl::FunctionRef<void(FiberInterface*)> on_hit,
                                  absl::FunctionRef<void()> on_miss) {
  uint64_t hash = MixHash(token);
  SizedBuckets* sb = nullptr;
  while (true) {
    sb = buckets_.load(memory_order_acquire);
    unsigned bucket = sb->GetBucket(hash);
    ParkingBucket* pb = sb->arr + bucket;
    {
      SpinLockHolder h(&pb->lock);
      VLOG(1) << "Remove: token=" << token << " bucket=" << bucket;

      if (!pb->was_rehashed) {
        for (auto it = pb->waiters.begin(); it != pb->waiters.end(); ++it) {
          if (it->park_token() == token) {
            FiberInterface* fi = &*it;
            pb->waiters.erase(it);
            auto prev = num_entries_.fetch_sub(1, memory_order_relaxed);
            DCHECK_GT(prev, 0u);
            on_hit(fi);
            // qsbr_checkpoint();

            return fi;
          }
        }
        on_miss();
        return nullptr;
      }
    }
  }

  qsbr_checkpoint();
  return nullptr;
}

void ParkingHT::RemoveAll(uint64_t token, WaitQueue* wq) {
  uint64_t hash = MixHash(token);
  SizedBuckets* sb = nullptr;

  while (true) {
    sb = buckets_.load(memory_order_acquire);
    unsigned bucket = sb->GetBucket(hash);
    ParkingBucket* pb = sb->arr + bucket;
    {
      SpinLockHolder h(&pb->lock);
      if (!pb->was_rehashed) {
        auto it = pb->waiters.begin();
        while (it != pb->waiters.end()) {
          if (it->park_token() != token) {
            ++it;
            continue;
          }
          FiberInterface* fi = &*it;
          it = pb->waiters.erase(it);
          wq->push_back(*fi);
          auto prev = num_entries_.fetch_sub(1, memory_order_relaxed);
          DCHECK_GT(prev, 0u);
        }
        break;
      }
    }
  }
  qsbr_checkpoint();
}

void ParkingHT::TryRehash(SizedBuckets* cur_sb) {
  if (rehashing_.exchange(true, memory_order_acquire)) {
    return;
  }

  SizedBuckets* sb = buckets_.load(memory_order_relaxed);
  if (sb != cur_sb) {
    rehashing_.store(false, memory_order_release);
    return;
  }

  DVLOG(1) << "Rehashing parking hash table from " << sb->num_buckets;

  // log2(x)
  static_assert(__builtin_ctz(32) == 5);

  SizedBuckets* new_sb = new SizedBuckets(__builtin_ctz(sb->num_buckets) + 2);
  for (unsigned i = 0; i < sb->num_buckets; ++i) {
    sb->arr[i].lock.Lock();
  }
  for (unsigned i = 0; i < sb->num_buckets; ++i) {
    ParkingBucket* pb = sb->arr + i;
    pb->was_rehashed = true;
    while (!pb->waiters.empty()) {
      FiberInterface* fi = &pb->waiters.front();
      pb->waiters.pop_front();
      uint64_t hash = MixHash(fi->park_token());
      unsigned bucket = new_sb->GetBucket(hash);
      ParkingBucket* new_pb = new_sb->arr + bucket;
      new_pb->waiters.push_back(*fi);
    }
  }
  buckets_.store(new_sb, memory_order_release);

  for (unsigned i = 0; i < sb->num_buckets; ++i) {
    sb->arr[i].lock.Unlock();
  }

  uint64_t next_epoch = qsbr_global_epoch.fetch_add(kEpochInc, memory_order_relaxed) + kEpochInc;

  FbInitializer().sched->Defer(next_epoch, [sb] {
    DVLOG(1) << "Destroying old SizedBuckets with " << sb->num_buckets << " buckets";
    delete sb;
  });

  rehashing_.store(false, memory_order_release);
}
#endif

class DispatcherImpl final : public FiberInterface {
 public:
  DispatcherImpl(ctx::preallocated const& palloc, ctx::fixedsize_stack&& salloc,
                 Scheduler* sched) noexcept;
  ~DispatcherImpl();

  bool is_terminating() const {
    return is_terminating_;
  }

  void Notify() {
    unique_lock<mutex> lk(mu_);
    wake_suspend_ = true;
    cnd_.notify_one();
  }

 private:
  void DefaultDispatch(Scheduler* sched);

  ctx::fiber Run(ctx::fiber&& c);

  bool is_terminating_ = false;

  // This is used to wake up the scheduler from sleep.
  bool wake_suspend_ = false;

  mutex mu_;
  condition_variable cnd_;
};

DispatcherImpl* MakeDispatcher(Scheduler* sched) {
  ctx::fixedsize_stack salloc;
  ctx::stack_context sctx = salloc.allocate();
  ctx::preallocated palloc = MakePreallocated<DispatcherImpl>(sctx);

  void* sp_ptr = palloc.sp;

  // placement new of context on top of fiber's stack
  return new (sp_ptr) DispatcherImpl{std::move(palloc), std::move(salloc), sched};
}

// DispatcherImpl implementation.
DispatcherImpl::DispatcherImpl(ctx::preallocated const& palloc, ctx::fixedsize_stack&& salloc,
                               detail::Scheduler* sched) noexcept
    : FiberInterface{DISPATCH, 0, "_dispatch"} {
  entry_ = ctx::fiber(std::allocator_arg, palloc, salloc,
                      [this](ctx::fiber&& caller) { return Run(std::move(caller)); });
  scheduler_ = sched;
}

DispatcherImpl::~DispatcherImpl() {
  DVLOG(1) << "~DispatcherImpl";

  DCHECK(!entry_);
}

ctx::fiber DispatcherImpl::Run(ctx::fiber&& c) {
  if (c) {
    // We context switched from intrusive_ptr_release and this object is destroyed.
    return std::move(c);
  }

  // Normal SwitchTo operation.

  // auto& fb_init = detail::FbInitializer();
  if (scheduler_->policy()) {
    scheduler_->policy()->Run(scheduler_);
  } else {
    DefaultDispatch(scheduler_);
  }

  DVLOG(1) << "Dispatcher exiting, switching to main_cntx";
  is_terminating_ = true;

  // Like with worker fibers, we switch to another fiber, but in this case to the main fiber.
  // We will come back here during the deallocation of DispatcherImpl from intrusive_ptr_release
  // in order to return from Run() and come back to main context.
  auto fc = scheduler_->main_context()->SwitchTo();

  DCHECK(fc);  // Should bring us back to main, into intrusive_ptr_release.
  return fc;
}

void DispatcherImpl::DefaultDispatch(Scheduler* sched) {
  DCHECK(FiberActive() == this);
  DCHECK(!wait_hook.is_linked());

  while (true) {
    if (sched->IsShutdown()) {
      if (sched->num_worker_fibers() == 0)
        break;
    }

    sched->ProcessRemoteReady();
    if (sched->HasSleepingFibers()) {
      sched->ProcessSleep();
    }

    if (sched->HasReady()) {
      FiberInterface* fi = sched->PopReady();
      DCHECK(!fi->list_hook.is_linked());
      DCHECK(!fi->sleep_hook.is_linked());
      sched->AddReady(this);

      DVLOG(2) << "Switching to " << fi->name();
      // qsbr_worker_fiber_online();
      fi->SwitchTo();
      DCHECK(!list_hook.is_linked());
      DCHECK(FiberActive() == this);
      // qsbr_worker_fiber_offline();
    } else {
      sched->DestroyTerminated();

      bool has_sleeping = sched->HasSleepingFibers();
      auto cb = [this]() { return wake_suspend_; };

      unique_lock<mutex> lk{mu_};
      if (has_sleeping) {
        auto next_tp = sched->NextSleepPoint();
        cnd_.wait_until(lk, next_tp, move(cb));
      } else {
        cnd_.wait(lk, move(cb));
      }
      wake_suspend_ = false;
    }
    sched->RunDeferred();
  }
  sched->DestroyTerminated();
}

}  // namespace

Scheduler::Scheduler(FiberInterface* main_cntx) : main_cntx_(main_cntx) {
  DCHECK(!main_cntx->scheduler_);
  main_cntx->scheduler_ = this;
  dispatch_cntx_.reset(MakeDispatcher(this));
}

Scheduler::~Scheduler() {
  shutdown_ = true;
  DCHECK(main_cntx_ == FiberActive());

  while (HasReady()) {
    FiberInterface* fi = PopReady();
    DCHECK(!fi->wait_hook.is_linked());
    DCHECK(!fi->sleep_hook.is_linked());
    fi->SwitchTo();
  }

  DispatcherImpl* dimpl = static_cast<DispatcherImpl*>(dispatch_cntx_.get());
  if (!dimpl->is_terminating()) {
    DVLOG(1) << "~Scheduler switching to dispatch " << dispatch_cntx_->IsDefined();
    auto fc = dispatch_cntx_->SwitchTo();
    CHECK(!fc);
    CHECK(dimpl->is_terminating());
  }
  delete custom_policy_;
  custom_policy_ = nullptr;

  CHECK_EQ(0u, num_worker_fibers_);

  // destroys the stack and the object via intrusive_ptr_release.
  dispatch_cntx_.reset();
  DestroyTerminated();
}

ctx::fiber_context Scheduler::Preempt() {
  if (ready_queue_.empty()) {
    // All user fibers are inactive, we should switch back to the dispatcher.
    return dispatch_cntx_->SwitchTo();
  }

  DCHECK(!ready_queue_.empty());
  FiberInterface* fi = &ready_queue_.front();
  ready_queue_.pop_front();

  return fi->SwitchTo();
}

void Scheduler::AddReady(FiberInterface* fibi) {
  DCHECK(!fibi->list_hook.is_linked());
  ready_queue_.push_back(*fibi);

  // Case of notifications coming to a sleeping fiber.
  if (fibi->sleep_hook.is_linked()) {
    sleep_queue_.erase(sleep_queue_.iterator_to(*fibi));
  }
}

void Scheduler::ScheduleFromRemote(FiberInterface* cntx) {
  DVLOG(1) << "ScheduleFromRemote " << cntx->name();

  remote_ready_queue_.Push(cntx);

  if (custom_policy_) {
    custom_policy_->Notify();
  } else {
    DispatcherImpl* dimpl = static_cast<DispatcherImpl*>(dispatch_cntx_.get());
    dimpl->Notify();
  }
}

void Scheduler::Attach(FiberInterface* cntx) {
  cntx->scheduler_ = this;
  if (cntx->type() == FiberInterface::WORKER) {
    ++num_worker_fibers_;
  }
}

void Scheduler::ScheduleTermination(FiberInterface* cntx) {
  terminate_queue_.push_back(*cntx);
  if (cntx->type() == FiberInterface::WORKER) {
    --num_worker_fibers_;
  }
}

void Scheduler::DestroyTerminated() {
  while (!terminate_queue_.empty()) {
    FiberInterface* tfi = &terminate_queue_.front();
    terminate_queue_.pop_front();
    DVLOG(2) << "Releasing terminated " << tfi->name_;

    // maybe someone holds a Fiber handle and waits for the fiber to join.
    intrusive_ptr_release(tfi);
  }
}

void Scheduler::WaitUntil(chrono::steady_clock::time_point tp, FiberInterface* me) {
  DCHECK(!me->sleep_hook.is_linked());
  DCHECK(!me->list_hook.is_linked());

  me->tp_ = tp;
  sleep_queue_.insert(*me);
  auto fc = Preempt();
  DCHECK(!fc);
}

void Scheduler::ProcessRemoteReady() {
  while (true) {
    FiberInterface* fi = remote_ready_queue_.Pop();
    if (!fi)
      break;

    // It could be that we pulled a fiber from remote_ready_queue_ and added it to ready_queue_,
    // but meanwhile a remote thread adds the same fiber again to the remote_ready_queue_,
    // even before ready_queue_ has even been processed. We should not push the already added fiber
    // to ready_queue.
    if (fi->list_hook.is_linked())
      continue;

    DVLOG(2) << "set ready " << fi->name();
    AddReady(fi);
  }
}

void Scheduler::ProcessSleep() {
  DCHECK(!sleep_queue_.empty());
  std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
  DVLOG(3) << "now " << now.time_since_epoch().count();

  do {
    auto it = sleep_queue_.begin();
    if (it->tp_ > now)
      break;

    FiberInterface& fi = *it;
    sleep_queue_.erase(it);

    DCHECK(!fi.list_hook.is_linked());
    DVLOG(2) << "timeout for " << fi.name();
    ready_queue_.push_back(fi);
  } while (!sleep_queue_.empty());
}

void Scheduler::AttachCustomPolicy(DispatchPolicy* policy) {
  CHECK(custom_policy_ == nullptr);
  custom_policy_ = policy;
}

void Scheduler::RunDeferred() {
#if PARKING_ENABLED
  bool skip_validation = false;

  while (!deferred_cb_.empty()) {
    const auto& k_v = deferred_cb_.back();
    if (skip_validation) {
      k_v.second();
      deferred_cb_.pop_back();

      continue;
    }

    if (!qsbr_sync(k_v.first))
      break;

    k_v.second();
    skip_validation = true;
    deferred_cb_.pop_back();
  }
#endif
}

#if PARKING_ENABLED
void FiberInterface::NotifyParked(FiberInterface* other) {
  DCHECK(other->scheduler_ && other->scheduler_ != scheduler_);

  uintptr_t token = uintptr_t(other);

  // to avoid the missed notification case, we reset the flag even if we had not find the fiber.
  // this handles the scenario, where the parking fiber started async process, but has not been
  // added to the parking lot yet and now this process tries to notify it.
  FiberInterface* item = g_parking_ht->Remove(
      token,
      [](FiberInterface* fibi) {
        fibi->flags_.fetch_and(~kParkingInProgress, memory_order_relaxed);
      },
      [other] { return other->flags_.fetch_and(~kParkingInProgress, memory_order_relaxed); });

  if (item == nullptr) {  // The fiber has not parked yet.
    // we reset the flag, so "other" will skip the suspension.
    return;
  }
  CHECK(item == other);
  other->scheduler_->ScheduleFromRemote(other);
}

FiberInterface* FiberInterface::NotifyParked(uint64_t token) {
  FiberInterface* removed = g_parking_ht->Remove(
      token, [](FiberInterface* fibi) {}, [] {});
  if (removed) {
    ActivateOther(removed);
  }
  return removed;
}

void FiberInterface::NotifyAllParked(uint64_t token) {
  WaitQueue res;

  g_parking_ht->RemoveAll(token, &res);
  while (!res.empty()) {
    auto& fibi = res.front();
    res.pop_front();
    ActivateOther(&fibi);
  }
}

void FiberInterface::SuspendUntilWakeup() {
  uintptr_t token = uintptr_t(this);
  bool parked = g_parking_ht->Emplace(token, this, [this] {
    // if parking process was stopped we should not park.
    return (flags_.load(memory_order_relaxed) & kParkingInProgress) == 0;
  });

  if (parked) {
    scheduler_->Preempt();
  }
}

bool FiberInterface::SuspendConditionally(uint64_t token, absl::FunctionRef<bool()> validate) {
  bool parked = g_parking_ht->Emplace(token, this, std::move(validate));

  if (parked) {
    scheduler_->Preempt();
    return true;
  }

  return false;
}

#endif

}  // namespace detail

DispatchPolicy::~DispatchPolicy() {
}

}  // namespace fb2
}  // namespace util
