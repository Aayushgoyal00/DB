aayus@Xiaomi_Book MINGW64 /e/software ideas/DB (main)
$ ./build/transaction_update_benchmark.exe benchmark_update.db benchmark_update.wal 16000 500

Concurrent UPDATE benchmark
Total operations per thread-count: 16000
Keys: 1-500

Testing 1 thread (16000 ops/thread, 16000 total)...
  TPS: 113.76
  p50: 8.44 ms
  p95: 12.38 ms
  p99: 14.25 ms
  Committed: 16000
  Failed: 0

Testing 2 threads (8000 ops/thread, 16000 total)...
  TPS: 147.86
  p50: 13.80 ms
  p95: 21.57 ms
  p99: 25.65 ms
  Committed: 16000
  Failed: 0

Testing 4 threads (4000 ops/thread, 16000 total)...
  TPS: 228.75
  p50: 13.30 ms
  p95: 37.70 ms
  p99: 49.22 ms
  Committed: 16000
  Failed: 0

Testing 8 threads (2000 ops/thread, 16000 total)...
  TPS: 251.37
  p50: 17.41 ms
  p95: 83.84 ms
  p99: 107.60 ms
  Committed: 16000
  Failed: 0

Testing 16 threads (1000 ops/thread, 16000 total)...
  TPS: 261.45
  p50: 25.36 ms
  p95: 186.46 ms
  p99: 247.34 ms
  Committed: 16000
  Failed: 0

Testing 32 threads (500 ops/thread, 16000 total)...
  TPS: 220.00
  p50: 51.97 ms
  p95: 309.93 ms
  p99: 509.81 ms
  Committed: 16000
  Failed: 0


============================================================
Threads            TPS    p50 (ms)    p95 (ms)    p99 (ms)      Failed
------------------------------------------------------------
1               113.76        8.44       12.38       14.25           0
2               147.86       13.80       21.57       25.65           0
4               228.75       13.30       37.70       49.22           0
8               251.37       17.41       83.84      107.60           0
16              261.45       25.36      186.46      247.34           0
32              220.00       51.97      309.93      509.81           0
============================================================





for 100 operations and 1000 keys
============================================================
Threads            TPS    p50 (ms)    p95 (ms)    p99 (ms)      Failed
------------------------------------------------------------
1                80.34        9.91       13.75       16.47           0
2               116.84       15.63       24.86       29.52           0
4               117.34       27.37       56.24      101.32           0
8               180.70       25.61       79.28      109.92           0
16              194.52       36.16      109.61      119.17           0
32              194.28       70.91      172.54      208.45           0
============================================================






aayus@Xiaomi_Book MINGW64 /e/software ideas/DB (main)
$ ./build/transaction_crud_benchmark.exe benchmark_crud.db benchmark_crud.wal 1000 500

Transaction CRUD concurrency benchmark
Total operations per thread-count: 1000
Keys: 1-500
Workload: UPDATE / DELETE / UPSERT

Building seed database (500 keys, once)...
Seed database ready.

Testing 1 thread (1000 ops/thread, 1000 total)...
  TPS: 121.35
  p50: 7.51 ms
  p95: 13.05 ms
  p99: 14.65 ms
  Committed: 1000
  Expected NotFound: 0
  Other errors: 0

Testing 2 threads (500 ops/thread, 1000 total)...
  TPS: 138.03
  p50: 13.11 ms
  p95: 26.94 ms
  p99: 28.27 ms
  Committed: 961
  Expected NotFound: 39
  Other errors: 0

Testing 4 threads (250 ops/thread, 1000 total)...
  TPS: 121.08
  p50: 23.71 ms
  p95: 63.28 ms
  p99: 82.04 ms
  Committed: 810
  Expected NotFound: 190
  Other errors: 0

Testing 8 threads (125 ops/thread, 1000 total)...
  TPS: 121.82
  p50: 35.66 ms
  p95: 136.85 ms
  p99: 163.59 ms
  Committed: 768
  Expected NotFound: 232
  Other errors: 0

Testing 16 threads (62 ops/thread, 992 total)...
  TPS: 190.78
  p50: 37.05 ms
  p95: 192.81 ms
  p99: 289.11 ms
  Committed: 735
  Expected NotFound: 257
  Other errors: 0

Testing 32 threads (31 ops/thread, 992 total)...
  TPS: 215.55
  p50: 29.90 ms
  p95: 260.48 ms
  p99: 992.89 ms
  Committed: 713
  Expected NotFound: 279
  Other errors: 0


==============================================================
Threads          TPS     p50(ms)     p95(ms)     p99(ms)      Commit    NotFound      Errors
--------------------------------------------------------------
1             121.35        7.51       13.05       14.65        1000           0           0
2             138.03       13.11       26.94       28.27         961          39           0
4             121.08       23.71       63.28       82.04         810         190           0
8             121.82       35.66      136.85      163.59         768         232           0
16            190.78       37.05      192.81      289.11         735         257           0
32            215.55       29.90      260.48      992.89         713         279           0
==============================================================

aayus@Xiaomi_Book MINGW64 /e/software ideas/DB (main)
$ 



aayus@Xiaomi_Book MINGW64 /e/software ideas/DB (main)
$ ./build/transaction_update_benchmark.exe benchmark_update.db benchmark_update.
wal 1000 500

Concurrent UPDATE benchmark
Total operations per thread-count: 1000
Keys: 1-500

Building seed database (500 keys, once)...
Seed database ready.

Testing 1 thread (1000 ops/thread, 1000 total)...
  TPS: 116.35
  p50: 7.95 ms
  p95: 11.37 ms
  p99: 12.92 ms
  Committed: 1000
  Failed: 0

Testing 2 threads (500 ops/thread, 1000 total)...
  TPS: 134.78
  p50: 14.00 ms
  p95: 20.51 ms
  p99: 24.08 ms
  Committed: 1000
  Failed: 0

Testing 4 threads (250 ops/thread, 1000 total)...
  TPS: 162.86
  p50: 20.69 ms
  p95: 39.22 ms
  p99: 49.87 ms
  Committed: 1000
  Failed: 0

Testing 8 threads (125 ops/thread, 1000 total)...
  TPS: 215.91
  p50: 31.35 ms
  p95: 69.07 ms
  p99: 89.73 ms
  Committed: 1000
  Failed: 0

Testing 16 threads (62 ops/thread, 992 total)...
  TPS: 276.85
  p50: 31.37 ms
  p95: 129.64 ms
  p99: 213.71 ms
  Committed: 992
  Failed: 0

Testing 32 threads (31 ops/thread, 992 total)...
  TPS: 325.84
  p50: 39.64 ms
  p95: 179.78 ms
  p99: 607.64 ms
  Committed: 992
  Failed: 0


============================================================
Threads            TPS    p50 (ms)    p95 (ms)    p99 (ms)      Failed
------------------------------------------------------------
1               116.35        7.95       11.37       12.92           0
2               134.78       14.00       20.51       24.08           0
4               162.86       20.69       39.22       49.87           0
8               215.91       31.35       69.07       89.73           0
16              276.85       31.37      129.64      213.71           0
32              325.84       39.64      179.78      607.64           0
============================================================

aayus@Xiaomi_Book MINGW64 /e/software ideas/DB (main)
$ 




aayus@Xiaomi_Book MINGW64 /e/software ideas/DB (main)
$ ./build/tests/test_transaction_lost_update.exe
Lost-update correctness check
Total operations per configuration: 2000
Timeout per configuration: 60s
Heartbeat interval: 5s

Variant A-single-counter
Running 1 threads...
A-single-counter / 1 threads: PASS (elapsed 1.73s, committed 2000, failed 0, completed 2000/2000)
Running 2 threads...
A-single-counter / 2 threads: PASS (elapsed 1.62s, committed 2000, failed 0, completed 2000/2000)
Running 4 threads...
A-single-counter / 4 threads: PASS (elapsed 1.64s, committed 2000, failed 0, completed 2000/2000)
Running 8 threads...
A-single-counter / 8 threads: PASS (elapsed 1.81s, committed 2000, failed 0, completed 2000/2000)
Running 16 threads...
A-single-counter / 16 threads: PASS (elapsed 1.81s, committed 2000, failed 0, completed 2000/2000)
Running 32 threads...
A-single-counter / 32 threads: PASS (elapsed 1.69s, committed 1984, failed 0, completed 1984/2000)

Variant B-multiple-counters
Running 1 threads...
B-multiple-counters / 1 threads: PASS (elapsed 3.39s, committed 2000, failed 0, completed 2000/2000)
Running 2 threads...
B-multiple-counters / 2 threads: PASS (elapsed 3.46s, committed 2000, failed 0, completed 2000/2000)
Running 4 threads...
B-multiple-counters / 4 threads: PASS (elapsed 3.45s, committed 2000, failed 0, completed 2000/2000)
Running 8 threads...
B-multiple-counters / 8 threads: PASS (elapsed 4.12s, committed 2000, failed 0, completed 2000/2000)
Running 16 threads...
B-multiple-counters / 16 threads: PASS (elapsed 3.96s, committed 2000, failed 0, completed 2000/2000)
Running 32 threads...
B-multiple-counters / 32 threads: PASS (elapsed 3.51s, committed 1984, failed 0, completed 1984/2000)


============================================================
Variant               Threads  Lost-update             Invariants        Elapsed(s)  
------------------------------------------------------------
A-single-counter      1        PASS                    PASS              1.73        
A-single-counter      2        PASS                    PASS              1.62        
A-single-counter      4        PASS                    PASS              1.64        
A-single-counter      8        PASS                    PASS              1.81        
A-single-counter      16       PASS                    PASS              1.81        
A-single-counter      32       PASS                    PASS              1.69        
B-multiple-counters   1        PASS                    PASS              3.39        
B-multiple-counters   2        PASS                    PASS              3.46        
B-multiple-counters   4        PASS                    PASS              3.45        
B-multiple-counters   8        PASS                    PASS              4.12        
B-multiple-counters   16       PASS                    PASS              3.96        
B-multiple-counters   32       PASS                    PASS              3.51        
============================================================

aayus@Xiaomi_Book MINGW64 /e/software ideas/DB (main)
$ 

