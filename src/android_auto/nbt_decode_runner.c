/* Own-child supervisor for the software/no-display Cinemo client only.
 * Does not start, stop, or modify the stock io-nme service.
 */
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static volatile sig_atomic_t stop;
static void stopped(int s){(void)s;stop=1;}
static long long ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (long long)t.tv_sec*1000+t.tv_nsec/1000000;}
int main(int argc,char**argv){
 pid_t child;int status,stage=0;long seconds;char*end;long long deadline;
 if(argc!=5)return 2;
 seconds=strtol(argv[1],&end,10);if(*end||seconds<0||seconds>3600)return 2;
 signal(SIGTERM,stopped);signal(SIGINT,stopped);signal(SIGPIPE,SIG_IGN);
 child=fork();if(child<0)return 2;
 if(!child){struct rlimit core;
  /* Software decoding on purpose (car test 2026-11-10): hardware decoding (-wd 1) gave a picture, but the io-nme video
   * queue grew to 0.85 MB (seconds of delay; software keeps it at 0.004 MB), and a playback speed above normal (-s 1200)
   * made NmeCmdLine stop after the first frame of the live stream. */  memset(&core,0,sizeof(core));if(setrlimit(RLIMIT_CORE,&core)||setpgid(0,0))_exit(125);
  /* argv[4] == "screen": hardware video path (car 2026-11-07): render into the stock video window class of Cinemo
   * output 1 (CinemoVideo1_TaskA/B, made visible by nbt_hwvideo) instead of raw I420 frames into a FIFO */
  /* "screen720": phone canvas 1280x720 with content rows 0..479 (insets 0,240,0,0) -> crop 667/1000 */
  if(!strcmp(argv[4],"screen720"))
   execl(argv[2],"NmeCmdLine","-a0","-wd","0","-wi","0","-wm","0","-vd","NmeDeviceScreen","-vi",
    "?video-output-id=1&video-output-aspect-x=1&video-output-aspect-y=1&video-window-class=CinemoVideo1_TaskA,CinemoVideo1_TaskB&video-subpicture-window-class=CinemoSubpicture1_TaskA,CinemoSubpicture1_TaskB",
    "-vf","-vr","0,0,1000,667","-va","3","-x","800","-y","480","-xx","0","-yy","0","-i","-ic",argv[3],(char*)0);
  /* "screencenter": whole 800x480 phone canvas 1:1 (no crop); nbt_hwvideo places the window in the middle of the display */
  else if(!strcmp(argv[4],"screencenter"))
   execl(argv[2],"NmeCmdLine","-a0","-wd","0","-wi","0","-wm","0","-vd","NmeDeviceScreen","-vi",
    "?video-output-id=1&video-output-aspect-x=1&video-output-aspect-y=1&video-window-class=CinemoVideo1_TaskA,CinemoVideo1_TaskB&video-subpicture-window-class=CinemoSubpicture1_TaskA,CinemoSubpicture1_TaskB",
    "-vf","-va","3","-x","800","-y","480","-xx","0","-yy","0","-i","-ic",argv[3],(char*)0);
  else if(!strcmp(argv[4],"screen"))
   execl(argv[2],"NmeCmdLine","-a0","-wd","0","-wi","0","-wm","0","-vd","NmeDeviceScreen","-vi",
    "?video-output-id=1&video-output-aspect-x=1&video-output-aspect-y=1&video-window-class=CinemoVideo1_TaskA,CinemoVideo1_TaskB&video-subpicture-window-class=CinemoSubpicture1_TaskA,CinemoSubpicture1_TaskB",
    /* -vf: no frame dropping (car 2026-11-07: the live stream has no usable timestamps, the renderer dropped 992 of
     * 1204 frames and the picture froze); -vr 0,0,1000,625 + -va 3: only the phone's content rows 0..299 of 480
     * (insets 0,180,0,0), stretched to the window */
    /* -x/-y/-xx/-yy: window 800x480 at 0,0 (with only -vr the window took the cropped size 800x300: lower part
     * black, picture squashed, car 2026-11-07) */
    "-vf","-vr","0,0,1000,625","-va","3","-x","800","-y","480","-xx","0","-yy","0","-i","-ic",argv[3],(char*)0);
  else execl(argv[2],"NmeCmdLine","-n","-a0","-wd","0","-wi","0","-wm","0","-i","-ic","-dv",argv[4],argv[3],(char*)0);
  _exit(127);
 }
 if(setpgid(child,child)&&errno!=EACCES&&errno!=ESRCH){kill(child,SIGKILL);waitpid(child,&status,0);return 125;}
 deadline=seconds?ms()+seconds*1000LL:0;
 for(;;){pid_t r=waitpid(child,&status,WNOHANG);if(r==child)break;if(r<0&&errno!=EINTR)return 125;
  if(stop||(deadline&&ms()>=deadline)){
   /* 4 s for a clean tear-down of the client's graph inside the shared io-nme (car 2026-11-07: after abrupt session
    * ends the next session showed no picture until a reboot; suspected leftover graph/window in io-nme) */
   if(!stage){kill(-child,SIGTERM);stage=1;deadline=ms()+4000;stop=0;}
   else{kill(-child,SIGKILL);while(waitpid(child,&status,0)<0&&errno==EINTR){};stage=2;break;}
  }
  usleep(10000);
 }
 printf("DECODE_CLIENT_END forced=%d status=%d\n",stage,status);
 return stage?124:WIFEXITED(status)?WEXITSTATUS(status):128+WTERMSIG(status);
}
