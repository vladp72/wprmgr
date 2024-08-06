// wprmgr.cpp : Defines the entry point for the application.
//

#include "test\ac_test_thread_pool.h"

#include <memory>
#include <atomic>
#include <stdatomic.h>

int main() {

    //test_ft_to_timepoint_conversion();
    
    //test_default_tp_submit_work();
    //test_default_tp_post();
    //test_default_tp_timer_work_item();
    //test_default_tp_wait_work_item();
    //test_default_tp_io_handler();

    test_tp_submit_work();
    //test_tp_post();
    //test_tp_timer_work_item();
    //test_tp_wait_work_item();
    //test_tp_io_handler();

    //test_default_thread_pool_cancelation_group();
    //test_thread_pool_cancelation_group();

    //stresstest_default_thread_pool_cancelation_group();
    //stresstest_thread_pool_cancelation_group();

    return 0;
}
