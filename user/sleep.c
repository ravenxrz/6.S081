#include "kernel/types.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int ticker_num;

  if(argc != 2){
    fprintf(2, "Usage: sleep ticker_num\n");
    exit(1);
  }
  ticker_num = atoi(argv[1]);
  sleep(ticker_num);
  exit(2);
}
