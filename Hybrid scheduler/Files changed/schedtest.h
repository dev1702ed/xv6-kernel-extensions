// Shared helpers for the scheduler test programs (test_1/2/3, test_scheduler).
// Header-only so every test links against the normal ULIB.
//
// Burst times are in timer ticks. A CPU-bound child with burst b does b
// units of CPU work (one unit ~ one tick under qemu), so the burst it
// declares is an honest estimate. An I/O-bound child alternates a little CPU work
// with sleep(1), so it blocks and re-enters the ready queue repeatedly.

#include "processInfo.h"

#define CPU 0
#define IO  1
#define MAXCHILD 16

struct rec {
  int pid, burst, type, finish, switches;
};

static volatile int sink;

static void
spin(int n)
{
  int i;
  for(i = 0; i < n; i++)
    sink += i;
}

// Loop iterations that take about one timer tick (set by calibrate()).
static int unit = 1000000;

// Measure `unit` by timing a long spin (no polling inside the loop, so
// the measurement isn't disturbed by system calls). Called once by the
// parent before forking. Emulated time is noisy: treat turnaround
// numbers as relative, not absolute.
static void
calibrate(void)
{
  int n = 1000000, t, dt;

  for(;;){
    t = uptime();
    spin(n);
    dt = uptime() - t;
    if(dt >= 20 || n >= (1 << 30))
      break;
    n *= 2;
  }
  unit = n / (dt > 0 ? dt : 1);
}

static void
cpu_work(int units)
{
  int j;
  for(j = 0; j < units; j++)
    spin(unit);
}

static void
io_work(int units)
{
  int j;
  for(j = 0; j < units; j++){
    spin(unit / 4);              // short CPU burst ...
    sleep(1);                    // ... then block, like waiting for a device
  }
}

// Fork n children with the given bursts/types, wait for all of them and
// print one row per child in completion order, plus averages.
// Turnaround is measured from the moment the batch starts.
static void __attribute__((unused))
run_batch(char *title, int n, int *burst, int *type)
{
  int fds[2], i, j, t0, pid, sum = 0, sumcpu = 0, ncpu = 0;
  struct rec r, done[MAXCHILD];
  struct processInfo info;

  printf(1, "\n== %s ==\n", title);
  if(pipe(fds) < 0){
    printf(2, "pipe failed\n");
    exit();
  }
  t0 = uptime();
  for(i = 0; i < n; i++){
    pid = fork();
    if(pid < 0){
      printf(2, "fork failed\n");
      exit();
    }
    if(pid == 0){
      close(fds[0]);
      set_burst_time(burst[i]);
      if(type[i] == CPU)
        cpu_work(burst[i]);
      else
        io_work(burst[i]);
      r.pid = getpid();
      r.burst = get_burst_time();
      r.type = type[i];
      r.finish = uptime() - t0;
      r.switches = getProcInfo(r.pid, &info) == 0 ? info.numberContextSwitches : -1;
      write(fds[1], &r, sizeof(r));
      close(fds[1]);
      exit();
    }
  }
  close(fds[1]);
  for(i = 0; i < n; i++)
    wait();
  for(i = 0; i < n && read(fds[0], &done[i], sizeof(struct rec)) == sizeof(struct rec); i++)
    ;
  close(fds[0]);

  // Sort by finish time (records may reach the pipe slightly out of order).
  for(j = 1; j < i; j++){
    struct rec x = done[j];
    int k = j - 1;
    while(k >= 0 && done[k].finish > x.finish){
      done[k+1] = done[k];
      k--;
    }
    done[k+1] = x;
  }

  printf(1, "order  pid  type  burst  turnaround  switches\n");
  for(j = 0; j < i; j++){
    printf(1, "%d      %d   %s   %d     %d          %d\n", j + 1, done[j].pid,
           done[j].type == CPU ? "CPU" : "IO ", done[j].burst,
           done[j].finish, done[j].switches);
    sum += done[j].finish;
    if(done[j].type == CPU){
      sumcpu += done[j].finish;
      ncpu++;
    }
  }
  printf(1, "avg turnaround (all): %d ticks", i ? sum / i : 0);
  if(ncpu)
    printf(1, ", CPU-bound only: %d ticks", sumcpu / ncpu);
  printf(1, ", batch makespan: %d ticks\n", uptime() - t0);
}
