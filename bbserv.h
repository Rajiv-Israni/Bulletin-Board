#include <pthread.h>
#include <arpa/inet.h>  //for sockaddr_in

//debug times for read/write
#define DEBUG_TIME_RD 3
#define DEBUG_TIME_WR 6

//max sizes for user, message and line
#define MAX_USR_LEN 20
#define MAX_MSG_LEN 200
#define MAX_LINE_LEN 250

//Max size of request bounded buffer
#define MAX_RBB_LEN 100
#define MAX_CMD_ARGS 10

struct peer {
  struct sockaddr_in inaddr;  //IP, port
  int fd;
  int rv;
};

struct cmd {
  char * arg[MAX_CMD_ARGS];  //command has at most 10 args
  int nargs;      //number of args
};

struct bulletin_item {
  int num;
  char usr[MAX_USR_LEN+1];
  char msg[MAX_MSG_LEN+1];
};

struct context { //thread context
  pthread_t thread;
  int fd;
  int sync_on;
  struct bulletin_item rec;
  char line[MAX_LINE_LEN + 1];
};

struct bounded_buf {
  int in,out,count;
  int fds[MAX_RBB_LEN];

  pthread_mutex_t mutex;
  pthread_cond_t  empty, full;
};

struct bulletin_board {
  pthread_rwlock_t rwlock;
  int board_len;
  int board_size;

  int fd;                         //file descriptor
  struct bulletin_item * items;   //mmaped to file

};
