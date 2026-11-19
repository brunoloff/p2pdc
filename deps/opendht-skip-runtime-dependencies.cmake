# Run in OpenDHT's source folder on Windows. OpenDHT installs copies of every DLL it uses, but
# CMake cannot find MSYS2's DLLs there and stops the install; programs find them on their own.
set(rule "install(RUNTIME_DEPENDENCY_SET opendht_rdeps")
set(skipped "set(p2pdc_skipped_opendht_rdeps")
file(READ CMakeLists.txt text)
string(FIND "${text}" "${skipped}" done)
if(NOT done EQUAL -1)
    return() # already applied
endif()
string(FIND "${text}" "${rule}" found)
if(found EQUAL -1)
    message(FATAL_ERROR "OpenDHT's CMakeLists.txt no longer has '${rule}'; update this script.")
endif()
# The rule's arguments stay, as the value of an unused variable.
string(REPLACE "${rule}" "${skipped}" text "${text}")
file(WRITE CMakeLists.txt "${text}")
