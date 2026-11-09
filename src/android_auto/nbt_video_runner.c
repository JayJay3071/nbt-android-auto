/* Bounded runner for ONE own stock-Cinemo client (NmeCmdLine) with caller-chosen arguments.
 * Purpose: native video-device experiments (-vd/-vi/-vl ...) with a hard time limit and a clean stop.
 *
 *   video_runner <seconds 1..3600, 0 = until SIGTERM> <path-to-NmeCmdLine> [NmeCmdLine arguments...]
 *
 * It only forks/execs the given client in its own process group. It never touches the stock io-nme or
 * HmiMain3D processes. Stop = SIGTERM to the client's group, 2 s grace, then SIGKILL (reported as forced).
 * Core dumps are disabled for the child so an experiment cannot fill the RAM disk.
 */
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_CLIENT_ARGS 48

static volatile sig_atomic_t stop;
static void stopped(int s) { (void)s; stop = 1; }
static long long ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

int main(int argc, char** argv) {
  pid_t child;
  int status = 0, stage = 0, i, n = 0;
  long seconds;
  char* end;
  long long deadline;
  char* args[MAX_CLIENT_ARGS + 2];

  if (argc < 3 || argc - 2 > MAX_CLIENT_ARGS) {
    fprintf(stderr, "usage: video_runner <seconds> <client> [args...]\n");
    return 2;
  }
  seconds = strtol(argv[1], &end, 10);
  if (*end || seconds < 0 || seconds > 3600) return 2;
  if (access(argv[2], X_OK)) { fprintf(stderr, "client not executable: %s\n", argv[2]); return 2; }

  signal(SIGTERM, stopped);
  signal(SIGINT, stopped);
  signal(SIGPIPE, SIG_IGN);

  args[n++] = (char*)"NmeCmdLine";
  for (i = 3; i < argc; ++i) args[n++] = argv[i];
  args[n] = 0;

  child = fork();
  if (child < 0) return 2;
  if (!child) {
    struct rlimit core;
    int fd;
    memset(&core, 0, sizeof(core));
    if (setrlimit(RLIMIT_CORE, &core) || setpgid(0, 0)) _exit(125);
    fd = open("/dev/null", O_RDONLY);
    if (fd >= 0) { dup2(fd, 0); if (fd > 2) close(fd); }
    execv(argv[2], args);
    _exit(127);
  }
  if (setpgid(child, child) && errno != EACCES && errno != ESRCH) {
    kill(child, SIGKILL);
    waitpid(child, &status, 0);
    return 125;
  }
  deadline = seconds ? ms() + seconds * 1000LL : 0;
  for (;;) {
    pid_t r = waitpid(child, &status, WNOHANG);
    if (r == child) break;
    if (r < 0 && errno != EINTR) return 125;
    if (stop || (deadline && ms() >= deadline)) {
      if (!stage) { kill(-child, SIGTERM); stage = 1; deadline = ms() + 2000; stop = 0; }
      else {
        kill(-child, SIGKILL);
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
        stage = 2;
        break;
      }
    }
    usleep(10000);
  }
  printf("VIDEO_RUNNER_END forced=%d status=%d\n", stage, status);
  fflush(stdout);
  return stage ? 124 : WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
