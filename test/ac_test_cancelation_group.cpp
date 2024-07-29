#include "ac_test_cancelation_group.h"

#include <stdlib.h>

#include "..\actp.h"
#include "..\acrundown.h"
#include "..\ackernelobject.h"
#include "..\acfileobject.h"

struct cancelation_group_test_context {
    static constexpr size_t work_to_start{100};
    static constexpr size_t waits_to_start{100};
    static constexpr size_t timers_to_start{100};
    static constexpr size_t io_handlers_to_start{2};
    static constexpr size_t ios_to_start{10};

    std::vector<ac::tp::work_item_ptr> posted_work_items_;
    std::vector<ac::tp::wait_work_item_ptr> posted_wait_work_items_;
    std::vector<ac::tp::timer_work_item_ptr> posted_timer_work_items_;
    std::vector<ac::tp::io_handler_ptr> started_io_handlers_;

    std::atomic<int> timers_executed_;
    std::atomic<int> work_executed_;
    std::atomic<int> waits_executed;
    std::atomic<int> ios_executed;

    ac::event event;

    template<typename T>
    static void validate_posted_work_in_collection_is_closed(std::vector<T> const &posted_work) {
        for (auto const &work : posted_work) {
            AC_CODDING_ERROR_IF_NOT(work->is_closed());
        }
    }

    void validate_posted_work_is_closed() {
        validate_posted_work_in_collection_is_closed(posted_work_items_);
        validate_posted_work_in_collection_is_closed(posted_wait_work_items_);
        validate_posted_work_in_collection_is_closed(posted_timer_work_items_);
        validate_posted_work_in_collection_is_closed(started_io_handlers_);
    }

    void clear() {
        posted_work_items_.clear();
        posted_wait_work_items_.clear();
        posted_timer_work_items_.clear();
        started_io_handlers_.clear();

        timers_executed_ = 0;
        work_executed_ = 0;
        waits_executed = 0;
        ios_executed = 0;

        event.close();
    }

    void reserve() {
        posted_work_items_.reserve(work_to_start);
        posted_wait_work_items_.reserve(waits_to_start);
        posted_timer_work_items_.reserve(timers_to_start);
        started_io_handlers_.reserve(io_handlers_to_start);
    }

    void prepare_for_next_test() {
        clear();
        reserve();
        event.create(ac::event::manuel, ac::event::unsignaled);
    }
};

void test_start_waits_on_cancelation_group(cancelation_group_test_context &context,
                                           ac::tp::safe_cleanup_group &group) {
    printf("---- test_start_waits_on_cancelation_group count %zi\n", context.waits_to_start);

    for (size_t i = 0; i < context.waits_to_start; ++i) {
        context.posted_wait_work_items_.push_back(group.try_schedule_wait(
            [&context](ac::tp::callback_instance &instance, TP_WAIT_RESULT wait_result) {
                UNREFERENCED_PARAMETER(wait_result);
                context.waits_executed += 1;
            },
            context.event.get_handle(),
            ac::tp::seconds{300}));
        ac::tp::wait_work_item_ptr &wait = context.posted_wait_work_items_.back();
        AC_CODDING_ERROR_IF(wait->is_closed());
    }
}

void test_start_timers_on_cancelation_group(cancelation_group_test_context &context,
                                            ac::tp::safe_cleanup_group &group) {
    printf("---- test_start_timers_on_cancelation_group count %zi\n", context.timers_to_start);

    for (size_t i = 0; i < context.timers_to_start; ++i) {
        context.posted_timer_work_items_.push_back(group.try_schedule(
            [&context](ac::tp::callback_instance &instance) {
                context.timers_executed_ += 1;
            },
            ac::tp::seconds{5},
            0));
        ac::tp::timer_work_item_ptr &timer = context.posted_timer_work_items_.back();
        AC_CODDING_ERROR_IF(timer->is_closed());
        AC_CODDING_ERROR_IF_NOT(timer->is_scheduled());
    }
}

#define TEST_DEFAULT_TP_IO_HANDLER_FILE_NAME L"foo.tst"
#define TEST_DEFAULT_TP_IO_HANDLER_FILE_SIZE (10LL * 1024LL * 1024LL)
#define TEST_DEFAULT_TP_IO_HANDLER_FILE_IO_SIZE (16 * 4096LL * 1024LL) // 1MB

void test_start_io_handler_on_cancelation_group(cancelation_group_test_context &context,
                                                ac::tp::safe_cleanup_group &group) {
    printf("---- test_start_io_handler_on_cancelation_group handlers count %zi "
           "ios count %zi\n",
           context.io_handlers_to_start,
           context.ios_to_start);

    ac::cbuffer buffer;
    buffer.resize(TEST_DEFAULT_TP_IO_HANDLER_FILE_IO_SIZE);

    for (size_t i = 0; i < context.io_handlers_to_start; ++i) {
        ac::file_object fo;
        fo.create(TEST_DEFAULT_TP_IO_HANDLER_FILE_NAME,
                  GENERIC_READ | GENERIC_WRITE,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  OPEN_ALWAYS,
                  FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED);

        context.started_io_handlers_.push_back(group.try_make_io_handler(
            fo.get_handle(),
            [&context](ac::tp::callback_instance &instance, OVERLAPPED *overlapped, ULONG, ULONG_PTR) {
                context.ios_executed += 1;
                std::unique_ptr<OVERLAPPED> overlapped_ptr{overlapped};
            }));

        ac::tp::io_handler_ptr &io_handler = context.started_io_handlers_.back();
        AC_CODDING_ERROR_IF(io_handler->is_closed());

        for (long long i = 0; i < context.ios_to_start; ++i) {
            auto io_guard{group.try_start_io(io_handler)};
            if (!io_guard) {
                AC_CODDING_ERROR_IF_NOT(io_guard.is_armed());

                long long offset = (i * TEST_DEFAULT_TP_IO_HANDLER_FILE_IO_SIZE) %
                                   TEST_DEFAULT_TP_IO_HANDLER_FILE_SIZE;
                std::unique_ptr<OVERLAPPED> overlapped_ptr{std::make_unique<OVERLAPPED>()};
                overlapped_ptr->hEvent = nullptr;
                overlapped_ptr->Offset = ac::get_low_dword(offset);
                overlapped_ptr->OffsetHigh = ac::get_high_dword(offset);

                //
                // throws on failure; on success of pending completion will go through io_handler
                //
                (void) fo.write(&buffer[0],
                                static_cast<DWORD>(buffer.size()),
                                overlapped_ptr.get());
                //
                // If we successfully started IO then release ownership of the overlapped pointer.
                // io_handler on completion will take over ownership and will delete this structure
                //
                overlapped_ptr.release();
                //
                // we successfully initiated IO so we can disarm IO guard
                //
                io_guard.disarm();
            }
        }
    }
}

void test_start_work_on_cancelation_group(cancelation_group_test_context &context,
                                          ac::tp::safe_cleanup_group &group) {
    printf("---- test_start_work_on_cancelation_group count %zi\n", context.work_to_start);

    for (size_t i = 0; i < context.work_to_start; ++i) {
        context.posted_work_items_.push_back(
            group.try_post([&context](ac::tp::callback_instance &instance) {
                context.work_executed_ += 1;
                Sleep(1);
            }));
        ac::tp::work_item_ptr &timer = context.posted_work_items_.back();
        AC_CODDING_ERROR_IF(timer->is_closed());
    }
}

void test_cancelation_group(ac::tp::thread_pool_ptr const &thread_pool) {
    ac::tp::safe_cleanup_group cancelation_group{thread_pool};

    constexpr int join_count{6};

    cancelation_group_test_context context;

    ac::scoped_file_delete scoped_delete{TEST_DEFAULT_TP_IO_HANDLER_FILE_NAME};

    for (int i = 0; i < join_count; ++i) {
        context.prepare_for_next_test();

        printf("---- test_cancelation_group round %i\n", i);
        test_start_waits_on_cancelation_group(context, cancelation_group);
        test_start_timers_on_cancelation_group(context, cancelation_group);
        test_start_work_on_cancelation_group(context, cancelation_group);
        test_start_io_handler_on_cancelation_group(context, cancelation_group);

        if (i % 3) {
            printf("---- test_cancelation_group setting event\n");
            context.event.set();
        }

        if (i % 2) {
            printf("---- test_cancelation_group joining\n");
            cancelation_group.join();
        } else {
            printf(
                "---- test_cancelation_group trying to cancel and joining\n");
            cancelation_group.try_cancel_and_join();
        }

        printf("---- test_cancelation_group validating\n");
        context.validate_posted_work_is_closed();

        printf("---- test_cancelation_group reinitializing\n");
        cancelation_group.reinitialize();
    }
}

void test_default_thread_pool_cancelation_group() {
    printf("\n---- test_default_thread_pool_cancelation_group\n");

    try {
        test_cancelation_group(ac::tp::thread_pool_ptr{});
    } catch (std::exception const &ex) {
        printf("---- test_default_thread_pool_cancelation_group failed %s\n", ex.what());
    }
    printf("---- test_default_thread_pool_cancelation_group complete\n");
}

void test_thread_pool_cancelation_group() {
    printf("\n---- test_thread_pool_cancelation_group\n");

    try {
        auto thread_pool{ac::tp::make_thread_pool(16, 8)};

        test_cancelation_group(thread_pool);
    } catch (std::exception const &ex) {
        printf("---- test_thread_pool_cancelation_group failed %s\n", ex.what());
    }
    printf("---- test_thread_pool_cancelation_group complete\n");
}

struct cancelation_group_stresstest_context {
    std::atomic<bool> shutdown{false};

    std::atomic<int> succeeded_to_post_work{0};
    std::atomic<int> succeeded_to_start_timer{0};
    std::atomic<int> succeeded_to_start_wait{0};
    std::atomic<int> succeeded_to_start_io{0};

    std::atomic<int> failed_to_post_work{0};
    std::atomic<int> failed_to_start_timer{0};
    std::atomic<int> failed_to_start_wait{0};
    std::atomic<int> failed_to_start_io{0};

    static constexpr int outstanding_work_limit{1000};
    static constexpr int outstanding_timer_work_limit{1000};
    static constexpr int outstanding_wait_limit{1000};
    static constexpr int outstanding_io_limit{1000};

    static constexpr int restart_count{100};
};

void stress_test_cancelation_group_post_work(cancelation_group_stresstest_context &context,
                                             ac::tp::safe_cleanup_group &cancelation_group) {
    printf("----- stress_test_cancelation_group_post_work started\n");

    size_t iteration{0};

    while (!context.shutdown) {
        ++iteration;
        try {
            auto work = cancelation_group.try_post(
                [&context](ac::tp::callback_instance &instance) {
                    Sleep(0);
                });

            if (work) {
                context.succeeded_to_post_work += 1;
                if (iteration % 10) {
                    Sleep(0); // periodically yield
                }
            } else {
                context.failed_to_post_work += 1;
                Sleep(0);
            }
        } catch (std::exception const &ex) {
            printf("---- stress_test_cancelation_group_post_work "
                   "failed %s\n",
                   ex.what());
            context.shutdown = true;
        }
    }
    printf("----- stress_test_cancelation_group_post_work exiting\n");
}

void stress_test_cancelation_group_post_timer_work(cancelation_group_stresstest_context &context,
                                                   ac::tp::safe_cleanup_group &cancelation_group) {
    printf("----- stress_test_cancelation_group_post_timer_work started\n");

    size_t iteration{0};

    while (!context.shutdown) {
        ++iteration;
        try {
            auto work = cancelation_group.try_schedule(
                [&context](ac::tp::callback_instance &instance) {
                    Sleep(0);
                },
                ac::tp::microseconds{500},
                0);

            if (work) {
                context.succeeded_to_start_timer += 1;
                if (iteration % 10) {
                    Sleep(0); // periodically yield
                }
            } else {
                context.failed_to_start_timer += 1;
                Sleep(0);
            }

        } catch (std::exception const &ex) {
            printf("---- stress_test_cancelation_group_post_timer_work "
                   "failed %s\n",
                   ex.what());
            context.shutdown = true;
        }
    }
    printf("----- stress_test_cancelation_group_post_timer_work exiting\n");
}

void stress_test_cancelation_group_post_wait_work(cancelation_group_stresstest_context &context,
                                                  ac::tp::safe_cleanup_group &cancelation_group) {
    printf("----- stress_test_cancelation_group_post_wait_work started\n");

    size_t iteration{0};

    ac::event work_restart_event{ac::event::manuel, ac::event::unsignaled};

    while (!context.shutdown) {
        ++iteration;
        try {
            auto work = cancelation_group.try_schedule_wait(
                [&context](
                    ac::tp::callback_instance &instance, TP_WAIT_RESULT) {
                    Sleep(0);
                },
                work_restart_event.get_handle(),
                ac::tp::seconds{1},
                0);

            if (work) {
                context.succeeded_to_start_wait += 1;
                if (iteration % 10) {
                    Sleep(0); // periodically yield
                    work_restart_event.pulse();
                }
            } else {
                work_restart_event.pulse();
                context.failed_to_start_wait += 1;
                Sleep(0);
            }
        } catch (std::exception const &ex) {
            printf("---- stress_test_cancelation_group_post_wait_work "
                   "failed %s\n",
                   ex.what());
            context.shutdown = true;
        }
    }
    printf("----- stress_test_cancelation_group_post_wait_work exiting\n");
}

void stress_test_cancelation_group_join_reinitialize(cancelation_group_stresstest_context &context,
                                                     ac::tp::safe_cleanup_group &cancelation_group) {
    Sleep(5);

    for (int i = 0; i < context.restart_count; ++i) {
    
        printf("----- stress_test_cancelation_group_join_reinitialize "
               "iteration %i\n",
               i);

        Sleep(3);

        int succeeded_to_post_work{0};
        int succeeded_to_start_timer{0};
        int succeeded_to_start_wait{0};
        int succeeded_to_start_io{0};

        int failed_to_post_work{0};
        int failed_to_start_timer{0};
        int failed_to_start_wait{0};
        int failed_to_start_io{0};

        if (i % 2) {
            cancelation_group.join();
        } else {
            cancelation_group.try_cancel_and_join();
        }

        succeeded_to_post_work = context.succeeded_to_post_work.exchange(0);
        succeeded_to_start_timer = context.succeeded_to_start_timer.exchange(0);
        succeeded_to_start_wait = context.succeeded_to_start_wait.exchange(0);
        succeeded_to_start_io = context.succeeded_to_start_io.exchange(0);

        printf("----- stress_test_cancelation_group_join_reinitialize "
               "succeeded work: %i, timer: %i, wait: %i, io: %i\n",
               succeeded_to_post_work,
               succeeded_to_start_timer,
               succeeded_to_start_wait,
               succeeded_to_start_io);

        Sleep(3);

        cancelation_group.reinitialize();

        failed_to_post_work = context.failed_to_post_work.exchange(0);
        failed_to_start_timer = context.failed_to_start_timer.exchange(0);
        failed_to_start_wait = context.failed_to_start_wait.exchange(0);
        failed_to_start_io = context.failed_to_start_io.exchange(0);

        printf("----- stress_test_cancelation_group_join_reinitialize "
               "failed work: %i, timer: %i, wait: %i, io: %i\n",
               failed_to_post_work,
               failed_to_start_timer,
               failed_to_start_wait,
               failed_to_start_io);
    }
}

void stresstest_cancelation_group(ac::tp::thread_pool_ptr const &thread_pool) {
    ac::slim_rundown rundown;
    ac::slim_rundown_join scoped_join(&rundown);

    cancelation_group_stresstest_context context;
    ac::scope_guard set_shutdow{
        ac::make_scope_guard([&context] { context.shutdown = true; })};

    ac::tp::safe_cleanup_group cancelation_group{thread_pool};

    ac::tp::post([&context,
                  rundown_guard = std::move(ac::slim_rundown_lock{&rundown}),
                  &cancelation_group](ac::tp::callback_instance &instance) {
        stress_test_cancelation_group_post_work(context, cancelation_group);
    });

    ac::tp::post([&context,
                  rundown_guard = std::move(ac::slim_rundown_lock{&rundown}),
                  &cancelation_group](ac::tp::callback_instance &instance) {
        stress_test_cancelation_group_post_timer_work(context, cancelation_group);
    });

    ac::tp::post([&context,
                  rundown_guard = std::move(ac::slim_rundown_lock{&rundown}),
                  &cancelation_group](ac::tp::callback_instance &instance) {
        stress_test_cancelation_group_post_wait_work(context, cancelation_group);
    });

    stress_test_cancelation_group_join_reinitialize(context, cancelation_group);
}

void stresstest_default_thread_pool_cancelation_group() {
    printf("\n---- stresstest_default_thread_pool_cancelation_group\n");

    try {
        stresstest_cancelation_group(ac::tp::thread_pool_ptr{});
    } catch (std::exception const &ex) {
        printf(
            "---- stresstest_default_thread_pool_cancelation_group failed %s\n",
            ex.what());
    }
    printf("---- stresstest_default_thread_pool_cancelation_group complete\n");
}

void stresstest_thread_pool_cancelation_group() {
    printf("\n---- stresstest_thread_pool_cancelation_group\n");

    try {
        auto thread_pool{ac::tp::make_thread_pool(16, 8)};

        stresstest_cancelation_group(thread_pool);
    } catch (std::exception const &ex) {
        printf("---- stresstest_thread_pool_cancelation_group failed %s\n", ex.what());
    }
    printf("---- stresstest_thread_pool_cancelation_group complete\n");
}
