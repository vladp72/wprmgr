# CMakeList.txt : CMake project for wprmgr, include source and define
# project specific logic here.
#

# Add source to this project's executable.
add_executable (wprmgr "wprmgr.cpp"  "actp.h" "acresourceowner.h" "acrundown.h" "acwaitonaddress.h" "accommon.h" "test/ac_test_thread_pool.h" "test/ac_test_thread_pool.cpp" "ackernelobject.h" "acfileobject.h"  )

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET wprmgr PROPERTY CXX_STANDARD 23)
endif()

# TODO: Add tests and install targets if needed.
