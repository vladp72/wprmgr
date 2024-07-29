
#ifndef _AC_HELPERS_WIN32_LIBRARY_THREAD_POOL_HEADER_
#define _AC_HELPERS_WIN32_LIBRARY_THREAD_POOL_HEADER_

#pragma once

#include "accommon.h"
#include "acresourceowner.h"

namespace ac::tp {

    enum class callback_runs_long : bool { no = false, yes = true };

    enum class callback_persistent : bool { no = false, yes = true };

    using nano = std::ratio<1LL, 1'000'000'000LL>;
    using nanoseconds = std::chrono::duration<long long, nano>;

    using nano100 = std::ratio<1LL, 10'000'000LL>;
    using nanoseconds100 = std::chrono::duration<long long, nano100>;

    using micro = std::ratio<1LL, 1'000'000LL>;
    using microseconds = std::chrono::duration<long long, micro>;

    using mili = std::ratio<1LL, 1'000LL>;
    using miliseconds = std::chrono::duration<long long, mili>;

    using seconds = std::chrono::duration<long long, std::ratio<1LL, 1LL>>;

    using duration = std::chrono::system_clock::duration;
    using time_point = std::chrono::system_clock::time_point;
    using period = std::chrono::system_clock::period;

    extern __declspec(selectany) duration const infinite_duration = duration{-1};

    [[nodiscard]] inline std::chrono::duration<long long, std::ratio<1, 10000000>> operator"" _ns100(
        unsigned long long v) {
        return (std::chrono::duration<long long, std::ratio<1, 10000000>>(v));
    }

    [[nodiscard]] inline std::chrono::duration<double, std::ratio<1, 10000000>> operator"" _ns100(
        long double _Val) {
        return (std::chrono::duration<double, std::ratio<1, 10000000>>(_Val));
    }

    class cleanup_group;
    class thread_pool;
    class callback_instance;
    class work_item_base;
    class work_item;
    class timer_work_item;
    class wait_work_item;
    class io_handler;
    class safe_cleanup_group;

    typedef std::shared_ptr<thread_pool> thread_pool_ptr;
    typedef std::shared_ptr<cleanup_group> cleanup_group_ptr;
    typedef std::shared_ptr<work_item_base> work_item_base_ptr;
    typedef std::shared_ptr<work_item> work_item_ptr;
    typedef std::shared_ptr<timer_work_item> timer_work_item_ptr;
    typedef std::shared_ptr<wait_work_item> wait_work_item_ptr;
    typedef std::shared_ptr<io_handler> io_handler_ptr;

    typedef std::move_only_function<void(callback_instance &)> work_item_callback; // see help for the CreateThreadpoolWork
    typedef std::move_only_function<void(callback_instance &)> timer_work_item_callback; // see help for the CreateThreadpoolTimer
    typedef std::move_only_function<void(callback_instance &, TP_WAIT_RESULT)> wait_work_item_callback; // see help for the CreateThreadpoolWait
    typedef std::move_only_function<void(callback_instance &, OVERLAPPED *, ULONG, ULONG_PTR)> io_callback; // see help for the CreateThreadpoolIo

    struct optional_callback_parameters {
        std::optional<TP_CALLBACK_PRIORITY> priority;
        std::optional<callback_runs_long> runs_long;
        std::optional<void *> module;
    };

    namespace details {
        template<typename C>
        inline VOID CALLBACK submit_work_worker(PTP_CALLBACK_INSTANCE instance,
                                                PVOID context) noexcept {
            std::unique_ptr<C> cb{reinterpret_cast<C *>(context)};
            callback_instance inst{instance, nullptr};
            (*cb)(inst);
        }
    } // namespace details

    //
    // A helper class that should not be used directly.
    // The concept of environment is incapsulated inside the cancelation
    // group.
    //
    class callback_environment final {
    public:
        callback_environment() noexcept
            : environment_() {
            initialize();
        }

        callback_environment(callback_environment &) = delete;
        callback_environment(callback_environment &&) = delete;
        callback_environment &operator=(callback_environment &) = delete;
        callback_environment &operator=(callback_environment &&) = delete;

        ~callback_environment() noexcept {
            destroy();
        }

        void set_callback_runs_long() noexcept {
            SetThreadpoolCallbackRunsLong(&environment_);
        }

        void set_callback_persistent() noexcept {
            SetThreadpoolCallbackPersistent(&environment_);
        }

        void set_callback_priority(TP_CALLBACK_PRIORITY priority) noexcept {
            SetThreadpoolCallbackPriority(&environment_, priority);
        }

        void set_thread_pool(PTP_POOL threadPool = nullptr) noexcept {
            SetThreadpoolCallbackPool(&environment_, threadPool);
        }

        void set_library(void *module) noexcept {
            SetThreadpoolCallbackLibrary(&environment_, module);
        }

        void set_cleanup_group(PTP_CLEANUP_GROUP cleanup_group,
                               PTP_CLEANUP_GROUP_CANCEL_CALLBACK cancel_callback) noexcept {
            SetThreadpoolCallbackCleanupGroup(&environment_, cleanup_group, cancel_callback);
        }

        void set_callback_optional_parameters(optional_callback_parameters const *params) {
            if (params) {
                set_callback_optional_parameters(*params);
            }
        }

        void set_callback_optional_parameters(optional_callback_parameters const &params) {
            if (params.runs_long == callback_runs_long::yes) {
                set_callback_runs_long();
            }
            if (params.priority) {
                set_callback_priority(params.priority.value());
            }
            if (params.module) {
                set_library(params.module.value());
            }
        }

        [[nodiscard]] PTP_CALLBACK_ENVIRON get_handle() noexcept {
            return &environment_;
        }

        void initialize() noexcept {
            InitializeThreadpoolEnvironment(&environment_);
        }

        void destroy() noexcept {
            DestroyThreadpoolEnvironment(&environment_);
        }

        TP_CALLBACK_ENVIRON environment_;
    };

    class work_item_profiling {
    public:
        using profiling_clock = std::chrono::high_resolution_clock;
        using profiling_duration = profiling_clock::duration;
        using profiling_time_point = profiling_clock::time_point;

        [[nodiscard]] profiling_duration get_wait_duration() const noexcept {
            profiling_duration result;
            if (scheduled_time_.time_since_epoch().count() > 0) {
                if (started_time_.time_since_epoch().count() > 0) {
                    result = started_time_ - scheduled_time_;
                } else {
                    result = now() - scheduled_time_;
                }
            } else {
                result = profiling_duration{};
            }
            return result;
        }

        [[nodiscard]] profiling_duration get_run_duration() const noexcept {
            profiling_duration result;
            if (started_time_.time_since_epoch().count() > 0) {
                if (completed_time_.time_since_epoch().count() > 0) {
                    result = completed_time_ - started_time_;
                } else {
                    result = now() - started_time_;
                }
            } else {
                result = profiling_duration{};
            }
            return result;
        }

        [[nodiscard]] profiling_duration get_duration() const noexcept {
            profiling_duration result;
            if (scheduled_time_.time_since_epoch().count() > 0) {
                if (completed_time_.time_since_epoch().count() > 0) {
                    result = completed_time_ - scheduled_time_;
                } else {
                    result = now() - scheduled_time_;
                }
            } else {
                result = profiling_duration{};
            }
            return result;
        }

    protected:
        static profiling_time_point now() {
            return profiling_clock::now();
        }

        void update_scheduled_time() noexcept {
            scheduled_time_ = now();
            completed_time_ = profiling_time_point{};
            started_time_ = profiling_time_point{};
        }

        void update_started_time() noexcept {
            started_time_ = now();
        }

        void update_completed_time() noexcept {
            completed_time_ = now();
        }

    private:
        //
        // Time when work item is posted
        //
        profiling_time_point scheduled_time_{};
        //
        // time when work item was picked by a thread
        // and started executing
        //
        profiling_time_point started_time_{};
        //
        // time when workitem completed execution
        //
        profiling_time_point completed_time_{};
    };

    //
    // All work items are inherited from this base that contains some common facilities
    // like state machine, helper functions and a common interface declaration (join).
    //
    // This state machine is an unfortunate complication that we have to do to work around issue
    // with work item cancelation. The issue is that WaitForThreadPool* methods
    // tells us if work item was canceled or executed so we need to do our own tracking.
    // There are two possible scenarios:
    // - Someone creates a work item and does not hold a reference to the work item so
    // we have t do AddRef while work item is in the waiting state to make sure it stays
    // alive. Once work item was executed it will call move_to_completed and will release
    // reference if no one has done this yet. If work item belongs to a cancelation group
    // then calling cancelation_group::cancel will either cancel the work item before it was
    // executed and will move it to the closed state (in this case we will release reference
    // we've added when we've moved the work item to the waiting state) or wait for the work
    // item to complete execution. !!! Would it close the TPT_* structure in this case?
    // if yes then there might be a bug because according to MSDN cancel method of the
    // cancelation group is called only for work items that were canceled before execution
    // so no one will mark the other work items as closed !!!!
    // - Someone creates a work item and holds a reference to it planning to eventually call
    // join. This scenario similar to the one above except that now race conditions became
    // even more complicated because of the join. First of all it is responsibility of the
    // caller to make sure join and cancelation_group::cleanup do not race with each other.
    // Once join completes it calls move_to_ready and releases reference if the work item is still
    // in waiting state, which could happens only if work item was canceled before it ran.
    //
    // All this would be MUCH simpler if Wait* would tell us if work item actually ran or not.
    //
    class cleanup_group_member_itf {
    public:
        virtual ~cleanup_group_member_itf() noexcept {
        }

        virtual void on_closed() noexcept = 0;
    };

    class work_item_base
        : public cleanup_group_member_itf
        , public std::enable_shared_from_this<work_item_base>
        , public work_item_profiling {
    public:
        virtual void join() noexcept = 0;
        virtual void try_cancel_and_join() noexcept = 0;

        [[nodiscard]] bool is_closed() const noexcept {
            return state_ == closed;
        }

        [[nodiscard]] operator bool() const noexcept {
            return !is_closed();
        }

    private:
        typedef enum {
            closed    = 0x01,
            ready     = 0x02,
            posted    = 0x04,
        } state_t;

        state_t atomic_set_state(state_t new_state) noexcept {
            return state_.exchange(new_state);
        }

        state_t atomic_set_state_if_not_closed(state_t new_state) noexcept {
            state_t state{state_.load()};
            while (state != closed &&
                   !state_.compare_exchange_strong(state, new_state)) {
            }
            return state;
        }


    protected:
        work_item_base() = default;

        work_item_base(work_item_base &) = delete;
        work_item_base(work_item_base &&) = delete;
        work_item_base &operator=(work_item_base &) = delete;
        work_item_base &operator=(work_item_base &&) = delete;

        virtual ~work_item_base() noexcept {
            AC_CODDING_ERROR_IF(state_ == posted);
            AC_CODDING_ERROR_IF_NOT(nullptr == self_);
        }

        void move_to_posted() noexcept {
            state_t old_state = atomic_set_state(posted);
            AC_CODDING_ERROR_IF_NOT(old_state == ready);
            AC_CODDING_ERROR_IF_NOT(nullptr == self_);
            self_ = shared_from_this();
            update_scheduled_time();
        }

        work_item_base_ptr move_to_ready() noexcept {
            state_t old_state = atomic_set_state_if_not_closed(ready);
            if (old_state == posted || old_state == closed) {
                return std::move(self_);
            }
            return work_item_base_ptr();
        }

        void move_to_closed() noexcept {
            state_t old_state = atomic_set_state(closed);
            if (old_state == posted) {
                self_ = work_item_base_ptr();
            }
        }

        void join_complete() {
            move_to_ready();
        }

        [[nodiscard]] work_item_base_ptr start_running() noexcept {
            update_started_time();
            return move_to_ready();
        }

        void complete_running() noexcept {
            update_completed_time();
        }


        [[nodiscard]] bool is_posted() const noexcept {
            return state_ == posted;
        }

        //
        // Type traits for the ScopedResOwner to set/clear
        // current thread ID at the call back execution scope
        //
        class set_thread_id_traits final {
        public:
            static void acquire(DWORD *v) noexcept {
                *v = GetCurrentThreadId();
            }

            static void release(DWORD *v) noexcept {
                *v = 0;
            }
        };
        //
        // Type that will be declared on the
        // stack to store/clean thread id that
        // is currently executing the call-back
        //
        using scoped_thread_id_t = ac::resource_owner<DWORD, set_thread_id_traits>;

    private:
        //
        // Tracks current state of the call-back
        //
        std::atomic<state_t> state_{ready};
        //
        // While WI is in waiting state we keep a strong
        // reference to ourself
        //
        work_item_base_ptr self_;
    };

    //
    // This is just a helper class that is passed to each callback function and
    // provides an access to the call instance. Do not create instance of this
    // class on your own.
    //
    class callback_instance final {
    public:
        explicit callback_instance(PTP_CALLBACK_INSTANCE instance,
                                   work_item_base *parent_work_item) noexcept
            : instance_{instance}
            , parent_work_item_{parent_work_item} {
        }

        callback_instance(callback_instance const &) = delete;
        callback_instance &operator=(callback_instance const &) = delete;
        callback_instance(callback_instance const &&) = delete;
        callback_instance operator=(callback_instance const &&) = delete;

        void set_event_on_callback_return(HANDLE event) noexcept {
            SetEventWhenCallbackReturns(instance_, event);
        }

        void release_semaphore_on_callback_return(HANDLE semaphore, DWORD release_count) noexcept {
            ReleaseSemaphoreWhenCallbackReturns(instance_, semaphore, release_count);
        }

        void leave_criticak_section_on_callback_return(PCRITICAL_SECTION critical_section) noexcept {
            LeaveCriticalSectionWhenCallbackReturns(instance_, critical_section);
        }

        void release_mutex_on_callback_return(HANDLE mutex) noexcept {
            ReleaseMutexWhenCallbackReturns(instance_, mutex);
        }

        void free_library_on_callback_return(HMODULE module) noexcept {
            FreeLibraryWhenCallbackReturns(instance_, module);
        }

        [[nodiscard]] bool may_run_long() noexcept {
            return (CallbackMayRunLong(instance_) ? true : false);
        }

        [[nodiscard]] std::chrono::high_resolution_clock::duration get_wait_duration() const noexcept {
            if (parent_work_item_) {
                return parent_work_item_->get_wait_duration();
            }
            return std::chrono::high_resolution_clock::duration{};
        }

        [[nodiscard]] std::chrono::high_resolution_clock::duration get_run_duration() const noexcept {
            if (parent_work_item_) {
                return parent_work_item_->get_run_duration();
            }
            return std::chrono::high_resolution_clock::duration{};
        }

        [[nodiscard]] std::chrono::high_resolution_clock::duration get_duration() const noexcept {
            if (parent_work_item_) {
                return parent_work_item_->get_duration();
            }
            return std::chrono::high_resolution_clock::duration{};
        }

    private:
        PTP_CALLBACK_INSTANCE instance_;
        work_item_base *parent_work_item_;
    };

    //
    // Simple work item that is executed immediately after it is posted.
    // You can create and use this class directly. You might consider using
    // this class directly if you want to preallocate resources to make sure
    // that post operation would not fail.
    // For more details see WokItemBase documentation above.
    //
    class work_item final: public work_item_base {
    protected:
    public:
        template<typename C>
        explicit work_item(C &&callback, callback_environment *environment = nullptr)
            : callback_(std::forward<C>(callback))
            , work_(nullptr)
            , callback_thread_id_(0) {
            work_ = CreateThreadpoolWork(&work_item::run_callback,
                                         this,
                                         environment ? environment->get_handle() : nullptr);

            if (nullptr == work_) {
                AC_THROW(GetLastError(), "CreateThreadpoolWork");
            }
        }

        ~work_item() noexcept {
            if (!is_closed()) {
                CloseThreadpoolWork(work_);
            }
        }

        template<typename C>
        [[nodiscard]] static work_item_ptr make(C &&callback,
                                                callback_environment *environment = nullptr) {
            return std::make_shared<work_item>(std::forward<C>(callback), environment);
        }

        template<typename C>
        [[nodiscard]] static work_item_ptr make(C &&callback,
                                                optional_callback_parameters const *params) {
            callback_environment environment;
            environment.set_callback_optional_parameters(params);
            return std::make_shared<work_item>(std::forward<C>(callback), environment);
        }

        void post() noexcept {
            move_to_posted();
            SubmitThreadpoolWork(work_);
        }

        void join() noexcept override {
            //
            // If we ever try to do join from the thread that
            // is running a call back then we will deadlock
            //
            AC_CODDING_ERROR_IF(is_current_thread_executing_callback());

            if (!is_closed()) {
                WaitForThreadpoolWorkCallbacks(work_, FALSE);

                join_complete();
            }
        }

        void try_cancel_and_join() noexcept override {
            //
            // If we ever try to do join from the thread that
            // is running a call back then we will deadlock
            //
            AC_CODDING_ERROR_IF(is_current_thread_executing_callback());

            if (!is_closed()) {
                WaitForThreadpoolWorkCallbacks(work_, TRUE);

                join_complete();
            }
        }

        [[nodiscard]] bool is_current_thread_executing_callback() const noexcept {
            return (GetCurrentThreadId() == callback_thread_id_);
        }

        [[nodiscard]] DWORD get_worker_thread_id() const noexcept {
            return callback_thread_id_;
        }

    private:
        void on_closed() noexcept override {
            move_to_closed();
        }

        static void CALLBACK run_callback(PTP_CALLBACK_INSTANCE instance,
                                          void *context,
                                          PTP_WORK work) noexcept {
            work_item *work_item_raw = static_cast<work_item *>(context);
            AC_CODDING_ERROR_IF_NOT(work_item_raw->work_ == work);
            work_item_raw->run(instance);
        }

        void run(PTP_CALLBACK_INSTANCE instance) noexcept {
            work_item_base_ptr self = start_running();

            AC_CODDING_ERROR_IF_NOT(self);

            callback_instance inst{instance, this};
            {
                //
                // Store the thread Id of the trhead that is
                // executing the call-back
                //
                scoped_thread_id_t store_executing_thread_id(&callback_thread_id_);

                callback_(inst);
                complete_running();
            }
        }
        //
        // Delegate that should be called when work
        // item got executed
        //
        work_item_callback callback_;
        //
        // Pointer to the thread pool descriptor
        // of the work item
        //
        PTP_WORK work_;
        //
        // When the call back is called it sets this
        // variable to the address of the current
        // thread so later of this thread can check if
        // it is a call-back and avoid calling wait
        // from inside the call-back. We also will use
        // this filed to assert in the cases where we
        // do call wait from inside the wait.
        //
        DWORD callback_thread_id_;
    };

    //
    // Work item that is getting executed on timer. Please note that helper
    // does not support periodic timer, but you can easily achieve the same
    // result by simply having the work item to reschedule itself. You can
    // create and use this class directly. You might consider using this class
    // directly if you want to preallocate resources to make sure that post
    // operation would not fail or if you want to be able reset or cancel the
    // work item. For more details see WokItemBase documentation above.
    //
    class timer_work_item final: public work_item_base {
    public:
        template<typename C>
        explicit timer_work_item(C &&callback, callback_environment *environment = nullptr)
            : callback_(std::forward<C>(callback))
            , timer_(nullptr)
            , callback_thread_id_(0) {
            timer_ = CreateThreadpoolTimer(&timer_work_item::run_callback,
                                           this,
                                           environment ? environment->get_handle() : nullptr);

            if (nullptr == timer_) {
                AC_THROW(GetLastError(), "CreateThreadpoolTimer");
            }
        }

        ~timer_work_item() noexcept {
            if (!is_closed()) {
                CloseThreadpoolTimer(timer_);
            }
        }

        template<typename C>
        [[nodiscard]] static timer_work_item_ptr make(C &&callback,
                                                      callback_environment *environment = nullptr) {
            return std::make_shared<timer_work_item>(std::forward<C>(callback), environment);
        }

        template<typename C>
        [[nodiscard]] static timer_work_item_ptr make(C &&callback,
                                                      optional_callback_parameters const *params) {
            callback_environment environment;
            environment.set_callback_optional_parameters(params);
            return std::make_shared<timer_work_item>(std::forward<C>(callback), environment);
        }

        [[nodiscard]] bool is_scheduled() noexcept {
            //
            // Once we set timer and the callback was called it still stays in
            // the set state. We want a different sematic because this is a
            // one-shot timer. In our case timer should become unset as soon as
            // callback has completed execution or was canceled.
            //
            return (IsThreadpoolTimerSet(timer_) && is_posted() ? true : false);
        }

        void schedule(duration const &due_time, DWORD window_length = 0) noexcept {
            AC_CODDING_ERROR_IF(callback_ == nullptr);

            ULARGE_INTEGER ulDueTime;
            FILETIME ftDueTime;
            ulDueTime.QuadPart = -due_time.count();
            ftDueTime.dwHighDateTime = ulDueTime.HighPart;
            ftDueTime.dwLowDateTime = ulDueTime.LowPart;

            move_to_posted();

            SetThreadpoolTimer(timer_, &ftDueTime, 0, window_length);
        }

        void schedule(time_point const &due_time, DWORD window_length = 0) noexcept {
            AC_CODDING_ERROR_IF(callback_ == nullptr);

            FILETIME ft_due_time{system_clock_time_point_to_filetime(due_time)};

            move_to_posted();

            SetThreadpoolTimer(timer_, &ft_due_time, 0, window_length);
        }

        void join() noexcept override {
            if (!is_closed()) {
                //
                // If we ever try to do join from the thread that
                // is running a call back then we will deadlock
                //
                AC_CODDING_ERROR_IF(is_current_thread_executing_callback());

                WaitForThreadpoolTimerCallbacks(timer_, FALSE);

                join_complete();
            }
        }

        void try_cancel_and_join() noexcept override {
            if (!is_closed()) {
                //
                // If we ever try to do join from the thread that
                // is running a call back then we will deadlock
                //
                AC_CODDING_ERROR_IF(is_current_thread_executing_callback());

                SetThreadpoolTimer(timer_, nullptr, 0, 0);

                WaitForThreadpoolTimerCallbacks(timer_, TRUE);

                join_complete();
            }
        }

        [[nodiscard]] bool is_current_thread_executing_callback() const {
            return (GetCurrentThreadId() == callback_thread_id_);
        }

        [[nodiscard]] DWORD get_worker_thread_id() const {
            return callback_thread_id_;
        }

    private:
        void on_closed() noexcept override {
            move_to_closed();
        }

        static void CALLBACK run_callback(PTP_CALLBACK_INSTANCE instance,
                                          void *context,
                                          PTP_TIMER timer) noexcept {
            timer_work_item *work_item = static_cast<timer_work_item *>(context);
            AC_CODDING_ERROR_IF_NOT(work_item->timer_ == timer);
            work_item->run(instance);
        }

        void run(PTP_CALLBACK_INSTANCE instance) noexcept {
            work_item_base_ptr self = start_running();

            callback_instance inst{instance, this};
            {
                //
                // Store the thread Id of the trhead that is
                // executing the call-back
                //
                scoped_thread_id_t store_executing_thread_id(&callback_thread_id_);

                callback_(inst);
                complete_running();
            }
        }

        //
        // Delegate that should be called when work
        // item got executed
        //
        timer_work_item_callback callback_;
        //
        // Pointer to the thread pool descriptor
        // of the timer work item
        //
        PTP_TIMER timer_;
        //
        // When the call back is called it sets this
        // variable to the address of the current
        // thread so later of this thread can check if
        // it is a call-back and avoid calling wait
        // from inside the call-back. We also will use
        // this filed to assert in the cases where we
        // do call wait from inside the wait.
        //
        DWORD callback_thread_id_;
    };

    //
    // Work item that is getting executed when object becomes signaled.
    // You can create and use this class directly. You might consider using
    // this class directly if you want to preallocate resources to make sure
    // that post operation would not fail or if you want to be able reset or
    // cancel the work item.
    // For more details see WokItemBase documentation above.
    //
    class wait_work_item final: public work_item_base {
    public:
        template<typename C>
        explicit wait_work_item(C &&callback, callback_environment *environment = nullptr)
            : callback_(std::forward<C>(callback))
            , wait_(nullptr)
            , callback_thread_id_(0) {
            wait_ = CreateThreadpoolWait(&wait_work_item::run_callback,
                                         this,
                                         environment ? environment->get_handle() : nullptr);

            if (nullptr == wait_) {
                AC_THROW(GetLastError(), "CreateThreadpoolWait");
            }
        }

        ~wait_work_item() noexcept {
            if (!is_closed()) {
                CloseThreadpoolWait(wait_);
            }
        }

        template<typename C>
        [[nodiscard]] static wait_work_item_ptr make(C &&callback,
                                                     callback_environment *environment = nullptr) {
            return std::make_shared<wait_work_item>(std::forward<C>(callback), environment);
        }

        template<typename C>
        [[nodiscard]] static wait_work_item_ptr make(C &&callback,
                                                     optional_callback_parameters const *params) {
            callback_environment environment;
            environment.set_callback_optional_parameters(params);
            return std::make_shared<wait_work_item>(std::forward<C>(callback), environment);
        }

        void schedule_wait(HANDLE handle, duration const &due_time = infinite_duration) noexcept {

            move_to_posted();

            if (due_time == infinite_duration) {
                SetThreadpoolWait(wait_, handle, nullptr);
            } else {
                ULARGE_INTEGER ulDueTime;
                FILETIME ftDueTime;
                ulDueTime.QuadPart = -due_time.count();
                ftDueTime.dwHighDateTime = ulDueTime.HighPart;
                ftDueTime.dwLowDateTime = ulDueTime.LowPart;

                SetThreadpoolWait(wait_, handle, &ftDueTime);
            }
        }

        void schedule_wait(HANDLE handle, time_point const &due_time) noexcept {
            FILETIME ft_due_time{system_clock_time_point_to_filetime(due_time)};

            move_to_posted();

            SetThreadpoolWait(wait_, handle, &ft_due_time);
        }

        void join() noexcept override {
            if (!is_closed()) {
                //
                // If we ever try to do join from the thread that
                // is running a call back then we will deadlock
                //
                AC_CODDING_ERROR_IF(is_current_thread_executing_callback());

                WaitForThreadpoolWaitCallbacks(wait_, FALSE);

                join_complete();
            }
        }

        void try_cancel_and_join() noexcept override {
            if (!is_closed()) {
                //
                // If we ever try to do join from the thread that
                // is running a call back then we will deadlock
                //
                AC_CODDING_ERROR_IF(is_current_thread_executing_callback());

                SetThreadpoolWait(wait_, nullptr, nullptr);

                WaitForThreadpoolWaitCallbacks(wait_, TRUE);

                join_complete();
            }
        }

        bool is_current_thread_executing_callback() const {
            return (GetCurrentThreadId() == callback_thread_id_);
        }

        [[nodiscard]] DWORD get_worker_thread_id() const {
            return callback_thread_id_;
        }

    private:
        void on_closed() noexcept override {
            move_to_closed();
        }

        static void CALLBACK run_callback(PTP_CALLBACK_INSTANCE instance,
                                          void *context,
                                          PTP_WAIT wait,
                                          TP_WAIT_RESULT wait_result) noexcept {
            wait_work_item *work_item = static_cast<wait_work_item *>(context);
            AC_CODDING_ERROR_IF_NOT(work_item->wait_ == wait);
            work_item->run(instance, wait_result);
        }

        void run(PTP_CALLBACK_INSTANCE instance, TP_WAIT_RESULT wait_result) noexcept {
            work_item_base_ptr self = start_running();

            callback_instance inst{instance, this};
            {
                //
                // Store the thread Id of the thread that is
                // executing the call-back
                //
                scoped_thread_id_t store_executing_thread_id(&callback_thread_id_);

                callback_(inst, wait_result);
                complete_running();
            }
        }

        //
        // Delegate that should be called when work
        // item got executed
        //
        wait_work_item_callback callback_;
        //
        // Pointer to the thread pool descriptor
        // of the timer work item
        //
        PTP_WAIT wait_;
        //
        // When the call back is called it sets this
        // variable to the address of the current
        // thread so later of this thread can check if
        // it is a call-back and avoid calling wait
        // from inside the call-back. We also will use
        // this filed to assert in the cases where we
        // do call wait from inside the wait.
        //
        DWORD callback_thread_id_;
    };

    class io_guard final {
    public:
        io_guard() {
        }

        explicit io_guard(io_handler *handler) noexcept;

        explicit io_guard(io_handler_ptr const &handler) noexcept;

        io_guard(io_guard const &) = delete;
        io_guard &operator=(io_guard const &) = delete;

        io_guard(io_guard &&other) noexcept
            : handler_(other.handler_) {
            other.handler_ = nullptr;
        }

        io_guard &operator=(io_guard &&other) noexcept {
            if (&other != this) {
                handler_ = other.handler_;
                other.handler_ = nullptr;
            }
            return *this;
        }

        ~io_guard() noexcept;

        void failed_start_io() noexcept;

        [[nodiscard]] bool is_armed() const noexcept {
            return nullptr != handler_;
        }

        operator bool() const noexcept {
            return is_armed();
        }

        void disarm() noexcept {
            handler_ = nullptr;
        }

    private:
        io_handler *handler_{nullptr};
    };

    //
    // This method provides access to the thread pool's completion port
    // Pleaser note that it does not use work_item_base state machine to
    // ensure lifetime. The assumption is that program must first ensure
    // that all ongoing IO is completed before going and destroying this
    // class.
    //
    class io_handler final: public cleanup_group_member_itf {
        friend class io_guard;

    public:
        template<typename C>
        explicit io_handler(HANDLE handle, C &&callback, callback_environment *environment = nullptr)
            : callback_(std::forward<C>(callback))
            , io_(nullptr) {
            io_ = CreateThreadpoolIo(handle,
                                     &io_handler::run_callback,
                                     this,
                                     environment ? environment->get_handle() : nullptr);

            if (nullptr == io_) {
                AC_THROW(GetLastError(), "CreateThreadpoolIo");
            }
        }

        ~io_handler() noexcept {
            if (!is_closed()) {
                //
                // if this object was not through cancelation group then make
                // sure that all started IOs are complete.
                //
                WaitForThreadpoolIoCallbacks(io_, FALSE);
                CloseThreadpoolIo(io_);
            }
        }

        template<typename C>
        [[nodiscard]] static io_handler_ptr make(HANDLE handle,
                                                 C &&callback,
                                                 callback_environment *environment = nullptr) {
            return std::make_shared<io_handler>(handle, std::forward<C>(callback), environment);
        }

        template<typename C>
        [[nodiscard]] static io_handler_ptr make(HANDLE handle,
                                                 C &&callback,
                                                 optional_callback_parameters const *params) {
            callback_environment environment;
            environment.set_callback_optional_parameters(params);
            return std::make_shared<io_handler>(handle, std::forward<C>(callback), environment);
        }

        //
        // Call this method every time before issuing an assync IO.
        //
        [[nodiscard]] io_guard start_io() noexcept {
            return io_guard{this};
        }

        //
        // According to MSDN you MUST call this method whenever
        // IO operation has completed synchronously with an error
        // code other than indicating that operation is pending.
        // Whenever possible prefer using io_guard instead of calling
        // this method directly.
        //
        void failed_start_io() noexcept {
            AC_CODDING_ERROR_IF(is_closed());

            CancelThreadpoolIo(io_);
        }

        void join() noexcept {
            if (!is_closed()) {
                WaitForThreadpoolIoCallbacks(io_, FALSE);
            }
        }

        [[nodiscard]] bool is_closed() const noexcept {
            return state_ == closed;
        }

        [[nodiscard]] operator bool() const noexcept {
            return !is_closed();
        }

    private:
        typedef enum {
            initialized,
            closed,
        } state_t;

        state_t atomic_set_state(state_t new_state) noexcept {
            return state_.exchange(new_state);
        }

        void move_to_closed() noexcept {
            state_t old_state = atomic_set_state(closed);
        }

        void internal_start_io() noexcept {
            AC_CODDING_ERROR_IF(is_closed());

            StartThreadpoolIo(io_);
        }

        void on_closed() noexcept override {
            move_to_closed();
        }

        static void CALLBACK run_callback(PTP_CALLBACK_INSTANCE instance,
                                          void *context,
                                          void *overlapped,
                                          ULONG result,
                                          ULONG_PTR bytes_transferred,
                                          PTP_IO io) noexcept {
            io_handler *handler = static_cast<io_handler *>(context);
            AC_CODDING_ERROR_IF_NOT(handler->io_ == io);
            handler->run(instance, static_cast<OVERLAPPED *>(overlapped), result, bytes_transferred);
        }

        void run(PTP_CALLBACK_INSTANCE instance,
                 OVERLAPPED *overlapped,
                 ULONG result,
                 ULONG_PTR bytes_transferred) noexcept {
            //
            // Please note that for The IO work item we cannot track
            // the thread that is currently executing the IO because
            // multiple threads might be executing the IO at the same
            // time using the same work item.
            //
            // scoped_thread_id_t storeExecutingThreadId(this);
            //

            callback_instance inst{instance, nullptr};
            AC_CODDING_ERROR_IF(callback_ == nullptr);
            callback_(inst, overlapped, result, bytes_transferred);
        }

        std::atomic<state_t> state_{state_t::initialized};
        io_callback callback_;
        PTP_IO io_;
    };

    inline io_guard::io_guard(io_handler *handler) noexcept
        : handler_(handler) {
        handler_->internal_start_io();
    }

    inline io_guard::io_guard(io_handler_ptr const &handler) noexcept
        : handler_(handler.get()) {
        handler_->internal_start_io();
    }

    inline io_guard ::~io_guard() noexcept {
        if (handler_) {
            handler_->failed_start_io();
        }
    }

    inline void io_guard::failed_start_io() noexcept {
        handler_->failed_start_io();
        handler_ = nullptr;
    }

    class thread_pool final: public std::enable_shared_from_this<thread_pool> {
    public:
        explicit thread_pool(unsigned long max_threads = ULONG_MAX,
                             unsigned long min_threads = ULONG_MAX,
                             PTP_POOL_STACK_INFORMATION stack_information = nullptr)
            : pool_(nullptr) {
            pool_ = CreateThreadpool(nullptr);

            if (nullptr == pool_) {
                AC_THROW(GetLastError(), "CreateThreadPool");
            }

            if (ULONG_MAX != max_threads) {
                set_max_thread_count(max_threads);
            }

            if (ULONG_MAX != min_threads) {
                set_min_thread_count(min_threads);
            }

            if (stack_information) {
                set_stack_information(stack_information);
            }
        }

        thread_pool(thread_pool &) = delete;
        thread_pool(thread_pool &&) = delete;
        thread_pool &operator=(thread_pool &) = delete;
        thread_pool &operator=(thread_pool &&) = delete;

        ~thread_pool() noexcept {
            CloseThreadpool(pool_);
        }

        [[nodiscard]] PTP_POOL get_handle() noexcept {
            return pool_;
        }

        static [[nodiscard]] thread_pool_ptr make(unsigned long max_threads = ULONG_MAX,
                                                  unsigned long min_threads = ULONG_MAX,
                                                  PTP_POOL_STACK_INFORMATION stack_information = nullptr) {
            return std::make_shared<thread_pool>(max_threads, min_threads, stack_information);
        }

        [[nodiscard]] cleanup_group_ptr make_cleanup_group();

        template<typename C>
        [[nodiscard]] work_item_ptr make_work_item(
            C &&callback, optional_callback_parameters const *params = nullptr) {
            callback_environment environment;
            environment.set_thread_pool(pool_);
            environment.set_callback_optional_parameters(params);

            return work_item::make(std::forward<C>(callback), &environment);
        }

        template<typename C>
        [[nodiscard]] timer_work_item_ptr make_timer_work_item(
            C &&callback, optional_callback_parameters const *params = nullptr) {
            callback_environment environment;
            environment.set_thread_pool(pool_);
            environment.set_callback_optional_parameters(params);

            return timer_work_item::make(std::forward<C>(callback), &environment);
        }

        template<typename C>
        [[nodiscard]] wait_work_item_ptr make_wait_work_item(
            C &&callback, optional_callback_parameters const *params = nullptr) {
            callback_environment environment;
            environment.set_thread_pool(pool_);
            environment.set_callback_optional_parameters(params);

            return wait_work_item::make(std::forward<C>(callback), &environment);
        }

        template<typename C>
        [[nodiscard]] io_handler_ptr make_io_handler(
            HANDLE handle, C &&callback, optional_callback_parameters const *params = nullptr) {
            callback_environment environment;
            environment.set_thread_pool(pool_);
            environment.set_callback_optional_parameters(params);

            return io_handler::make(handle, std::forward<C>(callback), &environment);
        }

        template<typename C>
        inline void submit_work(C &&callback) {
            std::unique_ptr<C> cb{new C{std::forward<C>(callback)}};
            if (TrySubmitThreadpoolCallback(
                    &details::submit_work_worker<C>, cb.get(), nullptr)) {
                cb.release();
            } else {
                AC_THROW(GetLastError(), "TrySubmitThreadpoolCallback");
            }
            return;
        }

        template<typename C>
        void submit_work(C &&callback, optional_callback_parameters const *params) {
            std::unique_ptr<C> cb{new C{std::forward<C>(callback)}};
            callback_environment environment;
            environment.set_thread_pool(pool_);
            environment.set_callback_optional_parameters(params);
            if (TrySubmitThreadpoolCallback(
                    &details::submit_work_worker<C>, cb.get(), environment.get_handle())) {
                cb.release();
            } else {
                AC_THROW(GetLastError(), "TrySubmitThreadpoolCallback");
            }
            return;
        }

        template<typename C>
        work_item_ptr post(C &&callback, optional_callback_parameters const *params = nullptr) {
            work_item_ptr work_item{make_work_item(std::forward<C>(callback), params)};
            work_item->post();
            return work_item;
        }

        template<typename C>
        timer_work_item_ptr schedule(C &&callback,
                                     time_point const &due_time,
                                     DWORD window_length = 0,
                                     optional_callback_parameters const *params = nullptr) {
            timer_work_item_ptr timer_work_item{
                make_timer_work_item(std::forward<C>(callback), params)};
            timer_work_item->schedule(due_time, window_length);
            return timer_work_item;
        }

        template<typename C>
        timer_work_item_ptr schedule(C &&callback,
                                     duration const &due_time,
                                     DWORD window_length = 0,
                                     optional_callback_parameters const *params = nullptr) {
            timer_work_item_ptr timer_work_item{
                make_timer_work_item(std::forward<C>(callback), params)};
            timer_work_item->schedule(due_time, window_length);
            return timer_work_item;
        }

        template<typename C>
        wait_work_item_ptr schedule_wait(C &&callback,
                                         HANDLE handle,
                                         duration const &due_time = infinite_duration,
                                         optional_callback_parameters const *params = nullptr) {
            wait_work_item_ptr wait_work_item{
                make_wait_work_item(std::forward<C>(callback), params)};
            wait_work_item->schedule_wait(handle, due_time);
            return wait_work_item;
        }

        template<typename C>
        wait_work_item_ptr schedule_wait(C &&callback,
                                         HANDLE handle,
                                         time_point const &due_time,
                                         optional_callback_parameters const *params = nullptr) {
            wait_work_item_ptr wait_work_item{
                make_wait_work_item(std::forward<C>(callback), params)};
            wait_work_item->schedule_wait(handle, due_time);
            return wait_work_item;
        }

        void get_stack_information(PTP_POOL_STACK_INFORMATION stack_information) noexcept {
            QueryThreadpoolStackInformation(pool_, stack_information);
        }

    private:
        void set_stack_information(PTP_POOL_STACK_INFORMATION stack_information) noexcept {
            SetThreadpoolStackInformation(pool_, stack_information);
        }

        void set_max_thread_count(unsigned long max_threads) noexcept {
            SetThreadpoolThreadMaximum(pool_, max_threads);
        }

        void set_min_thread_count(unsigned long min_threads) noexcept {
            SetThreadpoolThreadMinimum(pool_, min_threads);
        }

        PTP_POOL pool_;
    };

    //
    // This class is a wrapper around thread pool cleanup group
    //
    // Calling *join on the group calls cleanup_group::on_closed for every TP_*
    // object associated with this cancelation group. For more details see
    // cleanup_group::on_closed calls virtual function <object>::on_closed on the RAII wrapper
    // that owns object. The <object>::on_closed drops reference on itself that it is holding while
    // object is posted or running, and moves itself to closed state. Object destructor checks if
    // object was moved to closed state then it knows that TP_* object is already destroyed.
    // After object is moved to closed state it must not be used.
    // https://learn.microsoft.com/en-us/windows/win32/api/threadpoolapiset/nf-threadpoolapiset-closethreadpoolcleanupgroupmembers
    // App must be careful to serialize associating new objects with thread pool and calling *join.
    //
    // Consider using safe_cleanup_group instead of cleanup_group. It prevents
    // race described in the above article by destroying cleanup group that is being joined.
    // Attempts to create new objects on joined cleanup group will fail.
    // Caller is responsible to call reinitialize to create a new cleanup group, only after
    // that he will be able to create new objects.
    //
    class cleanup_group final: public std::enable_shared_from_this<cleanup_group> {
    public:
        [[nodiscard]] static cleanup_group_ptr make(thread_pool_ptr thread_pool = thread_pool_ptr{}) {
            return std::make_shared<cleanup_group>(std::move(thread_pool));
        }

        explicit cleanup_group(thread_pool_ptr const &thread_pool = thread_pool_ptr{})
            : group_(nullptr) {
            create(thread_pool);
        }

        cleanup_group(cleanup_group &) = delete;
        cleanup_group(cleanup_group &&) = delete;
        cleanup_group &operator=(cleanup_group &) = delete;
        cleanup_group &operator=(cleanup_group &&) = delete;

        ~cleanup_group() noexcept {
            try_cancel_and_join();

            CloseThreadpoolCleanupGroup(group_);
        }

        void join() noexcept {
            //
            // Passing FALSE as a second parameter does not work as expected
            // For instance it will not wait for timers and waits to get completed
            // but instead just cancels them so there is no "true" join
            //
            CloseThreadpoolCleanupGroupMembers(group_, FALSE, this);
        }

        void try_cancel_and_join() noexcept {
            //
            // Passing FALSE as a second parameter does not work as expected
            // For instance it will not wait for timers and waits to get completed
            // but instead just cancels them so there is no "true" join
            //
            CloseThreadpoolCleanupGroupMembers(group_, TRUE, this);
        }

        template<typename C>
        [[nodiscard]] work_item_ptr make_work_item(
            C &&callback, optional_callback_parameters const *params = nullptr) {
            callback_environment environment;
            environment.set_cleanup_group(group_, &cleanup_group::on_closed_callback);
            if (thread_pool_) {
                environment.set_thread_pool(thread_pool_->get_handle());
            }
            environment.set_callback_optional_parameters(params);

            return work_item::make(std::forward<C>(callback), &environment);
        }

        template<typename C>
        [[nodiscard]] timer_work_item_ptr make_timer_work_item(
            C &&callback, optional_callback_parameters const *params = nullptr) {
            callback_environment environment;
            environment.set_cleanup_group(group_, &cleanup_group::on_closed_callback);
            if (thread_pool_) {
                environment.set_thread_pool(thread_pool_->get_handle());
            }
            environment.set_callback_optional_parameters(params);

            return timer_work_item::make(std::forward<C>(callback), &environment);
        }

        template<typename C>
        [[nodiscard]] wait_work_item_ptr make_wait_work_item(
            C &&callback, optional_callback_parameters const *params = nullptr) {
            callback_environment environment;
            environment.set_cleanup_group(group_, &cleanup_group::on_closed_callback);
            if (thread_pool_) {
                environment.set_thread_pool(thread_pool_->get_handle());
            }
            environment.set_callback_optional_parameters(params);

            return wait_work_item::make(std::forward<C>(callback), &environment);
        }

        template<typename C>
        [[nodiscard]] io_handler_ptr make_io_handler(
            HANDLE handle, C &&callback, optional_callback_parameters const *params = nullptr) {
            callback_environment environment;
            environment.set_cleanup_group(group_, &cleanup_group::on_closed_callback);
            if (thread_pool_) {
                environment.set_thread_pool(thread_pool_->get_handle());
            }
            environment.set_callback_optional_parameters(params);

            return io_handler::make(handle, std::forward<C>(callback), &environment);
        }

        template<typename C>
        inline void submit_work(C &&callback) {
            std::unique_ptr<C> cb{new C{std::forward<C>(callback)}};
            if (TrySubmitThreadpoolCallback(
                    &details::submit_work_worker<C>, cb.get(), nullptr)) {
                cb.release();
            } else {
                AC_THROW(GetLastError(), "TrySubmitThreadpoolCallback");
            }
            return;
        }

        template<typename C>
        inline void submit_work(C &&callback, optional_callback_parameters const *params) {
            std::unique_ptr<C> cb{new C{std::forward<C>(callback)}};
            callback_environment environment;
            environment.set_cleanup_group(group_, &cleanup_group::on_closed_callback);
            if (thread_pool_) {
                environment.set_thread_pool(thread_pool_->get_handle());
            }
            environment.set_callback_optional_parameters(params);
            if (TrySubmitThreadpoolCallback(
                    &details::submit_work_worker<C>, cb.get(), environment.get_handle())) {
                cb.release();
            } else {
                AC_THROW(GetLastError(), "TrySubmitThreadpoolCallback");
            }
            return;
        }

        template<typename C>
        work_item_ptr post(C &&callback, optional_callback_parameters const *params = nullptr) {
            work_item_ptr work_item{make_work_item(std::forward<C>(callback), params)};
            work_item->post();
            return work_item;
        }

        template<typename C>
        timer_work_item_ptr schedule(C &&callback,
                                     time_point const &due_time,
                                     DWORD window_length = 0,
                                     optional_callback_parameters const *params = nullptr) {
            timer_work_item_ptr timer_work_item{
                make_timer_work_item(std::forward<C>(callback), params)};
            timer_work_item->schedule(due_time, window_length);
            return timer_work_item;
        }

        template<typename C>
        timer_work_item_ptr schedule(C &&callback,
                                     duration const &due_time,
                                     DWORD window_length = 0,
                                     optional_callback_parameters const *params = nullptr) {
            timer_work_item_ptr timer_work_item{
                make_timer_work_item(std::forward<C>(callback), params)};
            timer_work_item->schedule(due_time, window_length);
            return timer_work_item;
        }

        template<typename C>
        wait_work_item_ptr schedule_wait(C &&callback,
                                         HANDLE handle,
                                         duration const &due_time = infinite_duration,
                                         optional_callback_parameters const *params = nullptr) {
            wait_work_item_ptr wait_work_item{
                make_wait_work_item(std::forward<C>(callback), params)};
            wait_work_item->schedule_wait(handle, due_time);
            return wait_work_item;
        }

        template<typename C>
        wait_work_item_ptr schedule_wait(C &&callback,
                                         HANDLE handle,
                                         time_point const &due_time,
                                         optional_callback_parameters const *params = nullptr) {
            wait_work_item_ptr wait_work_item{
                make_timer_work_item(std::forward<C>(callback), params)};
            wait_work_item->schedule(handle, due_time);
            return wait_work_item;
        }

    private:
        [[nodiscard]] PTP_CLEANUP_GROUP get_handle() noexcept {
            return group_;
        }

        void create(thread_pool_ptr thread_pool) {
            group_ = CreateThreadpoolCleanupGroup();

            if (nullptr == group_) {
                AC_THROW(GetLastError(), "CreateThreadpoolCleanupGroup");
            }

            if (nullptr != thread_pool) {
                thread_pool_ = std::move(thread_pool);
            }
        }

        void on_closed(void *object_context) noexcept {
            cleanup_group_member_itf *member =
                static_cast<cleanup_group_member_itf *>(object_context);
            member->on_closed();
        }

        static void NTAPI on_closed_callback(void *object_context, void *cleanup_context) noexcept {
            cleanup_group *self = static_cast<cleanup_group *>(cleanup_context);
            self->on_closed(object_context);
        }

        PTP_CLEANUP_GROUP group_;
        callback_environment environment_;
        //
        // if cleanup group was associated with a thread pool then keep it alive
        // until cleanup group is destroyed.
        //
        thread_pool_ptr thread_pool_;
    };

    //
    // cleanup_group will wait for ongoing work to complete, but it does not prevent
    // new work from getting started.
    // safe_cleanup_group solves this by tracking if rundown has already started,
    // and preventing new work from getting scheduled until cleanup_group is
    // reinitialized again.
    // When reinitialize called on already initialized object it will implicitly
    // try_cancel_and_join, but only after new group was created so you might see new work
    // items start using new group while old work is still running down. To avoid that
    // call join explicitly before calling reinitialize.
    // All post and schedule methods have Try prefix pointing out that they might fail
    // when rundown started. Check result for nullptr to see if post succeeded or failed.
    //
    class safe_cleanup_group final {
    public:
        safe_cleanup_group()
            : group_(nullptr) {
            reinitialize(thread_pool_ptr{});
        }

        explicit safe_cleanup_group(thread_pool_ptr thread_pool)
            : group_(nullptr) {
            reinitialize(std::move(thread_pool));
        }

        safe_cleanup_group(safe_cleanup_group &) = delete;
        safe_cleanup_group(safe_cleanup_group &&) = delete;
        safe_cleanup_group &operator=(safe_cleanup_group &) = delete;
        safe_cleanup_group &operator=(safe_cleanup_group &&) = delete;

        ~safe_cleanup_group() noexcept {
            try_cancel_and_join();
        }

        void reinitialize(thread_pool_ptr thread_pool = thread_pool_ptr{}) {
            cleanup_group_ptr group;
            {
                auto grab{acquire_resource_exclusive(lock_)};
                group = std::move(group_);
                group_ = cleanup_group::make(std::move(thread_pool));
            }
        }

        [[nodiscard]] bool is_valid() const noexcept {
            auto grab{acquire_resource_shared(lock_)};
            return nullptr != group_;
        }

        [[nodiscard]] explicit operator bool() const noexcept {
            return is_valid();
        }

        [[nodiscard]] cleanup_group_ptr try_get_cleanup_group() noexcept {
            auto grab{acquire_resource_shared(lock_)};
            return group_;
        }

        void join() noexcept {
            cleanup_group_ptr group;
            {
                auto grab{acquire_resource_exclusive(lock_)};
                group = std::move(group_);
            }
            if (group != nullptr) {
                group->join();
            }
        }

        void try_cancel_and_join() noexcept {
            cleanup_group_ptr group;
            {
                auto grab{acquire_resource_exclusive(lock_)};
                group = std::move(group_);
            }
            if (group != nullptr) {
                group->try_cancel_and_join();
            }
        }

        template<typename C>
        [[nodiscard]] work_item_ptr try_make_work_item(
            C &&callback, optional_callback_parameters const *params = nullptr) {
            work_item_ptr work_item;
            {
                auto grab{ac::acquire_resource_shared(lock_)};
                if (group_) {
                    work_item = group_->make_work_item(std::forward<C>(callback), params);
                }
            }
            return work_item;
        }

        template<typename C>
        [[nodiscard]] timer_work_item_ptr try_make_timer_work_item(
            C &&callback, optional_callback_parameters const *params = nullptr) {
            timer_work_item_ptr timer_work_item;
            {
                auto grab{ac::acquire_resource_shared(lock_)};
                if (group_) {
                    timer_work_item = group_->make_timer_work_item(
                        std::forward<C>(callback), params);
                }
            }
            return timer_work_item;
        }

        template<typename C>
        [[nodiscard]] wait_work_item_ptr try_make_wait_work_item(
            C &&callback, optional_callback_parameters const *params = nullptr) {
            wait_work_item_ptr wait_work_item;
            {
                auto grab{ac::acquire_resource_shared(lock_)};
                if (group_) {
                    wait_work_item = group_->make_wait_work_item(
                        std::forward<C>(callback), params);
                }
            }
            return wait_work_item;
        }

        template<typename C>
        [[nodiscard]] io_handler_ptr try_make_io_handler(
            HANDLE handle, C &&callback, optional_callback_parameters const *params = nullptr) {
            io_handler_ptr io_handler;
            {
                auto grab{ac::acquire_resource_shared(lock_)};
                if (group_) {
                    io_handler = group_->make_io_handler(
                        handle, std::forward<C>(callback), params);
                }
            }
            return io_handler;
        }

        template<typename C>
        [[nodiscard]] work_item_ptr try_post(C &&callback,
                                             optional_callback_parameters const *params = nullptr) {
            work_item_ptr work_item;
            auto grab{ac::acquire_resource_shared(lock_)};
            if (group_) {
                work_item = group_->post(std::forward<C>(callback));
            }
            return work_item;
        }

        template<typename C>
        [[nodiscard]] bool try_post(work_item_ptr &work_item) {
            auto grab{ac::acquire_resource_shared(lock_)};
            if (group_) {
                work_item->post();
                return true;
            }
            return false;
        }

        template<typename C>
        inline [[nodiscard]] bool try_submit_work(C &&callback) {
            auto grab{ac::acquire_resource_shared(lock_)};
            if (group_) {
                group_->submit_work(std::forward<C>(callback));
                return true;
            }
            return false;
        }

        template<typename C>
        inline [[nodiscard]] bool try_submit_work(C &&callback,
                                                  optional_callback_parameters const *params) {
            auto grab{ac::acquire_resource_shared(lock_)};
            if (group_) {
                group_->submit_work(std::forward<C>(callback), params);
                return true;
            }
            return false;
        }

        template<typename C>
        [[nodiscard]] timer_work_item_ptr try_schedule(C &&callback,
                                                       time_point const &due_time,
                                                       DWORD window_length = 0,
                                                       optional_callback_parameters const *params = nullptr) {
            timer_work_item_ptr timer_work_item;
            {
                auto grab{ac::acquire_resource_shared(lock_)};
                if (group_) {
                    timer_work_item = group_->schedule(
                        std::forward<C>(callback), due_time, window_length);
                }
            }
            return timer_work_item;
        }

        template<typename C>
        [[nodiscard]] bool try_schedule(timer_work_item_ptr &timer_work_item,
                                        time_point const &due_time,
                                        DWORD window_length = 0,
                                        optional_callback_parameters const *params = nullptr) {
            auto grab{ac::acquire_resource_shared(lock_)};
            if (group_) {
                timer_work_item->schedule(due_time, window_length, params);
                return true;
            }
            return false;
        }

        template<typename C>
        [[nodiscard]] timer_work_item_ptr try_schedule(C &&callback,
                                                       duration const &due_time,
                                                       DWORD window_length = 0,
                                                       optional_callback_parameters const *params = nullptr) {
            timer_work_item_ptr timer_work_item;
            {
                auto grab{ac::acquire_resource_shared(lock_)};
                if (group_) {
                    timer_work_item = group_->schedule(
                        std::forward<C>(callback), due_time, window_length);
                }
            }
            return timer_work_item;
        }

        template<typename C>
        [[nodiscard]] bool try_schedule(timer_work_item_ptr &timer_work_item,
                                        duration const &due_time,
                                        DWORD window_length = 0,
                                        optional_callback_parameters const *params = nullptr) {
            auto grab{ac::acquire_resource_shared(lock_)};
            if (group_) {
                timer_work_item->schedule(due_time, window_length, params);
                return true;
            }
            return false;
        }

        template<typename C>
        [[nodiscard]] wait_work_item_ptr try_schedule_wait(
            C &&callback,
            HANDLE handle,
            duration const &due_time = infinite_duration,
            optional_callback_parameters const *params = nullptr) {
            wait_work_item_ptr wait_work_item;
            auto grab{ac::acquire_resource_shared(lock_)};
            {
                if (group_) {
                    wait_work_item = group_->schedule_wait(
                        std::forward<C>(callback), handle, due_time);
                }
            }
            return wait_work_item;
        }

        template<typename C>
        [[nodiscard]] bool try_schedule_wait(wait_work_item_ptr &wait_work_item,
                                             HANDLE handle,
                                             duration const &due_time = infinite_duration,
                                             optional_callback_parameters const *params = nullptr) {
            auto grab{ac::acquire_resource_shared(lock_)};
            if (group_) {
                wait_work_item->schedule_wait(handle, due_time, params);
                return true;
            }
            return false;
        }

        template<typename C>
        [[nodiscard]] wait_work_item_ptr try_schedule_wait(
            C &&callback,
            HANDLE handle,
            time_point const &due_time,
            optional_callback_parameters const *params = nullptr) {
            wait_work_item_ptr wait_work_item;
            auto grab{ac::acquire_resource_shared(lock_)};
            {
                if (group_) {
                    wait_work_item = group_->schedule_wait(
                        std::forward<C>(callback), handle, due_time);
                }
            }
            return wait_work_item;
        }

        template<typename C>
        [[nodiscard]] bool try_schedule_wait(wait_work_item_ptr &wait_work_item,
                                             HANDLE handle,
                                             time_point const &due_time,
                                             optional_callback_parameters const *params = nullptr) {
            auto grab{ac::acquire_resource_shared(lock_)};
            if (group_) {
                wait_work_item->schedule_wait(handle, due_time, params);
                return true;
            }
            return false;
        }

        [[nodiscard]] io_guard try_start_io(io_handler_ptr &io_handler) noexcept {
            auto grab{ac::acquire_resource_shared(lock_)};
            if (group_) {
                return io_handler->start_io();
            }
            return io_guard{};
        }

        resource_owner<rw_lock, rw_lock::acqiure_shared_traits_t> try_acquire() noexcept {
            auto grab{ac::acquire_resource_shared(lock_)};
            if (!group_) {
                grab.release();
            }
            return grab;
        }

    private:
        mutable ac::rw_lock lock_;
        cleanup_group_ptr group_;
    };

    inline [[nodiscard]] thread_pool_ptr make_thread_pool(
        unsigned long max_threads = ULONG_MAX,
        unsigned long min_threads = ULONG_MAX,
        PTP_POOL_STACK_INFORMATION stack_information = nullptr) {
        return thread_pool::make(max_threads, min_threads, stack_information);
    }

    inline cleanup_group_ptr thread_pool::make_cleanup_group() {
        return cleanup_group::make(shared_from_this());
    }

    inline [[nodiscard]] cleanup_group_ptr make_cleanup_group() {
        return cleanup_group::make();
    }

    template<typename C>
    inline [[nodiscard]] work_item_ptr make_work_item(C &&callback) {
        return work_item::make(std::forward<C>(callback));
    }

    template<typename C>
    inline [[nodiscard]] work_item_ptr make_work_item(C &&callback,
                                                      optional_callback_parameters const *params) {
        callback_environment environment;
        environment.set_callback_optional_parameters(params);

        return work_item::make(std::forward<C>(callback), &environment);
    }

    template<typename C>
    inline [[nodiscard]] timer_work_item_ptr make_timer_work_item(C &&callback) {
        return timer_work_item::make(std::forward<C>(callback));
    }

    template<typename C>
    inline [[nodiscard]] timer_work_item_ptr make_timer_work_item(
        C &&callback, optional_callback_parameters const *params) {
        callback_environment environment;
        environment.set_callback_optional_parameters(params);

        return timer_work_item::make(std::forward<C>(callback), &environment);
    }

    template<typename C>
    inline [[nodiscard]] wait_work_item_ptr make_wait_work_item(C &&callback) {
        return wait_work_item::make(std::forward<C>(callback));
    }

    template<typename C>
    inline [[nodiscard]] wait_work_item_ptr make_wait_work_item(
        C &&callback, optional_callback_parameters const *params) {
        callback_environment environment;
        environment.set_callback_optional_parameters(params);

        return wait_work_item::make(std::forward<C>(callback), &environment);
    }

    template<typename C>
    inline [[nodiscard]] io_handler_ptr make_io_handler(HANDLE handle, C &&callback) {
        return io_handler::make(handle, std::forward<C>(callback));
    }

    template<typename C>
    inline [[nodiscard]] io_handler_ptr make_io_handler(
        HANDLE handle, C &&callback, optional_callback_parameters const *params) {
        callback_environment environment;
        environment.set_callback_optional_parameters(params);

        return io_handler::make(handle, std::forward<C>(callback), &environment);
    }

    template<typename C>
    inline work_item_ptr post(C &&callback) {
        work_item_ptr work_item{make_work_item(std::forward<C>(callback))};
        work_item->post();
        return work_item;
    }

    template<typename C>
    inline work_item_ptr post(C &&callback, optional_callback_parameters const *params) {
        work_item_ptr work_item{make_work_item(std::forward<C>(callback), params)};
        work_item->post();
        return work_item;
    }

    template<typename C>
    inline void submit_work(C &&callback) {
        std::unique_ptr<C> cb{new C{std::forward<C>(callback)}};
        if (TrySubmitThreadpoolCallback(&details::submit_work_worker<C>, cb.get(), nullptr)) {
            cb.release();
        } else {
            AC_THROW(GetLastError(), "TrySubmitThreadpoolCallback");
        }
        return;
    }

    template<typename C>
    inline void submit_work(C &&callback, optional_callback_parameters const *params) {
        std::unique_ptr<C> cb{new C{std::forward<C>(callback)}};
        callback_environment environment;
        environment.set_callback_optional_parameters(params);
        if (TrySubmitThreadpoolCallback(
                &details::submit_work_worker<C>, cb.get(), environment.get_handle())) {
            cb.release();
        } else {
            AC_THROW(GetLastError(), "TrySubmitThreadpoolCallback");
        }
        return;
    }

    template<typename C>
    inline timer_work_item_ptr schedule(C &&callback,
                                        time_point const &due_time,
                                        DWORD window_length = 0) {
        timer_work_item_ptr timer_work_item{make_timer_work_item(std::forward<C>(callback))};
        timer_work_item->schedule(due_time, window_length);
        return timer_work_item;
    }

    template<typename C>
    inline timer_work_item_ptr schedule(C &&callback,
                                        time_point const &due_time,
                                        DWORD window_length,
                                        optional_callback_parameters const *params) {
        timer_work_item_ptr timer_work_item{
            make_timer_work_item(std::forward<C>(callback), params)};
        timer_work_item->schedule(due_time, window_length);
        return timer_work_item;
    }

    template<typename C>
    inline timer_work_item_ptr schedule(C &&callback,
                                        duration const &due_time,
                                        DWORD window_length = 0) {
        timer_work_item_ptr timer_work_item{make_timer_work_item(std::forward<C>(callback))};
        timer_work_item->schedule(due_time, window_length);
        return timer_work_item;
    }

    template<typename C>
    inline timer_work_item_ptr schedule(C &&callback,
                                        duration const &due_time,
                                        DWORD window_length,
                                        optional_callback_parameters const *params) {
        timer_work_item_ptr timer_work_item{
            make_timer_work_item(std::forward<C>(callback), params)};
        timer_work_item->schedule(due_time, window_length);
        return timer_work_item;
    }

    template<typename C>
    inline wait_work_item_ptr schedule_wait(C &&callback,
                                            HANDLE handle,
                                            duration const &due_time = infinite_duration) {
        wait_work_item_ptr wait_work_item{make_wait_work_item(std::forward<C>(callback))};
        wait_work_item->schedule_wait(handle, due_time);
        return wait_work_item;
    }

    template<typename C>
    inline wait_work_item_ptr schedule_wait(C &&callback,
                                            HANDLE handle,
                                            duration const &due_time,
                                            optional_callback_parameters const *params) {
        wait_work_item_ptr wait_work_item{
            make_wait_work_item(std::forward<C>(callback), params)};
        wait_work_item->schedule_wait(handle, due_time);
        return wait_work_item;
    }

    template<typename C>
    inline wait_work_item_ptr schedule_wait(C &&callback, HANDLE handle, time_point const &due_time) {
        wait_work_item_ptr wait_work_item{make_wait_work_item(std::forward<C>(callback))};
        wait_work_item->schedule_wait(handle, due_time);
        return wait_work_item;
    }

    template<typename C>
    inline wait_work_item_ptr schedule_wait(C &&callback,
                                            HANDLE handle,
                                            time_point const &due_time,
                                            optional_callback_parameters const *params) {
        wait_work_item_ptr wait_work_item{
            make_wait_work_item(std::forward<C>(callback), params)};
        wait_work_item->schedule_wait(handle, due_time);
        return wait_work_item;
    }

    template<typename T>
    class scoped_join {
    public:
        scoped_join() {
        }

        explicit scoped_join(std::shared_ptr<T> p)
            : p_{std::move(p)} {
        }

        scoped_join(scoped_join const &other) = delete;
        scoped_join &operator=(scoped_join const &other) = delete;

        scoped_join(scoped_join &&other)
            : p_{std::move(other.p_)} {
            other.p_.reset();
        }

        scoped_join &operator=(scoped_join &&other) {
            if (&other != this) {
                p_ = std::move(other.p_);
                other.p_.reset();
            }
            return *this;
        }

        ~scoped_join() noexcept {
            join();
        }

        void join() noexcept {
            if (p_) {
                p_->join();
                p_.reset();
            }
        }

        [[nodiscard]] bool is_armed() const noexcept {
            return p_ != nullptr;
        }

        operator bool() const noexcept {
            return is_armed();
        }

        void disarm() noexcept {
            p_.reset();
        }

    private:
        std::shared_ptr<T> p_{};
    };

} // namespace ac::tp

#endif //_AC_HELPERS_WIN32_LIBRARY_THREAD_POOL_HEADER_