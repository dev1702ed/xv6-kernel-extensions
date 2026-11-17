#include "types.h"
#include "stat.h"
#include "user.h"

// usage: setBurstTime n
// Sets this process's burst time and reads it back (tests the
// set_burst_time / get_burst_time pair).
int
main(int argc, char *argv[])
{
  int n;

  if(argc < 2){
    printf(2, "usage: setBurstTime n\n");
    exit();
  }
  n = atoi(argv[1]);
  if(set_burst_time(n) < 0){
    printf(2, "set_burst_time(%d) failed (must be > 0)\n", n);
    exit();
  }
  printf(1, "burst time set to %d, get_burst_time() = %d\n", n, get_burst_time());
  exit();
}
