# Hand-rolled test runner for Phase 0 — no external test framework needed
# yet. Swap in GoogleTest (via FetchContent) once assertions get numerous
# enough that a real framework's diffing/output pays for itself — a good
# Phase 2 or Phase 3 upgrade, not a Phase 0 one.
add_executable(test_disk_manager test_disk_manager.cpp)
target_link_libraries(test_disk_manager PRIVATE dbengine_core)
add_test(NAME DiskManagerTest COMMAND test_disk_manager)

add_executable(test_bplus_tree test_bplus_tree.cpp)
target_link_libraries(test_bplus_tree PRIVATE dbengine_core)
add_test(NAME BPlusTreeTest COMMAND test_bplus_tree)
