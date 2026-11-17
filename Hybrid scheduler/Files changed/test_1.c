#include "types.h"
#include "stat.h"
#include "user.h"
#include "schedtest.h"

// 10 children forked with bursts in random order.
// Even-numbered children are CPU-bound, odd ones I/O-bound.
int
main(void)
{
  int burst[10] = {40, 70, 10, 90, 60, 30, 20, 80, 100, 50};
  int type[10];
  int i;

  calibrate();
  for(i = 0; i < 10; i++)
    type[i] = (i % 2 == 0) ? CPU : IO;
  run_batch("test_1: random order, CPU/IO alternating", 10, burst, type);
  exit();
}
