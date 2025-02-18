// Copyright 2021 gRPC authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef GRPC_CORE_LIB_PROMISE_ACTIVITY_H
#define GRPC_CORE_LIB_PROMISE_ACTIVITY_H

#include <grpc/support/port_platform.h>

#include <stdint.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <utility>

#include "absl/base/thread_annotations.h"
#include "absl/status/status.h"
#include "absl/types/optional.h"
#include "absl/types/variant.h"

#include <grpc/support/log.h>

#include "src/core/lib/gprpp/construct_destruct.h"
#include "src/core/lib/gprpp/no_destruct.h"
#include "src/core/lib/gprpp/orphanable.h"
#include "src/core/lib/gprpp/sync.h"
#include "src/core/lib/promise/context.h"
#include "src/core/lib/promise/detail/promise_factory.h"
#include "src/core/lib/promise/detail/status.h"
#include "src/core/lib/promise/poll.h"

namespace grpc_core {

// A Wakeable object is used by queues to wake activities.
class Wakeable {
 public:
  // Wake up the underlying activity.
  // After calling, this Wakeable cannot be used again.
  virtual void Wakeup() = 0;
  // Drop this wakeable without waking up the underlying activity.
  virtual void Drop() = 0;

 protected:
  inline ~Wakeable() {}
};

namespace activity_detail {
struct Unwakeable final : public Wakeable {
  void Wakeup() override {}
  void Drop() override {}
};
static Unwakeable* unwakeable() {
  return NoDestructSingleton<Unwakeable>::Get();
}
}  // namespace activity_detail

class AtomicWaker;

// An owning reference to a Wakeable.
// This type is non-copyable but movable.
class Waker {
 public:
  explicit Waker(Wakeable* wakeable) : wakeable_(wakeable) {}
  Waker() : Waker(activity_detail::unwakeable()) {}
  ~Waker() { wakeable_->Drop(); }
  Waker(const Waker&) = delete;
  Waker& operator=(const Waker&) = delete;
  Waker(Waker&& other) noexcept : wakeable_(other.Take()) {}
  Waker& operator=(Waker&& other) noexcept {
    std::swap(wakeable_, other.wakeable_);
    return *this;
  }

  // Wake the underlying activity.
  void Wakeup() { Take()->Wakeup(); }

  template <typename H>
  friend H AbslHashValue(H h, const Waker& w) {
    return H::combine(std::move(h), w.wakeable_);
  }

  bool operator==(const Waker& other) const noexcept {
    return wakeable_ == other.wakeable_;
  }

 private:
  friend class AtomicWaker;

  Wakeable* Take() {
    return std::exchange(wakeable_, activity_detail::unwakeable());
  }

  Wakeable* wakeable_;
};

// An atomic variant of Waker - this type is non-copyable and non-movable.
class AtomicWaker {
 public:
  explicit AtomicWaker(Wakeable* wakeable) : wakeable_(wakeable) {}
  AtomicWaker() : AtomicWaker(activity_detail::unwakeable()) {}
  explicit AtomicWaker(Waker waker) : AtomicWaker(waker.Take()) {}
  ~AtomicWaker() { wakeable_.load(std::memory_order_acquire)->Drop(); }
  AtomicWaker(const AtomicWaker&) = delete;
  AtomicWaker& operator=(const AtomicWaker&) = delete;
  AtomicWaker(AtomicWaker&& other) noexcept = delete;
  AtomicWaker& operator=(AtomicWaker&& other) noexcept = delete;

  // Wake the underlying activity.
  void Wakeup() { Take()->Wakeup(); }

  // Return true if there is a not-unwakeable wakeable present.
  bool Armed() const noexcept {
    return wakeable_.load(std::memory_order_relaxed) !=
           activity_detail::unwakeable();
  }

  // Set to some new waker
  void Set(Waker waker) {
    wakeable_.exchange(waker.Take(), std::memory_order_acq_rel)->Wakeup();
  }

 private:
  Wakeable* Take() {
    return wakeable_.exchange(activity_detail::unwakeable(),
                              std::memory_order_acq_rel);
  }

  std::atomic<Wakeable*> wakeable_;
};

// An Activity tracks execution of a single promise.
// It executes the promise under a mutex.
// When the promise stalls, it registers the containing activity to be woken up
// later.
// The activity takes a callback, which will be called exactly once with the
// result of execution.
// Activity execution may be cancelled by simply deleting the activity. In such
// a case, if execution had not already finished, the done callback would be
// called with absl::CancelledError().
class Activity : public Orphanable {
 public:
  // Force wakeup from the outside.
  // This should be rarely needed, and usages should be accompanied with a note
  // on why it's not possible to wakeup with a Waker object.
  // Nevertheless, it's sometimes useful for integrations with Activity to force
  // an Activity to repoll.
  void ForceWakeup() { MakeOwningWaker().Wakeup(); }

  // Force the current activity to immediately repoll if it doesn't complete.
  virtual void ForceImmediateRepoll() = 0;

  // Return the current activity.
  // Additionally:
  // - assert that there is a current activity (and catch bugs if there's not)
  // - indicate to thread safety analysis that the current activity is indeed
  //   locked
  // - back up that assertation with a runtime check in debug builds (it's
  //   prohibitively expensive in non-debug builds)
  static Activity* current() { return g_current_activity_; }

  // Produce an activity-owning Waker. The produced waker will keep the activity
  // alive until it's awoken or dropped.
  virtual Waker MakeOwningWaker() = 0;

  // Produce a non-owning Waker. The waker will own a small heap allocated weak
  // pointer to this activity. This is more suitable for wakeups that may not be
  // delivered until long after the activity should be destroyed.
  virtual Waker MakeNonOwningWaker() = 0;

 protected:
  // Check if this activity is the current activity executing on the current
  // thread.
  bool is_current() const { return this == g_current_activity_; }
  // Check if there is an activity executing on the current thread.
  static bool have_current() { return g_current_activity_ != nullptr; }
  // Set the current activity at construction, clean it up at destruction.
  class ScopedActivity {
   public:
    explicit ScopedActivity(Activity* activity)
        : prior_activity_(g_current_activity_) {
      g_current_activity_ = activity;
    }
    ~ScopedActivity() { g_current_activity_ = prior_activity_; }
    ScopedActivity(const ScopedActivity&) = delete;
    ScopedActivity& operator=(const ScopedActivity&) = delete;

   private:
    Activity* const prior_activity_;
  };

 private:
  // Set during RunLoop to the Activity that's executing.
  // Being set implies that mu_ is held.
  static thread_local Activity* g_current_activity_;
};

// Owned pointer to one Activity.
using ActivityPtr = OrphanablePtr<Activity>;

namespace promise_detail {

template <typename Context>
class ContextHolder {
 public:
  using ContextType = Context;

  explicit ContextHolder(Context value) : value_(std::move(value)) {}
  Context* GetContext() { return &value_; }

 private:
  Context value_;
};

template <typename Context>
class ContextHolder<Context*> {
 public:
  using ContextType = Context;

  explicit ContextHolder(Context* value) : value_(value) {}
  Context* GetContext() { return value_; }

 private:
  Context* value_;
};

template <typename Context, typename Deleter>
class ContextHolder<std::unique_ptr<Context, Deleter>> {
 public:
  using ContextType = Context;

  explicit ContextHolder(std::unique_ptr<Context, Deleter> value)
      : value_(std::move(value)) {}
  Context* GetContext() { return value_.get(); }

 private:
  std::unique_ptr<Context, Deleter> value_;
};

template <typename HeldContext>
using ContextTypeFromHeld = typename ContextHolder<HeldContext>::ContextType;

template <typename... Contexts>
class ActivityContexts : public ContextHolder<Contexts>... {
 public:
  explicit ActivityContexts(Contexts&&... contexts)
      : ContextHolder<Contexts>(std::forward<Contexts>(contexts))... {}

  class ScopedContext : public Context<ContextTypeFromHeld<Contexts>>... {
   public:
    explicit ScopedContext(ActivityContexts* contexts)
        : Context<ContextTypeFromHeld<Contexts>>(
              static_cast<ContextHolder<Contexts>*>(contexts)
                  ->GetContext())... {
      // Silence `unused-but-set-parameter` in case of Contexts = {}
      (void)contexts;
    }
  };
};

// A free standing activity: an activity that owns its own synchronization and
// memory.
// The alternative is an activity that's somehow tied into another system, for
// instance the type seen in promise_based_filter.h as we're transitioning from
// the old filter stack to the new system.
// FreestandingActivity is-a Wakeable, but needs to increment a refcount before
// returning that Wakeable interface. Additionally, we want to keep
// FreestandingActivity as small as is possible, since it will be used
// everywhere. So we use inheritance to provide the Wakeable interface: this
// makes it zero sized, and we make the inheritance private to prevent
// accidental casting.
class FreestandingActivity : public Activity, private Wakeable {
 public:
  Waker MakeOwningWaker() final {
    Ref();
    return Waker(this);
  }
  Waker MakeNonOwningWaker() final;

  void Orphan() final {
    Cancel();
    Unref();
  }

  void ForceImmediateRepoll() final {
    mu_.AssertHeld();
    SetActionDuringRun(ActionDuringRun::kWakeup);
  }

 protected:
  // Action received during a run, in priority order.
  // If more than one action is received during a run, we use max() to resolve
  // which one to report (so Cancel overrides Wakeup).
  enum class ActionDuringRun : uint8_t {
    kNone,    // No action occured during run.
    kWakeup,  // A wakeup occured during run.
    kCancel,  // Cancel was called during run.
  };

  inline ~FreestandingActivity() override {
    if (handle_) {
      DropHandle();
    }
  }

  // Check if we got an internal wakeup since the last time this function was
  // called.
  ActionDuringRun GotActionDuringRun() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
    return std::exchange(action_during_run_, ActionDuringRun::kNone);
  }

  // Implementors of Wakeable::Wakeup should call this after the wakeup has
  // completed.
  void WakeupComplete() { Unref(); }

  // Set the action that occured during this run.
  // We use max to combine actions so that cancellation overrides wakeups.
  void SetActionDuringRun(ActionDuringRun action)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
    action_during_run_ = std::max(action_during_run_, action);
  }

  Mutex* mu() ABSL_LOCK_RETURNED(mu_) { return &mu_; }

 private:
  class Handle;

  // Cancel execution of the underlying promise.
  virtual void Cancel() = 0;

  void Ref() { refs_.fetch_add(1, std::memory_order_relaxed); }
  void Unref() {
    if (1 == refs_.fetch_sub(1, std::memory_order_acq_rel)) {
      delete this;
    }
  }

  // Return a Handle instance with a ref so that it can be stored waiting for
  // some wakeup.
  Handle* RefHandle() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);
  // If our refcount is non-zero, ref and return true.
  // Otherwise, return false.
  bool RefIfNonzero();
  // Drop the (proved existing) wait handle.
  void DropHandle() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  // All promise execution occurs under this mutex.
  Mutex mu_;

  // Current refcount.
  std::atomic<uint32_t> refs_{1};
  // If wakeup is called during Promise polling, we set this to Wakeup and
  // repoll. If cancel is called during Promise polling, we set this to Cancel
  // and cancel at the end of polling.
  ActionDuringRun action_during_run_ ABSL_GUARDED_BY(mu_) =
      ActionDuringRun::kNone;
  // Handle for long waits. Allows a very small weak pointer type object to
  // queue for wakeups while Activity may be deleted earlier.
  Handle* handle_ ABSL_GUARDED_BY(mu_) = nullptr;
};

// Implementation details for an Activity of an arbitrary type of promise.
// There should exist a static function:
// struct WakeupScheduler {
//   template <typename ActivityType>
//   void ScheduleWakeup(ActivityType* activity);
// };
// This function should arrange that activity->RunScheduledWakeup() be invoked
// at the earliest opportunity.
// It can assume that activity will remain live until RunScheduledWakeup() is
// invoked, and that a given activity will not be concurrently scheduled again
// until its RunScheduledWakeup() has been invoked.
// We use private inheritance here as a way of getting private members for
// each of the contexts.
// TODO(ctiller): We can probably reconsider the private inheritance here
// when we move away from C++11 and have more powerful template features.
template <class F, class WakeupScheduler, class OnDone, typename... Contexts>
class PromiseActivity final : public FreestandingActivity,
                              private ActivityContexts<Contexts...> {
 public:
  using Factory = PromiseFactory<void, F>;
  using ResultType = typename Factory::Promise::Result;

  PromiseActivity(F promise_factory, WakeupScheduler wakeup_scheduler,
                  OnDone on_done, Contexts&&... contexts)
      : FreestandingActivity(),
        ActivityContexts<Contexts...>(std::forward<Contexts>(contexts)...),
        wakeup_scheduler_(std::move(wakeup_scheduler)),
        on_done_(std::move(on_done)) {
    // Lock, construct an initial promise from the factory, and step it.
    // This may hit a waiter, which could expose our this pointer to other
    // threads, meaning we do need to hold this mutex even though we're still
    // constructing.
    mu()->Lock();
    auto status = Start(Factory(std::move(promise_factory)));
    mu()->Unlock();
    // We may complete immediately.
    if (status.has_value()) {
      on_done_(std::move(*status));
    }
  }

  ~PromiseActivity() override {
    // We shouldn't destruct without calling Cancel() first, and that must get
    // us to be done_, so we assume that and have no logic to destruct the
    // promise here.
    GPR_ASSERT(done_);
  }

  void RunScheduledWakeup() {
    GPR_ASSERT(wakeup_scheduled_.exchange(false, std::memory_order_acq_rel));
    Step();
    WakeupComplete();
  }

 private:
  using typename ActivityContexts<Contexts...>::ScopedContext;

  void Cancel() final {
    if (Activity::is_current()) {
      mu()->AssertHeld();
      SetActionDuringRun(ActionDuringRun::kCancel);
      return;
    }
    bool was_done;
    {
      MutexLock lock(mu());
      // Check if we were done, and flag done.
      was_done = done_;
      if (!done_) {
        ScopedActivity scoped_activity(this);
        ScopedContext contexts(this);
        MarkDone();
      }
    }
    // If we were not done, then call the on_done callback.
    if (!was_done) {
      on_done_(absl::CancelledError());
    }
  }

  // Wakeup this activity. Arrange to poll the activity again at a convenient
  // time: this could be inline if it's deemed safe, or it could be by passing
  // the activity to an external threadpool to run. If the activity is already
  // running on this thread, a note is taken of such and the activity is
  // repolled if it doesn't complete.
  void Wakeup() final {
    // If there is an active activity, but hey it's us, flag that and we'll loop
    // in RunLoop (that's calling from above here!).
    if (Activity::is_current()) {
      mu()->AssertHeld();
      SetActionDuringRun(ActionDuringRun::kWakeup);
      WakeupComplete();
      return;
    }
    if (!wakeup_scheduled_.exchange(true, std::memory_order_acq_rel)) {
      // Can't safely run, so ask to run later.
      wakeup_scheduler_.ScheduleWakeup(this);
    } else {
      // Already a wakeup scheduled for later, drop ref.
      WakeupComplete();
    }
  }

  // Drop a wakeup
  void Drop() final { this->WakeupComplete(); }

  // Notification that we're no longer executing - it's ok to destruct the
  // promise.
  void MarkDone() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu()) {
    GPR_ASSERT(!done_);
    done_ = true;
    Destruct(&promise_holder_.promise);
  }

  // In response to Wakeup, run the Promise state machine again until it
  // settles. Then check for completion, and if we have completed, call on_done.
  void Step() ABSL_LOCKS_EXCLUDED(mu()) {
    // Poll the promise until things settle out under a lock.
    mu()->Lock();
    if (done_) {
      // We might get some spurious wakeups after finishing.
      mu()->Unlock();
      return;
    }
    auto status = RunStep();
    mu()->Unlock();
    if (status.has_value()) {
      on_done_(std::move(*status));
    }
  }

  // The main body of a step: set the current activity, and any contexts, and
  // then run the main polling loop. Contained in a function by itself in
  // order to keep the scoping rules a little easier in Step().
  absl::optional<ResultType> RunStep() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu()) {
    ScopedActivity scoped_activity(this);
    ScopedContext contexts(this);
    return StepLoop();
  }

  // Similarly to RunStep, but additionally construct the promise from a
  // promise factory before entering the main loop. Called once from the
  // constructor.
  absl::optional<ResultType> Start(Factory promise_factory)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu()) {
    ScopedActivity scoped_activity(this);
    ScopedContext contexts(this);
    Construct(&promise_holder_.promise, promise_factory.Once());
    return StepLoop();
  }

  // Until there are no wakeups from within and the promise is incomplete:
  // poll the promise.
  absl::optional<ResultType> StepLoop() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu()) {
    GPR_ASSERT(is_current());
    while (true) {
      // Run the promise.
      GPR_ASSERT(!done_);
      auto r = promise_holder_.promise();
      if (auto* status = absl::get_if<kPollReadyIdx>(&r)) {
        // If complete, destroy the promise, flag done, and exit this loop.
        MarkDone();
        return IntoStatus(status);
      }
      // Continue looping til no wakeups occur.
      switch (GotActionDuringRun()) {
        case ActionDuringRun::kNone:
          return {};
        case ActionDuringRun::kWakeup:
          break;
        case ActionDuringRun::kCancel:
          MarkDone();
          return absl::CancelledError();
      }
    }
  }

  using Promise = typename Factory::Promise;
  // Scheduler for wakeups
  GPR_NO_UNIQUE_ADDRESS WakeupScheduler wakeup_scheduler_;
  // Callback on completion of the promise.
  GPR_NO_UNIQUE_ADDRESS OnDone on_done_;
  // Has execution completed?
  GPR_NO_UNIQUE_ADDRESS bool done_ ABSL_GUARDED_BY(mu()) = false;
  // Is there a wakeup scheduled?
  GPR_NO_UNIQUE_ADDRESS std::atomic<bool> wakeup_scheduled_{false};
  // We wrap the promise in a union to allow control over the construction
  // simultaneously with annotating mutex requirements and noting that the
  // promise contained may not use any memory.
  union PromiseHolder {
    PromiseHolder() {}
    ~PromiseHolder() {}
    GPR_NO_UNIQUE_ADDRESS Promise promise;
  };
  GPR_NO_UNIQUE_ADDRESS PromiseHolder promise_holder_ ABSL_GUARDED_BY(mu());
};

}  // namespace promise_detail

// Given a functor that returns a promise (a promise factory), a callback for
// completion, and a callback scheduler, construct an activity.
template <typename Factory, typename WakeupScheduler, typename OnDone,
          typename... Contexts>
ActivityPtr MakeActivity(Factory promise_factory,
                         WakeupScheduler wakeup_scheduler, OnDone on_done,
                         Contexts&&... contexts) {
  return ActivityPtr(
      new promise_detail::PromiseActivity<Factory, WakeupScheduler, OnDone,
                                          Contexts...>(
          std::move(promise_factory), std::move(wakeup_scheduler),
          std::move(on_done), std::forward<Contexts>(contexts)...));
}

}  // namespace grpc_core

#endif  // GRPC_CORE_LIB_PROMISE_ACTIVITY_H
