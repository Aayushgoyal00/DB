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

add_executable(test_slotted_page test_slotted_page.cpp)
target_link_libraries(test_slotted_page PRIVATE dbengine_core)
add_test(NAME SlottedPageTest COMMAND test_slotted_page)

add_executable(test_buffer_pool_manager test_buffer_pool_manager.cpp)
target_link_libraries(test_buffer_pool_manager PRIVATE dbengine_core)
add_test(NAME BufferPoolManagerTest COMMAND test_buffer_pool_manager)

add_executable(test_wal_recovery test_wal_recovery.cpp)
target_link_libraries(test_wal_recovery PRIVATE dbengine_core)
add_test(NAME WalRecoveryTest COMMAND test_wal_recovery)

add_executable(test_bpm_bplus_tree test_bpm_bplus_tree.cpp)
target_link_libraries(test_bpm_bplus_tree PRIVATE dbengine_core)
add_test(NAME BpmBPlusTreeTest COMMAND test_bpm_bplus_tree)

add_executable(test_transaction_concurrency test_transaction_concurrency.cpp)
target_link_libraries(test_transaction_concurrency PRIVATE dbengine_core)
add_test(NAME TransactionConcurrencyTest COMMAND test_transaction_concurrency)

add_executable(test_crash_recovery test_crash_recovery.cpp)
target_link_libraries(test_crash_recovery PRIVATE dbengine_core)
add_test(NAME CrashRecoveryTest COMMAND test_crash_recovery)

add_executable(test_transaction_lost_update test_transaction_lost_update.cpp)
target_link_libraries(test_transaction_lost_update PRIVATE dbengine_core)
add_test(NAME TransactionLostUpdateTest COMMAND test_transaction_lost_update)
