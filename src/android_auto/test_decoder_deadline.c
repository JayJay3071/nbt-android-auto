/* VM-only supervisor for the copied Cinemo null-renderer experiment.
 * Never invoke on the car. Signals target only our own unreaped child group.
 */
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
static long long now_ms(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC,&t)) return -1;
    return (long long)t.tv_sec*1000+t.tv_nsec/1000000;
}
static int supervise(int test) {
    pid_t child;
    int status, killed=0, rc;
    long long deadline=now_ms();
    if (deadline<0) return 125;
    child=fork();
    if (child<0) return 125;
    if (child==0) {
        struct rlimit core;
        memset(&core,0,sizeof(core)); core.rlim_cur=0; core.rlim_max=0;
        if (setpgid(0,0) || setrlimit(RLIMIT_CORE,&core)) _exit(125);
        if (test==1) { signal(SIGTERM,SIG_IGN); for (;;) pause(); }
        if (test==2) {
            if (setenv("LD_PRELOAD","./foreground.so",1)) _exit(125);
            execl("./io-nme","io-nme","-p",".","-c","-vv",(char*)0);
            _exit(127);
        }
        if (test==5) {
            execl("./NmeCmdLine","NmeCmdLine","-n","-a0","-wd","0","-wi","0","-wm","0","-i","-ic","-dv","http-frames.raw","http://127.0.0.1:18765/pattern.h264",(char*)0);
            _exit(127);
        }
        if (test==3 || test==4) {
            execl("./NmeCmdLine","NmeCmdLine","-n","-a0","-wd","0","-wi","0","-wm","0","-i","-ic","-dv",test==3 ? "annexb-frames.raw" : "mp4-frames.raw",test==3 ? "pattern-800x480.h264" : "pattern-800x480.mp4",(char*)0);
            _exit(127);
        }
        execl("./NmeCmdLine","NmeCmdLine","-n","-a0","-wd","0","-wi","0","-wm","0","-i","pattern-800x480.mp4",(char*)0);
        _exit(127);
    }
    /* Child establishes its own group before exec; race-safe parent assist. */
    if (setpgid(child,child) && errno!=EACCES && errno!=ESRCH) {
        kill(child,SIGKILL); waitpid(child,&status,0); return 125;
    }
    deadline+=test==1 ? 200 : 15000;
    for (;;) {
        rc=waitpid(child,&status,WNOHANG);
        if (rc==child) break;
        if (rc<0 && errno!=EINTR) return 125;
        long long time=now_ms();
        if (time<0 || time>=deadline) {
            if (!killed) {
                kill(-child,SIGTERM); killed=1; deadline=time+(test==1 ? 100 : 1000);
            } else {
                kill(-child,SIGKILL);
                do { rc=waitpid(child,&status,0); } while (rc<0 && errno==EINTR);
                return rc==child ? 124 : 125;
            }
        }
        usleep(10000);
    }
    if (killed) return 124;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128+WTERMSIG(status);
}
int main(int argc,char**argv) {
    int result;
    if (geteuid()==0 && (argc!=2 || strcmp(argv[1],"--vm-service-start"))) { puts("VM_DECODER_TEST_REFUSED_ROOT"); return 2; }
    if (argc!=2 || (strcmp(argv[1],"--selftest") && strcmp(argv[1],"--vm-null-decode") && strcmp(argv[1],"--vm-service-start") && strcmp(argv[1],"--vm-annexb-dump") && strcmp(argv[1],"--vm-mp4-dump") && strcmp(argv[1],"--vm-http-dump"))) return 2;
    if (!strcmp(argv[1],"--selftest")) {
        result=supervise(1);
        if (result!=124) return 1;
        puts("DECODER_DEADLINE_OK own_child_terminated_and_reaped=1"); return 0;
    }
    if (!strcmp(argv[1],"--vm-service-start")) {
        if ((geteuid()!=100 && geteuid()!=0) || access("/dev/io-nme",F_OK)==0 || access("/home/nbtbuild/.nbt-build/aa-cinemo-20261106-0155/foreground.so",R_OK)) return 2;
        result=supervise(2);
        printf("CINEMO_SERVICE_CHILD_EXIT=%d\n",result); return result;
    }
    /* NmeOpen in the original libNme is a client of this resource manager.
     * Copied decoder libraries alone cannot run NmeCmdLine in a clean VM.
     * Do not open the node here: this is only a prerequisite check.
     */
    if (access(getenv("NBT_VM_CINEMO_NODE") ? getenv("NBT_VM_CINEMO_NODE") : "/dev/io-nme",F_OK)) {
        puts("DECODER_PREREQUISITE_MISSING /dev/io-nme; isolated VM Cinemo service required");
        return 3;
    }
    /* Refuse to overwrite previous pixel evidence. */
    if ((!strcmp(argv[1],"--vm-annexb-dump") && access("annexb-frames.raw",F_OK)==0) ||
        (!strcmp(argv[1],"--vm-mp4-dump") && access("mp4-frames.raw",F_OK)==0) ||
        (!strcmp(argv[1],"--vm-http-dump") && access("http-frames.raw",F_OK)==0)) return 2;
    result=supervise(!strcmp(argv[1],"--vm-http-dump") ? 5 : !strcmp(argv[1],"--vm-annexb-dump") ? 3 : !strcmp(argv[1],"--vm-mp4-dump") ? 4 : 0);
    printf("DECODER_CHILD_EXIT=%d\n",result); return result;
}

