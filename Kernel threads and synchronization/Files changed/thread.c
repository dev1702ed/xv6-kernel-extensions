// Kernel threads + user-level synchronisation test.
//
//   thread            -> spinlock (default)
//   thread none       -> no lock: shows the lost-update race
//   thread spin       -> thread_spin_lock / thread_spin_unlock
//   thread mutex      -> thread_mutex_lock / thread_mutex_unlock (sleeps instead of spinning)
//   thread sem        -> binary semaphore built on the mutex
//
// Two threads add 3200 and 2800 to a shared counter, so the correct
// final balance is 6000.

#include "types.h"
#include "stat.h"
#include "user.h"
#include "x86.h"

#define NONE  0
#define SPIN  1
#define MUTEX 2
#define SEM   3

// ---------------------------------------------------------------- spinlock
// Same algorithm as the kernel's acquire()/release(), minus pushcli()
// (disabling interrupts is privileged and not needed in user space).
struct thread_spinlock {
  volatile uint locked;
};

void
thread_spin_init(struct thread_spinlock *lk)
{
  lk->locked = 0;
}

void
thread_spin_lock(struct thread_spinlock *lk)
{
  // xchg is atomic on x86: exactly one thread sees the old value 0.
  while(xchg(&lk->locked, 1) != 0)
    ;
  // Keep the compiler and CPU from moving critical-section accesses
  // above the point where the lock is taken.
  __sync_synchronize();
}

void
thread_spin_unlock(struct thread_spinlock *lk)
{
  __sync_synchronize();
  // Plain store is enough to release on x86, but use asm so the
  // compiler cannot split or reorder it.
  asm volatile("movl $0, %0" : "+m" (lk->locked) : );
}

// ---------------------------------------------------------------- mutex
// Same test-and-set, but a waiter gives up the CPU instead of spinning.
// xv6 has no user-level yield(), so sleep for one timer tick.
struct thread_mutex {
  volatile uint locked;
};

void
thread_mutex_init(struct thread_mutex *m)
{
  m->locked = 0;
}

void
thread_mutex_lock(struct thread_mutex *m)
{
  while(xchg(&m->locked, 1) != 0)
    sleep(1);
  __sync_synchronize();
}

void
thread_mutex_unlock(struct thread_mutex *m)
{
  __sync_synchronize();
  asm volatile("movl $0, %0" : "+m" (m->locked) : );
}

// ---------------------------------------------------------------- semaphore
// Counting semaphore: the mutex protects the count; waiters back off
// one tick at a time while the count is zero.
struct thread_sem {
  struct thread_mutex m;
  volatile int count;
};

void
thread_sem_init(struct thread_sem *s, int n)
{
  thread_mutex_init(&s->m);
  s->count = n;
}

void
thread_sem_wait(struct thread_sem *s)
{
  for(;;){
    thread_mutex_lock(&s->m);
    if(s->count > 0){
      s->count--;
      thread_mutex_unlock(&s->m);
      return;
    }
    thread_mutex_unlock(&s->m);
    sleep(1);
  }
}

void
thread_sem_post(struct thread_sem *s)
{
  thread_mutex_lock(&s->m);
  s->count++;
  thread_mutex_unlock(&s->m);
}

// ---------------------------------------------------------------- test
struct balance {
  char name[32];
  int amount;
};

volatile int total_balance = 0;
int mode = SPIN;
struct thread_spinlock lock;
struct thread_mutex mtx;
struct thread_sem sem;

volatile unsigned int
delay(unsigned int d)
{
  unsigned int i;
  for(i = 0; i < d; i++)
    __asm volatile("nop" ::: );
  return i;
}

static void
enter(void)
{
  if(mode == SPIN)       thread_spin_lock(&lock);
  else if(mode == MUTEX) thread_mutex_lock(&mtx);
  else if(mode == SEM)   thread_sem_wait(&sem);
}

static void
leave(void)
{
  if(mode == SPIN)       thread_spin_unlock(&lock);
  else if(mode == MUTEX) thread_mutex_unlock(&mtx);
  else if(mode == SEM)   thread_sem_post(&sem);
}

void
do_work(void *arg)
{
  int i, old;
  struct balance *b = (struct balance*) arg;

  printf(1, "Starting do_work: s:%s\n", b->name);
  for(i = 0; i < b->amount; i++){
    enter();
    old = total_balance;   // read ...
    delay(100000);         // ... widen the window ...
    total_balance = old + 1;  // ... write: lost update without a lock
    leave();
  }
  printf(1, "Done s:%s\n", b->name);
  thread_exit();
}

int
main(int argc, char *argv[])
{
  struct balance b1 = {"b1", 3200};
  struct balance b2 = {"b2", 2800};
  char *names[] = { "none", "spin", "mutex", "sem" };
  void *s1, *s2;
  int t1, t2, r1, r2, i;

  if(argc > 1){
    mode = -1;
    for(i = 0; i < 4; i++)
      if(strcmp(argv[1], names[i]) == 0)
        mode = i;
    if(mode < 0){
      printf(2, "usage: thread [none|spin|mutex|sem]\n");
      exit();
    }
  }
  printf(1, "mode: %s\n", names[mode]);

  thread_spin_init(&lock);
  thread_mutex_init(&mtx);
  thread_sem_init(&sem, 1);

  s1 = malloc(4096);
  s2 = malloc(4096);

  t1 = thread_create(do_work, (void*)&b1, s1);
  t2 = thread_create(do_work, (void*)&b2, s2);
  if(t1 < 0 || t2 < 0){
    printf(2, "thread_create failed\n");
    exit();
  }

  r1 = thread_join();
  r2 = thread_join();

  printf(1, "Threads finished: (%d):%d, (%d):%d, shared balance:%d (expected %d)\n",
         t1, r1, t2, r2, total_balance, b1.amount + b2.amount);

  free(s1);
  free(s2);
  exit();
}
