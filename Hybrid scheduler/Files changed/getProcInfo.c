#include "types.h"
#include "stat.h"
#include "user.h"
#include "processInfo.h"

// usage: getProcInfo pid [pid ...]
int
main(int argc, char *argv[])
{
  struct processInfo info;
  int i, pid;

  if(argc < 2){
    printf(2, "usage: getProcInfo pid [pid ...]\n");
    exit();
  }
  for(i = 1; i < argc; i++){
    pid = atoi(argv[i]);
    if(getProcInfo(pid, &info) < 0){
      printf(2, "pid %d: no such process\n", pid);
      continue;
    }
    printf(1, "pid %d: name=%s ppid=%d size=%d switches=%d state=%s killed=%d burst=%d\n",
           pid, info.name, info.ppid, info.psize, info.numberContextSwitches,
           info.state, info.killed, info.burst_time);
  }
  exit();
}
