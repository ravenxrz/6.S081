#include "kernel/types.h"
#include "user/user.h"
int main(int argc, char *argv[]) {
  int fds1[2];    // parent write, child read
  int fds2[2];    // parent read, child write
  char c;
  int child_pid;
  pipe(fds1); // assume it will be success
  pipe(fds2); // assume it will be success
  if ((child_pid = fork()) == 0) {
    close(fds1[1]);
    close(fds2[0]);
    // child
    // step2: receive one byte from parent
    read(fds1[0], &c, 1);
    printf("%d: received ping\n", getpid());
    // step3: send one byte back to parent
    write(fds2[1], &c, 1);
  } else {
    close(fds1[0]);
    close(fds2[1]);
    // parent
    // step1: send one byte to child
    write(fds1[1], &c, 1);
    read(fds2[0], &c, 1);
    printf("%d: received pong\n", getpid());
    wait((void *)0);
  }
  exit(0);
}
