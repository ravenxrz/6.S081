#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/stat.h"
#include "user/user.h"

// get one cmd from `cmd` one at a time, which is delimted by '\n'
// and return the cmd line
char *get_one_command(char **cmdp) {
  char *start = *cmdp;
  char *cmd = *cmdp;
  while (*cmd && *cmd != '\n') {
    cmd++;
  }
  if (*cmd == '\n') {
    *cmd = '\0';
    *cmdp = cmd + 1; // advance to next cmd
  } else {
    *cmdp = cmd; // end of cmd
  }
  return start;
}

int main(int argc, char *argv[]) {
  char *cmd[MAXARG];
  int cmd_len = 0;
  if (argc == 1) {
    // default use echo
    cmd[0] = "echo";
    cmd_len++;
  } else {
    // else read from argv
    for (int i = 1; i < argc; i++) {
      cmd[i - 1] = argv[i];
    }
    cmd_len = argc - 1;
  }

  // read cmds from stdin
  char extra_cmd_str[512];
  char *extra_cmd_p = extra_cmd_str;
  char *append_cmds[MAXARG];
  int acmd_len = 0;
  read(0, extra_cmd_str, sizeof(extra_cmd_str));
  for (char *cmd = get_one_command(&extra_cmd_p); *cmd;
       cmd = get_one_command(&extra_cmd_p)) {
    append_cmds[acmd_len++] = cmd;
  }

  for (int i = 0; i < acmd_len; i++) {
    cmd[cmd_len] = append_cmds[i];
    if (fork() == 0) {
      exec(cmd[0], &cmd[0]);
      printf("exec error\n");
      exit(0);
    } else {
      wait((void *)0);
    }
  }

  exit(0);
}
