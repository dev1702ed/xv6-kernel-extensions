#include "types.h"
#include "stat.h"
#include "user.h"
#include "schedtest.h"

// Scheduler test suite. Build the kernel with each policy and compare:
//   make clean && make qemu SCHEDPOLICY=RR      (original xv6)
//   make clean && make qemu SCHEDPOLICY=SJF
//   make clean && make qemu SCHEDPOLICY=HYBRID
// then run `test_scheduler` and compare the tables.
int
main(void)
{
  int i;

  calibrate();
  printf(1, "calibration: %d loop iterations per tick\n", unit);

  // Test 1: six CPU-bound children whose declared burst matches the work
  // they do, forked longest-first. Under SJF they should finish in
  // increasing burst order and the average turnaround should drop.
  int b1[6] = {30, 25, 20, 15, 10, 5};
  int t1[6] = {CPU, CPU, CPU, CPU, CPU, CPU};
  run_batch("Test 1: CPU-bound, bursts 30..5 forked longest first", 6, b1, t1);

  // Test 2: two identical CPU-bound workloads (20 units each); only the
  // declared burst differs. The one declaring the shorter burst should
  // finish first under SJF/hybrid; RR treats them the same.
  int b2[2] = {40, 5};
  int t2[2] = {CPU, CPU};
  printf(1, "\n(test 2: both children do 20 units of work; declared bursts 40 and 5)\n");
  {
    int fds[2], pid, t0, k;
    struct rec r, out[2];
    pipe(fds);
    t0 = uptime();
    for(i = 0; i < 2; i++){
      if((pid = fork()) == 0){
        close(fds[0]);
        set_burst_time(b2[i]);
        cpu_work(20);
        r.pid = getpid(); r.burst = get_burst_time(); r.type = t2[i];
        r.finish = uptime() - t0; r.switches = 0;
        write(fds[1], &r, sizeof(r));
        exit();
      }
    }
    close(fds[1]);
    wait(); wait();
    for(k = 0; k < 2 && read(fds[0], &out[k], sizeof(r)) == sizeof(r); k++)
      printf(1, "finished %d: pid %d declared burst %d, turnaround %d ticks\n",
             k + 1, out[k].pid, out[k].burst, out[k].finish);
    close(fds[0]);
  }

  // Test 3: mixed CPU-bound and I/O-bound children. I/O-bound ones block
  // every tick; the scheduler must still order the CPU-bound ones by
  // burst and must not lose processes that sleep and wake.
  int b3[6] = {25, 10, 20, 15, 5, 30};
  int t3[6] = {CPU, IO, CPU, IO, CPU, IO};
  run_batch("Test 3: mixed CPU-bound and I/O-bound", 6, b3, t3);

  exit();
}
