#include "types.h"
#include "stat.h"
#include "user.h"
#include "schedtest.h"

// 10 children forked with bursts in decreasing bursts.
// Even-numbered children are CPU-bound, odd ones I/O-bound.
int
main(void)
{
  int burst[10] = {100, 90, 80, 70, 60, 50, 40, 30, 20, 10};
  int type[10];
  int i;

  calibrate();
  for(i = 0; i < 10; i++)
    type[i] = (i % 2 == 0) ? CPU : IO;
  run_batch("test_3: decreasing bursts, CPU/IO alternating", 10, burst, type);
  exit();
}
