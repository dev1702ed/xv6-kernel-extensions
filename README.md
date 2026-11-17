# xv6-kernel-extensions

Extensions to the xv6 teaching kernel (x86), plus a filesystem benchmarking study. The repo has four parts.

1. **Kernel threads and synchronization** (`Kernel threads and synchronization/`): `thread_create`,
   `thread_join` and `thread_exit` system calls for threads that share an address space, plus a user-level
   spinlock and a sleeping mutex, used to fix a race on a shared counter.
2. **Process information and scheduling** (`Hybrid scheduler/`): system calls that report process-table
   state and per-process burst times, plus shortest-job-first and hybrid SJF + round-robin schedulers,
   compared against the default round robin with `test_scheduler`.
3. **Memory management** (`Memory management/`): lazy allocation for `sbrk` handled in the page-fault path
   (Part A), and paging to disk with swap-in/swap-out kernel processes and LRU page selection (Part B).
4. **Quantitative comparison of filesystems** (`filesystem-comparison/`): vdbench workloads that compare
   ZFS with LZ4 compression on and off, and with AES-256-GCM encryption on and off.

Each part has its patch (`file.patch`), the changed files, and the results of the test runs.

## Running

Parts 1–3 are patches against xv6:

```sh
git clone https://github.com/mit-pdos/xv6-public && cd xv6-public && git checkout eeb7b41
patch -p1 < "<folder>/file.patch"
make qemu-nox
```

- Part 2: pick the scheduler with `make clean && make qemu-nox SCHEDPOLICY=RR|SJF|HYBRID`.
- Part 3B: force swapping with `make clean && make qemu-nox PHYSTOP=0x300000`.
- Part 4: from the vdbench directory, run `scripts/compressionTestScript.sh` and
  `scripts/encryptionTestScript.sh`, then run `scripts/cleanScript.sh` to clean up. This needs
  `zfsutils-linux` and a Java runtime.
