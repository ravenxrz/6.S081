#include "kernel/types.h"
#include "user/user.h"

/**
 *  @left_fd fd array from left process, which are alread `piped open`
 */
void 
create_right_side_process(int left_fd[2]) {
  int pid;
  int val;
  int first_val;
  int created_child = 0;
  int right_fd[2];
  if ((pid = fork()) == 0) {
    close(left_fd[1]);
    // receive the first number
    read(left_fd[0], &first_val, sizeof(first_val));
    printf("prime %d\n", first_val);
    // now start to receive number til fds[0] is closed
    while (read(left_fd[0], &val, sizeof(val)) != 0) {
      if (val % first_val == 0) continue;
      if (!created_child) {
        created_child = 1;
        // create the child
        pipe(right_fd);
        create_right_side_process(right_fd);
        close(right_fd[0]);
      }
      // send val to child
      write(right_fd[1], &val, sizeof(val));
    }
    close(right_fd[1]); // notify children to exit
    wait((void *)0);
    exit(0);
  }
}

// calc prime number between 2-35
int 
main() {
  // the first process
  int right_fd[2];
  int i;
  printf("prime 2\n");
  pipe(right_fd);
  create_right_side_process(right_fd);
  close(right_fd[0]);
  for (i = 3; i <= 35; i++) {
    if (i % 2 != 0) {
      write(right_fd[1], &i, sizeof(i));
    }
  }
  close(right_fd[1]);
  wait((void *)0);
  exit(0);
}
