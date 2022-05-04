#include "kernel/types.h"
#include "kernel/fs.h"
#include "kernel/stat.h"
#include "user/user.h"

char *fmtname(char *path) {
  static char buf[DIRSIZ + 1];
  char *p;

  // Find first character after last slash.
  for (p = path + strlen(path); p >= path && *p != '/'; p--)
    ;
  p++;

  // Return blank-padded name.
  if (strlen(p) >= DIRSIZ)
    return p;
  memmove(buf, p, strlen(p));
  memset(buf + strlen(p), ' ', DIRSIZ - strlen(p));
  return buf;
}

int path_cmp(char *target_path, char *cmp_path) {
  char *p = target_path;
  char *q = cmp_path;
  for (; *p && *q; p++, q++) {
    if (*p != *q) {
      return *p - *q;
    }
  }
  return !(*p == 0 && *q == 0);
}

void find(char *dir, char *fname) {
  int fd;
  struct dirent de; // de.name is just the filename
  struct stat st;
  char buf[512];
  memset(buf, 0, sizeof(buf));
  strcat(strcat(buf, dir), "/");
  int base_len = strlen(buf);

  if ((fd = open(dir, 0)) < 0) {
    fprintf(2, "find: cannot open %s\n", dir);
    return;
  }
  if (fstat(fd, &st) < 0) {
    fprintf(2, "find: cannot stat %s\n", dir);
    close(fd);
    return;
  }
  if (st.type != T_DIR) {
    fprintf(2,
            "find: wrong param of `dir`, which should be a directory name\n");
    exit(0);
  }

  while (read(fd, &de, sizeof(de)) == sizeof(de)) {
    if (de.inum == 0 || strcmp(de.name, ".") == 0 ||
        strcmp(de.name, "..") == 0) {
      continue;
    }
    memcpy(buf + base_len, de.name, strlen(de.name) + 1);
    if (stat(buf, &st) < 0) {
      fprintf(2, "find: cannot stat %s\n", de.name);
      continue;
    }
    switch (st.type) {
    case T_FILE:
      if (path_cmp(fname, de.name) == 0) {
        printf("%s\n", buf);
      }
      break;
    case T_DIR:
      find(buf, fname);
      break;
    }
  }
  close(fd);
}

int main(int argc, char *argv[]) {
  if (argc != 3) {
    fprintf(2, "usage: find base_dir file_name\n");
    exit(0);
  }
  find(argv[1], argv[2]);
  exit(0);
}
