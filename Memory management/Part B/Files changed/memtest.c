#include "types.h"
#include "stat.h"
#include "user.h"

// Sanity test for swapping to disk.
//
//   memtest [children] [iterations]      default 20 children x 10 iterations
//
// Each child allocates 4 KB per iteration with malloc, fills it with
// values from a deterministic function of (child, iteration, offset) and
// checks it straight away (as the handout asks). It keeps every buffer,
// and at the end re-checks ALL of them in random order: by then other
// children have competed for memory, so pages that were swapped out and
// back in are verified too. Build with a small PHYSTOP to force paging:
//   make clean && make qemu-nox PHYSTOP=0x300000

#define PGSIZE 4096
#define MAXITER 64

static char
value(int child, int iter, int k)
{
  return (char)((child * 31 + iter * 7 + k * 13 + (k >> 5)) % 251);
}

// xv6's printf writes one character at a time, so lines from different
// children interleave. Build each line first and write it in one call.
static void
say(char *a, int n, char *b)
{
  char buf[96], tmp[12];
  int i = 0, j = 0;

  while(*a) buf[i++] = *a++;
  do { tmp[j++] = '0' + n % 10; n /= 10; } while(n > 0);
  while(j > 0) buf[i++] = tmp[--j];
  while(*b) buf[i++] = *b++;
  write(1, buf, i);
}

static int
check(char *buf, int child, int iter)
{
  int k, bad = 0;

  for(k = 0; k < PGSIZE; k++)
    if(buf[k] != value(child, iter, k))
      bad++;
  return bad;
}

static void
child_process(int child, int iters)
{
  char *ptr[MAXITER];
  int j, k, bad, total = 0;

  for(j = 0; j < iters; j++){
    if((ptr[j] = malloc(PGSIZE)) == 0){
      printf(1, "child %d: malloc failed at iteration %d\n", child, j);
      exit();
    }
    for(k = 0; k < PGSIZE; k++)
      ptr[j][k] = value(child, j, k);
    bad = check(ptr[j], child, j);
    if(bad)
      say("bad bytes right after writing, child ", child, "\n");
    total += bad;
  }

  // Let the other children allocate (and push our pages out) first.
  sleep(20);

  // Re-verify every page, in random order.
  for(k = 0; k < iters * 2; k++){
    j = randomrange(0, iters - 1);
    total += check(ptr[j], child, j);
  }
  for(j = 0; j < iters; j++)
    total += check(ptr[j], child, j);

  say("child ", child, total == 0 ? ": all pages matched after re-check\n"
                                   : ": MISMATCH after re-check\n");
  exit();
}

int
main(int argc, char *argv[])
{
  int i, n = 20, iters = 10;

  if(argc > 1)
    n = atoi(argv[1]);
  if(argc > 2)
    iters = atoi(argv[2]);
  if(iters > MAXITER)
    iters = MAXITER;

  for(i = 0; i < n; i++){
    int pid = fork();
    if(pid < 0){
      printf(1, "fork failed at child %d\n", i);
      break;
    }
    if(pid == 0)
      child_process(i, iters);
  }
  while(wait() != -1)
    ;
  printf(1, "memtest done\n");
  exit();
}
