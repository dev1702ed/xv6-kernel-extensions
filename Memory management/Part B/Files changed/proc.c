#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "stat.h"
#include "fs.h"
#include "file.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;
static struct proc *allocproc(void);

// ---------------------------------------------------------------- swapping
//
// Two kernel processes move user pages between memory and swap files:
//   swapoutd  frees a physical page when an allocation fails (queue1)
//   swapind   reads a swapped-out page back on a page fault (queue2)
//
// Locking rules
//   queue1.lock / queue2.lock  protect the request queues; waiters sleep
//                              on them, so a wakeup can't be lost.
//   swapio (sleeplock)         serialises all swap-file I/O and every
//                              PTE_P <-> PTE_SWAP transition on user pages.
//   ptable.lock                held while choosing a victim, so processes
//                              can't exit and free their page tables
//                              under us.
// No spinlock is ever held across file I/O (which sleeps).
//
// One CPU (NCPU = 1) is assumed: a victim process is never running while
// its PTE is changed, and switchuvm() reloads CR3 (flushing its TLB)
// before it runs again.
//
// A swapped-out PTE has P clear and PTE_SWAP set; its W/U bits are kept.
// The page is stored in "/<pid>_<vpn>.swp" (vpn = VA >> 12, in hex),
// i.e. the 20 most significant bits of the VA, as the handout asks.
// The name is at most 13 characters (DIRSIZ is 14).

struct req_queue {
  struct spinlock lock;
  struct proc *queue[NPROC];
  int s, e;                  // circular: pop at s, push at e
};

struct req_queue queue1;     // processes waiting for a free page
struct req_queue queue2;     // processes waiting for a page to be swapped in
struct sleeplock swapio;
static struct proc *swapoutd;
static int nswapout, nswapin;   // statistics, shown by ^P (procdump)
static int nswapped;            // pages currently on disk (guarded by swapio)

// Swap space is the root file system (FSSIZE blocks, NINODES inodes in
// mkfs.c). Each swapped page needs 8 blocks and an inode; stop well short
// of either limit. When swap is full, allocations fail as in vanilla xv6.
#define MAXSWAP 700

// Caller holds q->lock. Returns 0 if the queue is full.
static int
rpush(struct req_queue *q, struct proc *p)
{
  if((q->e + 1) % NPROC == q->s)
    return 0;
  q->queue[q->e] = p;
  q->e = (q->e + 1) % NPROC;
  return 1;
}

// Caller holds q->lock. Returns 0 if the queue is empty.
static struct proc*
rpop(struct req_queue *q)
{
  struct proc *p;

  if(q->s == q->e)
    return 0;
  p = q->queue[q->s];
  q->s = (q->s + 1) % NPROC;
  return p;
}

// ------------------------------------------------ swap files (inode level)

static void
swap_name(char *buf, int pid, uint va)
{
  static char hex[] = "0123456789abcdef";
  char tmp[16];
  uint vpn = va >> PTXSHIFT;
  int i = 0, n = 0;

  buf[n++] = '/';
  do { tmp[i++] = '0' + pid % 10; pid /= 10; } while(pid > 0);
  while(i > 0) buf[n++] = tmp[--i];
  buf[n++] = '_';
  do { tmp[i++] = hex[vpn % 16]; vpn /= 16; } while(vpn > 0);
  while(i > 0) buf[n++] = tmp[--i];
  safestrcpy(buf + n, ".swp", 5);
}

// Create (or truncate-in-place) a regular file. Like create() in sysfile.c.
// Returns a locked inode. Caller is inside begin_op()/end_op().
static struct inode*
swap_create(char *path)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if((dp = nameiparent(path, name)) == 0)
    return 0;
  ilock(dp);
  if((ip = dirlookup(dp, name, 0)) != 0){
    iunlockput(dp);
    ilock(ip);
    return ip;
  }
  if((ip = ialloc(dp->dev, T_FILE)) == 0)
    panic("swap_create: ialloc");
  ilock(ip);
  ip->major = 0;
  ip->minor = 0;
  ip->nlink = 1;
  iupdate(ip);
  if(dirlink(dp, name, ip->inum) < 0)
    panic("swap_create: dirlink");
  iunlockput(dp);
  return ip;
}

// Write one page to the swap file of (pid, va). Caller holds swapio.
static int
swap_write_page(int pid, uint va, char *page)
{
  char path[32];
  struct inode *ip;
  int off, n, max = ((MAXOPBLOCKS-1-1-2) / 2) * BSIZE;   // as in filewrite()

  swap_name(path, pid, va);
  begin_op();
  ip = swap_create(path);
  if(ip == 0){
    end_op();
    return -1;
  }
  iunlock(ip);
  end_op();

  // Write in pieces small enough for one log transaction each.
  for(off = 0; off < PGSIZE; off += n){
    n = PGSIZE - off < max ? PGSIZE - off : max;
    begin_op();
    ilock(ip);
    if(writei(ip, page + off, off, n) != n){
      iunlock(ip);
      end_op();
      iput(ip);
      return -1;
    }
    iunlock(ip);
    end_op();
  }
  begin_op();
  iput(ip);
  end_op();
  return 0;
}

// Read the swap file of (pid, va) into page. Caller holds swapio.
int
swap_read_page(int pid, uint va, char *page)
{
  char path[32];
  struct inode *ip;
  int r;

  swap_name(path, pid, va);
  begin_op();
  if((ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  r = readi(ip, page, 0, PGSIZE);
  iunlockput(ip);
  end_op();
  return r == PGSIZE ? 0 : -1;
}

// Remove the swap file of (pid, va). Like sys_unlink(). Caller holds swapio.
static void
swap_unlink(int pid, uint va)
{
  char path[32], name[DIRSIZ];
  struct inode *ip, *dp;
  struct dirent de;
  uint off;

  swap_name(path, pid, va);
  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return;
  }
  ilock(dp);
  if((ip = dirlookup(dp, name, &off)) == 0){
    iunlockput(dp);
    end_op();
    return;
  }
  ilock(ip);
  memset(&de, 0, sizeof(de));
  if(writei(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
    panic("swap_unlink: writei");
  iunlockput(dp);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
}

// Delete the swap files for every swapped-out page of pgdir in
// [start, end), and clear those PTEs. Used by exit, exec and sbrk(-n).
void
swap_free_range(int pid, pde_t *pgdir, uint start, uint end)
{
  uint va;
  pte_t *pte;

  acquiresleep(&swapio);
  for(va = PGROUNDUP(start); va < end; va += PGSIZE){
    if((pte = walkpgdir(pgdir, (char*)va, 0)) != 0 && (*pte & PTE_SWAP)){
      swap_unlink(pid, va);
      *pte = 0;
      nswapped--;
    }
  }
  releasesleep(&swapio);
}

// ------------------------------------------------ swapping out

// Suspend the calling process (SLEEPING) and queue a request for the
// swap-out daemon; return when it has freed a page. Returns -1 if it
// could not free anything (or we were killed).
static int
wait_for_page(struct proc *p)
{
  int failed;

  if(p == 0 || p == swapoutd)        // boot code / the swapper itself
    return -1;
  acquire(&queue1.lock);
  if(!rpush(&queue1, p)){
    release(&queue1.lock);
    return -1;
  }
  wakeup(&queue1);                   // swapoutd sleeps on &queue1
  sleep(p, &queue1.lock);
  failed = p->swapfail;
  p->swapfail = 0;
  release(&queue1.lock);
  return (failed || p->killed) ? -1 : 0;
}

// Allocate a page for user memory, swapping something out if memory is
// exhausted. Returns 0 only if nothing could be swapped out.
char*
kalloc_user(void)
{
  char *mem;

  for(;;){
    if((mem = kalloc()) != 0)
      return mem;
    if(wait_for_page(myproc()) < 0)
      return 0;
  }
}

// Wait until at least n physical pages are free, swapping pages out if
// necessary. Used before allocations that can't simply call kalloc_user():
// page tables (setupkvm, mappages) and kernel stacks. Pages aren't held
// while waiting, so waiters can't starve each other; since the kernel is
// preemptible, callers retry if someone else took the pages first.
int
kreserve(int n)
{
  while(kfreecount() < n)
    if(wait_for_page(myproc()) < 0)
      return -1;
  return 0;
}

static int clock_proc, clock_va;     // second-chance clock hand

// Is p's memory a candidate for eviction? Caller holds ptable.lock.
static int
evictable(struct proc *p)
{
  return !p->kproc && p->pgdir != 0 &&
         (p->state == RUNNABLE || p->state == SLEEPING);
}

// Choose a victim with the second-chance (clock) approximation of LRU:
// sweep user pages of all processes; a page whose Accessed bit is set
// gets the bit cleared and is skipped this time; the first page found
// with A clear is the victim. At most two full sweeps.
// Caller holds swapio and ptable.lock. Marks the PTE swapped and returns
// the physical frame (still allocated), or 0 if there is nothing to evict.
static uint
choose_victim(int *pidp, uint *vap, pte_t **ptep)
{
  int sweeps, scanned, nproc = NPROC;
  struct proc *p;
  pte_t *pte;
  uint pa;

  for(sweeps = 0; sweeps < 2; sweeps++){
    for(scanned = 0; scanned <= nproc; scanned++){
      p = &ptable.proc[clock_proc];
      if(evictable(p)){
        for(; clock_va < p->sz; clock_va += PGSIZE){
          pte = walkpgdir(p->pgdir, (char*)clock_va, 0);
          if(pte == 0 || !(*pte & PTE_P) || !(*pte & PTE_U))
            continue;
          if(*pte & PTE_A){
            *pte &= ~PTE_A;          // second chance
            continue;
          }
          pa = PTE_ADDR(*pte);
          *pidp = p->pid;
          *vap = clock_va;
          *ptep = pte;
          *pte = (*pte & ~(PTE_P | PTE_A | PTE_D)) | PTE_SWAP;
          clock_va += PGSIZE;
          return pa;
        }
      }
      clock_proc = (clock_proc + 1) % NPROC;
      clock_va = 0;
    }
  }
  return 0;
}

// Evict one page. Returns 1 if a page was freed.
static int
swap_out_one(void)
{
  int pid;
  uint va, pa;
  pte_t *pte;

  acquiresleep(&swapio);
  if(nswapped >= MAXSWAP){           // swap space full
    releasesleep(&swapio);
    return 0;
  }
  acquire(&ptable.lock);
  pa = choose_victim(&pid, &va, &pte);
  release(&ptable.lock);
  if(pa == 0){
    releasesleep(&swapio);
    return 0;
  }
  // The PTE already says "swapped", so a fault on it will queue a
  // swap-in, which can't proceed until we release swapio below.
  if(swap_write_page(pid, va, (char*)P2V(pa)) < 0){
    // Disk full: undo. The victim can't have exited meanwhile (exit
    // needs swapio to clean up), so its PTE is still valid.
    swap_unlink(pid, va);
    *pte = pa | (PTE_FLAGS(*pte) & ~PTE_SWAP) | PTE_P;
    releasesleep(&swapio);
    return 0;
  }
  nswapout++;
  nswapped++;
  releasesleep(&swapio);
  kfree((char*)P2V(pa));
  return 1;
}

// Entry point of the swap-out kernel process.
void
swap_out_process_function(void)
{
  struct proc *p;
  int freed;

  for(;;){
    acquire(&queue1.lock);
    while(queue1.s == queue1.e)
      sleep(&queue1, &queue1.lock);   // no requests: suspended
    release(&queue1.lock);

    // Evict one page per request round, even if a few pages are already
    // free: a waiter may need several (kreserve), and "some page is free"
    // alone could leave it waiting forever. Only report failure if
    // nothing could be evicted and nothing is free.
    freed = swap_out_one() || kfreecount() > 0;

    // A page is free: wake every process that was waiting for memory
    // (they retry kalloc). If nothing could be evicted, tell them to fail.
    acquire(&queue1.lock);
    while((p = rpop(&queue1)) != 0){
      if(!freed)
        p->swapfail = 1;
      wakeup(p);
    }
    release(&queue1.lock);
  }
}

// ------------------------------------------------ swapping in

// Called by trap() on a page fault at addr. If the page was swapped
// out, queue a swap-in request, sleep until the page is mapped again and
// return 0. Returns -1 for any other fault.
int
swap_in_request(uint addr)
{
  struct proc *p = myproc();
  pte_t *pte;

  if(p == 0 || p->kproc || addr >= KERNBASE)
    return -1;
  if((pte = walkpgdir(p->pgdir, (char*)addr, 0)) == 0 || !(*pte & PTE_SWAP))
    return -1;
  if(mycpu()->ncli > 0)
    panic("page fault on a swapped page while holding a spinlock");

  acquire(&queue2.lock);
  p->addr = PGROUNDDOWN(addr);
  if(!rpush(&queue2, p))
    panic("swap-in queue full");
  wakeup(&queue2);                   // swapind sleeps on &queue2
  while(!(*pte & PTE_P))
    sleep(p, &queue2.lock);
  release(&queue2.lock);
  return 0;
}

// Entry point of the swap-in kernel process.
void
swap_in_process_function(void)
{
  struct proc *p;
  pte_t *pte;
  char *mem;
  uint va;
  int pid;

  for(;;){
    acquire(&queue2.lock);
    while((p = rpop(&queue2)) == 0)
      sleep(&queue2, &queue2.lock);   // no requests: suspended
    pid = p->pid;
    va = p->addr;
    release(&queue2.lock);

    // Get a frame before taking swapio: if memory is short this waits
    // for swapoutd, which itself needs swapio.
    while((mem = kalloc_user()) == 0)
      ;

    // p cannot exit while it waits for this page (it loops in
    // swap_in_request until the page is present), so p->pgdir is stable.
    acquiresleep(&swapio);
    pte = walkpgdir(p->pgdir, (char*)va, 0);
    if(p->pid != pid || pte == 0 || !(*pte & PTE_SWAP))
      panic("swap_in: bad request");
    if(swap_read_page(pid, va, mem) < 0)
      panic("swap_in: read");
    swap_unlink(pid, va);
    *pte = V2P(mem) | (PTE_FLAGS(*pte) & ~PTE_SWAP) | PTE_P;
    nswapin++;
    nswapped--;
    releasesleep(&swapio);

    acquire(&queue2.lock);
    wakeup(p);
    release(&queue2.lock);
  }
}

// ------------------------------------------------ kernel processes

// Create a process that lives entirely in kernel mode, starting at
// entrypoint. It gets a kernel-only page table and the root directory as
// cwd (it opens swap files), and is made RUNNABLE.
void
create_kernel_process(const char *name, void (*entrypoint)(void))
{
  struct proc *p;

  if((p = allocproc()) == 0)
    panic("create_kernel_process: allocproc");
  if((p->pgdir = setupkvm()) == 0)
    panic("create_kernel_process: setupkvm");
  p->sz = 0;
  p->kproc = 1;
  p->kentry = entrypoint;
  p->parent = initproc;
  p->cwd = namei("/");
  safestrcpy(p->name, name, sizeof(p->name));
  if(entrypoint == swap_out_process_function)
    swapoutd = p;

  acquire(&ptable.lock);
  p->state = RUNNABLE;
  release(&ptable.lock);
}

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
  initlock(&queue1.lock, "swapout queue");
  initlock(&queue2.lock, "swapin queue");
  initsleeplock(&swapio, "swapio");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;
  p->kproc = 0;
  p->kentry = 0;
  p->addr = 0;
  p->swapfail = 0;

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n < 0)
    swap_free_range(curproc->pid, curproc->pgdir, sz + n, sz);
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process. If that failed for lack of memory (not of
  // process slots), make room - possibly by swapping - and retry.
  while((np = allocproc()) == 0)
    if(kfreecount() > 0 || kreserve(1) < 0)
      return -1;

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  pde_t *pgdir;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  // Remove this process's swap files. Nothing below sleeps, so the
  // swapper cannot evict another of our pages before we become a ZOMBIE
  // (zombies are never chosen as victims).
  swap_free_range(curproc->pid, curproc->pgdir, 0, curproc->sz);

  // Free the whole address space now rather than in the parent's wait():
  // under memory pressure a parent may not reap for a long time (memtest
  // forks all its children before waiting), and a zombie's memory can't
  // be swapped out. Switch to the kernel-only page table first so we are
  // not running on the one being freed, and keep interrupts off until
  // sched() so we can't be rescheduled with p->pgdir == 0.
  pushcli();
  switchkvm();
  pgdir = curproc->pgdir;
  curproc->pgdir = 0;
  curproc->sz = 0;
  freevm(pgdir);

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  popcli();             // ptable.lock now keeps interrupts off

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        if(p->pgdir)          // normally already freed by exit()
          freevm(p->pgdir);
        p->pgdir = 0;
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;
  
  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE)
        continue;

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;

      swtch(&(c->scheduler), p->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }
    release(&ptable.lock);

  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // A kernel process never returns to user space: run its entry point.
  if(myproc()->kproc){
    myproc()->kentry();
    panic("kernel process returned");
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  cprintf("swap: %d pages out, %d pages in, %d pages free\n",
          nswapout, nswapin, kfreecount());

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}
