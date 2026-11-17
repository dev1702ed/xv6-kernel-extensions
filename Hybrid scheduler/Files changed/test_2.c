#include "types.h"
#include "stat.h"
#include "user.h"
#include "schedtest.h"

// 10 children forked with bursts in increasing bursts.
// Even-numbered children are CPU-bound, odd ones I/O-bound.
int
main(void)
{
  int burst[10] = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
  int type[10];
  int i;

  calibrate();
  for(i = 0; i < 10; i++)
    type[i] = (i % 2 == 0) ? CPU : IO;
  run_batch("test_2: increasing bursts, CPU/IO alternating", 10, burst, type);
  exit();
}
