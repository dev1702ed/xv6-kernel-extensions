// Passed from kernel to user by getProcInfo().
// Starts from the course-provided struct (first three fields) and adds
// a few fields the test programs print.
struct processInfo
{
    int ppid;                    // parent pid (0 if none)
    int psize;                   // process size in bytes
    int numberContextSwitches;   // times scheduled onto a CPU
    int burst_time;              // set by set_burst_time(), 0 if unset
    int killed;
    char name[16];
    char state[16];
};
