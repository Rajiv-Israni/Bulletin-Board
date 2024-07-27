#include <netdb.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <ctype.h>
#include <limits.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "bbserv.h"

static const char* pid_fileame = "bbserv.pid";
static const char* nousername = "noname";

static int cfg_port[2] = {9000, 10000};
static int cfg_max_threads = 20;
static int cfg_daemon = 1;
static int cfg_debug = 0;

static char * cfg_bulletin_file = NULL;  //bulletin board file

static struct peer * cfg_peer = NULL;
static unsigned int cfg_npeers = 0;

static struct bulletin_board bboard;
static struct bulletin_item commited;
static int commited_index = -1;

static struct bounded_buf rbb;  //request bounded buffer
static struct context * tctx = NULL;  //thread contexts

static void sig_handler(const int sig);

static int sfd[2];  //sockets for our ports

//HELPER: convert string to int
static int stoi(const char * str){
  char * endptr;
  const long num = strtol(str, &endptr, 10);
  if((errno == ERANGE && (num == LONG_MAX || num == LONG_MIN)) ||
      (errno != 0 && num == 0)  ){
    perror("strtol");
    return -1;
  }

  if(endptr == str) {
    fprintf(stderr, "No digits were found\n");
    return -1;
  }

  if (*endptr != '\0'){
    fprintf(stderr, "Further characters after number: %s\n", endptr);
    return -1;
  }

  return (int)num;
}

//HELPER: convert string to bool
static int stob(const char * str) {
  if(strcmp(str, "true") == 0){
    return 1;
  }else if(strcmp(str, "false") == 0){
    return 0;
  }else{
    return stoi(str);
  }
}

//HELPER: convert string to string[]
static int stoa(char * line, char ** args, const int args_size){
  int i = 1;
  char *save_ptr;

  args[0] = strtok_r(line, " \t", &save_ptr);
  while(args[i-1] && (i < args_size)){
    args[i++] = strtok_r(NULL, " \t", &save_ptr);
  }
  args[i] = NULL;
  return i - 1;
}

//HELPER: convert string to command
static int stocmd(char* str, struct cmd * cmd){

  bzero(cmd, sizeof(struct cmd));

  char * delim = strchr(str, ' ');
  cmd->arg[0] = str;
  cmd->nargs = 1;

  if(delim != NULL){
    delim[0] = '\0';

    cmd->arg[1] = &delim[1];
    if(cmd->arg[1][0] != '\0'){ //if no argument
      cmd->nargs++;
    }

    char * ptr = cmd->arg[1];
    int i;
    for(i=2; i < 4; i++){
      delim = strchr(ptr, '/');
      if(delim == NULL){
        break;
      }
      delim[0] = '\0';
      cmd->nargs++;
      ptr = cmd->arg[i] = &delim[1];
    }
  }

  return 0;
}

static int readln(const int fd, char *buf, const int buf_size){
  int len = 0, rv;
  while( (rv = read(fd, &buf[len], 1)) > 0){

    if(buf[len] == '\r'){
      buf[len] = '\0';
      continue;
    }else if(buf[len] == '\n'){
      buf[len] = '\0';
      break;
    }else{
      ++len;
    }
  }

  if(rv < 0){
    perror("read");
    return -1;
  }else if(rv == 0){
    return 0;
  }

  return len;
}

static int bulletin_map(){
  struct stat st;

  bboard.fd = open(cfg_bulletin_file, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
  if(bboard.fd == -1){
    perror("open");
  }

  if(fstat(bboard.fd, &st) == -1){
    perror("lstat");
    return -1;
  }

  if(st.st_size == 0){  //if its a new file
    //allocate space for the records
    bboard.board_len = 0;
    bboard.board_size = 10;
    st.st_size = bboard.board_size * sizeof(struct bulletin_item);
    if(ftruncate(bboard.fd, st.st_size) < 0){
      perror("ftruncate");
      return -1;
    }
  }else{

    //find how much items we have in bulletin board
    int i;
    for(i=0; i < bboard.board_size; i++){
      if(bboard.items[i].num == 0){ //item with 0 is free
        break;
      }
      bboard.board_len++;
    }
    bboard.board_size = st.st_size / sizeof(struct bulletin_item);
  }

  bboard.items =  mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, bboard.fd, 0);
  if(bboard.items == MAP_FAILED){
    perror("mmap");
    return -1;
  }

  return 0;
}

//NET: connect a peer
static int peer_connect(struct peer * p) {

  p->fd = socket(AF_INET, SOCK_STREAM, 0);
  if(p->fd < 0){
    perror("socket");
    return -1;
  }

  if(connect(p->fd, (struct sockaddr *)&p->inaddr, sizeof(struct sockaddr_in)) < 0) {
    perror("connect");
    close(p->fd);
    return -1;
  }
  return 0;
}

//SYNC: connect to all peers
static int psync_connect(){
  int i;
  for(i=0; i < cfg_npeers; i++){
    if(peer_connect(&cfg_peer[i]) == -1)
      return -1;
  }
  return 0;
}

//SYNC: disconnect all peers
static void psync_disconnect(){
  int i;
  for(i=0; i < cfg_npeers; i++){
    if(cfg_peer[i].fd > 0){
      shutdown(cfg_peer[i].fd, SHUT_RDWR);
      close(cfg_peer[i].fd);
      cfg_peer[i].fd = -1;
    }
  }
}

static int psync_rdall(char * buf, const int buf_size){

  int ack = 0, nack = 0;
  struct timespec timeout;

  int i, nfds = 0;
  for(i=0; i < cfg_npeers; i++){
    if(nfds < cfg_peer[i].fd)
      nfds = cfg_peer[i].fd;
  }
  nfds++;

  timeout.tv_sec = 1;
  timeout.tv_nsec = 0;
  if(cfg_debug){
    timeout.tv_sec += 2*DEBUG_TIME_WR; //in debug, we sleep between writes
  }

  while((ack + nack) < cfg_npeers){

    fd_set rdfds;
    FD_ZERO(&rdfds);
    for(i=0; i < cfg_npeers; i++){
      FD_SET(cfg_peer[i].fd, &rdfds);
    }

    switch(pselect(nfds + 1, &rdfds, NULL, NULL, &timeout, NULL)){
      case -1:
        perror("pselect");
      case 0:
        return -1;  //on timeout

      default:

        for(i=0; i < cfg_npeers; i++){
          if(FD_ISSET(cfg_peer[i].fd, &rdfds)){


            if(readln(cfg_peer[i].fd, buf, buf_size) <= 0){
              return -1;
            }

            if(cfg_debug){
              printf("[PSYNC:%d] %s\n", i, buf);
            }

            if(strcmp(buf, "NACK") == 0){  //if peer failed
              nack++;
            }else if(strcmp(buf, "ACK") == 0){
              ack++;
            }
          }
        }
        break;
    } //end select
  } //end while

  return (nack > 0) ? -1 : 0; //if even one nack, then return error
}

static int psync_wrall(const char * buf){
  int i;
  char buf2[MAX_LINE_LEN+1];
  const int len = strlen(buf);

  for(i=0; i < cfg_npeers; i++){
    if(cfg_debug){
      printf("[PSYNC:%d out] %s", i, buf);
    }

    if(send(cfg_peer[i].fd, buf, len, 0) != len){
      return -1;
    }
  }

  return psync_rdall(buf2, MAX_LINE_LEN);
}

static int psync_commit(const int id, const char * username, const char *message){
  char buf[MAX_LINE_LEN+1];
  
  if(id == -1){
    snprintf(buf, MAX_LINE_LEN, "SYNC_WRITE %s/%s\n", username, message);
  }else{
    snprintf(buf, MAX_LINE_LEN, "SYNC_REPLACE %d/%s/%s\n", id, username, message);;
  }

  return psync_wrall(buf);
}

static int bulletin_open(){

  if(cfg_bulletin_file == NULL){
    return -1;
  }

  if(bulletin_map() == -1){
    return -1;
  }

  pthread_rwlock_init(&bboard.rwlock, NULL);

  //initialize the last commit
  bzero(&commited, sizeof(struct bulletin_item));
  commited.num = -1;

  return 0;
}

static int bulletin_close(){
  munmap(bboard.items, bboard.board_size*sizeof(struct bulletin_item));
  close(bboard.fd);
  pthread_rwlock_destroy(&bboard.rwlock);
  return 0;
}

static int bulletin_remap(){
  //increase size of bulleting board with 10 items
  const int new_size = (bboard.board_size + 10)*sizeof(struct bulletin_item);
  if(ftruncate(bboard.fd, new_size) < 0){
    perror("ftruncate");
    return -1;
  }
  //reopen the board
  bulletin_close();
  bulletin_open();
  return 0;
}

//Find a record by id
static int bulletin_search(const int num){
  int i;
  for(i=0; i < bboard.board_size; i++){
    if(bboard.items[i].num == num)
      return i;
  }
  return -1;
}

static int bulletin_read(const int num, struct bulletin_item *rec){

  pthread_rwlock_rdlock(&bboard.rwlock);

  if(cfg_debug){
    printf("[READING] item.num=%i\n", num);
    sleep(DEBUG_TIME_RD);
  }

  const int index = bulletin_search(num);

  int rv = 0;
  if(index >= 0){  //if record was found
    rv = 1;
    memcpy(rec, &bboard.items[index], sizeof(struct bulletin_item));
  }
  if(cfg_debug){
    printf("[READING DONE] item.num=%i\n", num);
  }
  pthread_rwlock_unlock(&bboard.rwlock);

  return rv;
}

static int bulletin_write(const char *user, const char *message){

  if((bboard.board_len + 1) > bboard.board_size){
    bulletin_remap(); //increase size of bulletin board
  }

  //fill record data
  const int index = ++bboard.board_len;
  bboard.items[index].num = index;
  strncpy(bboard.items[index].usr, user, MAX_USR_LEN);
  strncpy(bboard.items[index].msg, message, MAX_MSG_LEN);

  if(cfg_debug){
    printf("[WRITING] item.num=%d\n", bboard.items[index].num);
    sleep(DEBUG_TIME_WR);
  }

  //save info about last commit
  commited_index = index;
  commited.num = -1;  //writes have no number

  if(cfg_debug){
    printf("[WRITE DONE] item.num=%d\n", bboard.items[index].num);
  }

  return bboard.items[index].num;
}

static int bulletin_replace(const int num, const char *user, const char *message){

  if(cfg_debug){
    printf("[REPLACING] item.num=%d\n", num);
    sleep(DEBUG_TIME_WR);
  }

  const int index = bulletin_search(num);
  if(index >= 0){

    //save info for reverting commit
    commited_index = index;
    memcpy(&commited, &bboard.items[index], sizeof(struct bulletin_item));

    //id stays the same, update rest
    strncpy(bboard.items[index].usr, user, MAX_USR_LEN);
    strncpy(bboard.items[index].msg, message, MAX_MSG_LEN);
  }

  if(cfg_debug){
    printf("[REPLACE END] num=%d\n", bboard.items[index].num);
  }

  return bboard.items[index].num;
}

static int bulletin_commit(const int number, const char *user, const char *message){
  int rc = 0;

  //synchronize the commit operation
  pthread_rwlock_wrlock(&bboard.rwlock);

  //before precommit - just see who is available
  if(psync_connect() < 0){  //read welcome message
    pthread_rwlock_unlock(&bboard.rwlock);
    return -1;
  }

  //precommit - locks the bulletin board in all instances
  rc = psync_wrall("SYNC_ON\n");
  if(rc == 0){
    //actual commit
    rc = psync_commit(number, user, message);
    if(rc >= 0){  //if commit succeeded
      if(number == -1){
        rc = bulletin_write(user, message);
      }else{
        rc = bulletin_replace(number, user, message);
      }
    }
  }

  //if we had a failure in previous steps
  if(rc < 0){
    psync_wrall("SYNC_ABORT\n");
  }else{
    psync_wrall("SYNC_OFF\n");
  }
  psync_disconnect();

  pthread_rwlock_unlock(&bboard.rwlock);

  return rc;
}

static int bulletin_sync(const int on){
  int rc;
  if(on == 1){
    if(cfg_debug){
      printf("[SYNC ON]\n");
    }
    rc = (pthread_rwlock_wrlock(&bboard.rwlock) != 0) ? -1 : 0;
  }else{
    if(cfg_debug){
      printf("[SYNC OFF]\n");
    }
    rc = (pthread_rwlock_unlock(&bboard.rwlock) != 0) ? -1 : 0;
  }

  return rc;
}

static int bulletin_revert(){
  int rc = 1;

  if(cfg_debug){
    printf("[ABORTING] item.num=%i\n", commited.num);
  }

  pthread_rwlock_wrlock(&bboard.rwlock);

  if(commited.num == -1){
    //reduce item count, and clear last item
    memset(&bboard.items[--bboard.board_len], 0, sizeof(struct bulletin_board));

    if(cfg_debug){
      printf("[WRITING] Reverted last written item\n");
    }
  }else{
    //restore old record
    rc = bulletin_replace(commited.num, commited.usr, commited.msg);
    if(cfg_debug){
      if(rc > 0){
        printf("[REPLACING] Undo item.num=%d, %s/%s\n", commited.num, commited.usr, commited.msg);
      }else{
        printf("[REPLACING] Undo item.num=%d error\n", commited.num);
      }
    }
  }

  //clear last commit
  commited_index = -1;
  bzero(&commited, sizeof(struct bulletin_item));

  pthread_rwlock_unlock(&bboard.rwlock);
  return rc;
}

static int peer_resolve(const char * hname, const int port, struct sockaddr_in *inaddr){

  memset(inaddr, 0, sizeof(struct sockaddr_in));
  inaddr->sin_family = AF_INET;

  struct hostent * hinfo = gethostbyname(hname);
  if(hinfo == NULL){
    perror("gethostbyname");
    return -1;
  }

  memcpy(&inaddr->sin_addr, hinfo->h_addr, hinfo->h_length);
  inaddr->sin_port = htons(port);

  return 0;
}

//CONFIG: initialize the peers[]
static int config_peers(char ** args, const int npeers){
  int i = 0;

  cfg_npeers = npeers;
  cfg_peer = (struct peer*) calloc(cfg_npeers, sizeof(struct peer));
  if(cfg_peer == NULL){
    perror("calloc");
    return -1;
  }

  for(i=0; i < npeers; i++){
    char * save_ptr;
    const char * hname = strtok_r(args[i], ":", &save_ptr);
    const char * pport = strtok_r(NULL,    ":", &save_ptr);

    const int port = stoi(pport);
    if((port < 0) || (port > 65535)){
      return -1;
    }

    if(peer_resolve(hname, port, &cfg_peer[i].inaddr) == -1){
      return -1;
    }

    cfg_peer[i].fd = -1;
  }

  return i;
}

//CONFIG: setup config, using a file
static int config_file(const char * config){
  char line[1024];
  char * args[10];

  int fd = open(config, O_RDONLY);
  if(fd == -1){
    perror("open");
    return -1;
  }

  int rv = 0;
  while((readln(fd, line, sizeof(line)) > 0) && (rv == 0)){

    char * opt = strtok(line, "=");
    char * optarg = strtok(NULL, "=");

    if((opt == NULL) || (optarg == NULL)){
      continue;
    }

    if(strcmp(opt, "THMAX") == 0){
      cfg_max_threads = stoi(optarg);
      if(cfg_max_threads <= 0){
        rv = -1;
        break;
      }

    }else if(strcmp(opt, "BBPORT") == 0){
      cfg_port[0] = stoi(optarg);
      if((cfg_port[0] <=0) || (cfg_port[0] > 65535)){
        rv = -1;
        break;
      }

    }else if(strcmp(opt, "SYNCPORT") == 0){
      cfg_port[1] = stoi(optarg);
      if((cfg_port[1] <= 0) || (cfg_port[1] > 65535)){
        rv = -1;
        break;
      }

    }else if(strcmp(opt, "BBFILE") == 0){
      if(cfg_bulletin_file){  //if an old value exists
        free(cfg_bulletin_file);
      }

      cfg_bulletin_file = strdup(optarg);
      if(cfg_bulletin_file == NULL){
        perror("strdup");
        rv = -1;
        break;
      }

    }else if(strcmp(opt, "PEERS") == 0){
      cfg_npeers = stoa(optarg, args, 10);
      if(cfg_npeers > 0){
        if(config_peers(args, cfg_npeers) < 0){
          rv = -1;
          break;
        }
      }

    }else if(strcmp(opt, "DAEMON") == 0){
      cfg_daemon = stob(optarg);
      if(cfg_daemon == -1){
        rv = -1;
        break;
      }

    }else if(strcmp(opt, "DEBUG") == 0){
      cfg_debug = stob(optarg);
      if(cfg_debug == -1){
        rv = -1;
        break;
      }

    }else{
      fprintf(stderr, "Error: Invalid keyword '%s' in config %s\n", opt, config);
      rv = -1;
      break;
    }
  }
  close(fd);
  return rv;
}

//CONFIG: setup config, from argv[]
static int config_argv(const int argc, char * argv[]){
  int opt;

  while((opt = getopt(argc, argv, "b:c:dfp:s:T:")) > 0){

    switch(opt){
      case 'b':
        if(cfg_bulletin_file)
          free(cfg_bulletin_file);

        cfg_bulletin_file = strdup(optarg);
        if(cfg_bulletin_file == NULL){
          perror("strdup");
          return -1;
        }
        break;

      case 'c':
        if(config_file(optarg) < 0)
          return -1;
        break;

      case 'd':
        cfg_debug = 1;
        break;

      case 'f':
        cfg_daemon = 0;
        break;

      case 'p':
        cfg_port[0] = stoi(optarg);
        if((cfg_port[0] <=0) || (cfg_port[0] > 65535)){
          return -1;
        }
        break;

      case 's':
        cfg_port[1] = stoi(optarg);
        if((cfg_port[1] <=0) || (cfg_port[1] > 65535)){
          return -1;
        }
        break;

      case 'T':
        cfg_max_threads = stoi(optarg);
        if(cfg_max_threads <= 0){
          return -1;
        }
        break;

      default:
        fprintf(stderr, "Error: Invalid option '%c'\n", opt);
        return -1;
        break;
    }
  }

  const int nargs = argc - optind;
  if(nargs > 0){
    cfg_npeers = config_peers(&argv[optind], nargs);
    if(cfg_npeers < 0){
      return -1;
    }
  }

  return 0;
}

static int bb_pop(){ //pop one request from bounded buffer

  pthread_mutex_lock(&rbb.mutex);
  while(rbb.count <= 0){
    pthread_cond_wait(&rbb.full, &rbb.mutex);
  }

  const int fd = rbb.fds[rbb.out];
  rbb.out = (rbb.out + 1) % MAX_RBB_LEN;
  rbb.count--;
  pthread_cond_signal(&rbb.empty);
  pthread_mutex_unlock(&rbb.mutex);

  return fd;
}

static int bb_push(const int fd){

  pthread_mutex_lock(&rbb.mutex);
  while(rbb.count >= MAX_RBB_LEN){  //while bb is full
    pthread_cond_wait(&rbb.empty, &rbb.mutex);
  }

  rbb.fds[rbb.in] = fd;
  rbb.in = (rbb.in + 1) % MAX_RBB_LEN;
  rbb.count++;

  pthread_cond_signal(&rbb.full);
  pthread_mutex_unlock(&rbb.mutex);

  return 0;
}

static int cmd_user(struct context *ctx, struct cmd * cmd){
  if(cmd->nargs != 2){
    return -1;  //invalid count of arguments
  }

  //validate username argument
  if( (strchr(cmd->arg[1], '/') != NULL) || (strcmp(cmd->arg[1], nousername) == 0)){
    dprintf(ctx->fd, "2.2 ERROR USER Invalid username\n");

  }else if(strcmp(ctx->rec.usr, nousername) != 0){
    dprintf(ctx->fd, "2.2 ERROR USER Already registered\n");
  }else{
    strncpy(ctx->rec.usr, cmd->arg[1], MAX_USR_LEN);
    dprintf(ctx->fd, "1.0 Hello %s, Welcome to Bulletin Board\n", ctx->rec.usr);
  }

  return 0; //sucess
}

static int cmd_read(struct context *ctx, struct cmd * cmd){
  if(cmd->nargs != 2){
    return -1;  //invalid count of arguments
  }

  const int number = stoi(cmd->arg[1]);
  if(number < 0){
    return -1;
  }

  switch(bulletin_read(number, &ctx->rec)){
    case 0:
      dprintf(ctx->fd, "2.1 UNKNOWN %s No such message\n", cmd->arg[1]);
      break;

    case -1:
      dprintf(ctx->fd, "2.2 ERROR READ system error\n");
      break;

    default:
      dprintf(ctx->fd, "2.0 MESSAGE %i %s/%s\n", number, ctx->rec.usr, ctx->rec.msg);
      break;
  }
  return 0;
}

static int cmd_write(struct context *ctx, struct cmd * cmd){
  if(cmd->nargs != 2){
    return -1;  //invalid count of arguments
  }

  const int number = bulletin_commit(-1, ctx->rec.usr, cmd->arg[1]);
  switch(number){
    case -1:
      dprintf(ctx->fd, "3.2 ERROR WRITE system error\n");
      break;

    default:
      dprintf(ctx->fd, "3.0 WROTE %i\n", number);
      break;
  }
  return 0;
}

static int cmd_replace(struct context *ctx, struct cmd * cmd){
  if(cmd->nargs != 3){
    return -1;  //invalid count of arguments
  }

  const int number = stoi(cmd->arg[1]);
  if(number < 0){
    return -1;
  }

  switch(bulletin_commit(number, ctx->rec.usr, cmd->arg[2])){
    case -1:
      dprintf(ctx->fd, "3.2 ERROR WRITE system error\n");
      break;

    case 0:
      dprintf(ctx->fd, "3.1 UNKNOWN %s\n", cmd->arg[1]);
      break;

    default:
      dprintf(ctx->fd, "3.0 WROTE %i\n", number);
      break;
  }
  return 0;
}

static int cmd_sync_on(struct context *ctx, struct cmd * cmd){
  int rv = 0;

  if(ctx->sync_on == 0){
    rv = bulletin_sync(1);
    ctx->sync_on = 1;
  }else{
    //we can't call SYNC_ON twice
    dprintf(ctx->fd, "3.2 ERROR WRITE system error\n");
    rv = -1;
  }
  return rv;
}

static int cmd_sync_off(struct context *ctx, struct cmd * cmd){
  int rv = 0;

  if(ctx->sync_on == 1){
    rv = bulletin_sync(0);
    ctx->sync_on = 0;
  }else{
    //we can't call SYNC_OFF twice
    dprintf(ctx->fd, "3.2 ERROR WRITE system error\n");
    rv = -1;
  }
  return rv;
}

static int cmd_sync_abort(struct context *ctx, struct cmd * cmd){
  int rv = 0;

  if(ctx->sync_on == 1){
    rv = bulletin_revert();
    rv = bulletin_sync(0);  //sync off on abort automatically
    ctx->sync_on = 0;
  }else{
    //we can't call SYNC_ON twice
    dprintf(ctx->fd, "3.2 ERROR WRITE system error\n");
    rv = -1;
  }
  return rv;
}

static int cmd_sync_write(struct context *ctx, struct cmd * cmd){
  int rv = 0;

  if(ctx->sync_on == 1){
    if(cmd->nargs != 3){
      rv = -1;  //invalid count of arguments
    }else{
      rv = bulletin_write(cmd->arg[1], cmd->arg[2]);
    }

  }else{
    //we can't call SYNC_ON twice
    dprintf(ctx->fd, "3.2 ERROR WRITE system error\n");
    rv = -1;
  }
  return rv;
}

static int cmd_sync_replace(struct context *ctx, struct cmd * cmd){
  int rv = 0;

  if(ctx->sync_on == 1){
    if(cmd->nargs != 4){
      rv = -1;  //invalid count of arguments
    }else{
      const int number = stoi(cmd->arg[1]);
      if(number < 0){
        rv = -1;
      }else{
        rv = bulletin_replace(number, cmd->arg[2], cmd->arg[3]);
      }
    }

  }else{
    //we can't call SYNC_ON twice
    dprintf(ctx->fd, "3.2 ERROR WRITE system error\n");
    rv = -1;
  }
  return rv;
}

static int request_handler(struct context * ctx){

  if(dprintf(ctx->fd, "Welcome to bulletin board.\n") <= 0){
    perror("dprintf");
    return -1;
  }

  memset(&ctx->rec, 0, sizeof(struct bulletin_item));

  strncpy(ctx->rec.usr, nousername, MAX_USR_LEN);


  int len = 0, rv = 0, sync_on = 0;
  struct cmd cmd;

  while((len = readln(ctx->fd, ctx->line, MAX_LINE_LEN)) > 0){

    if(stocmd(ctx->line, &cmd) == -1){  //conver line to command
      break;
    }

    rv = 0;
    if(strcmp(cmd.arg[0], "USER") == 0){
      rv = cmd_user(ctx, &cmd);

    }else if(strcmp(cmd.arg[0], "READ") == 0){
      rv = cmd_read(ctx, &cmd);

    }else if(strcmp(cmd.arg[0], "WRITE") == 0){
      rv = cmd_write(ctx, &cmd);

    }else if(strcmp(cmd.arg[0], "REPLACE") == 0){
      rv = cmd_replace(ctx, &cmd);

    }else if(strcmp(cmd.arg[0], "QUIT") == 0){
      break;

    }else if(strncmp(cmd.arg[0], "SYNC_", 5) == 0){ //if its a sync command

      if(strcmp(cmd.arg[0], "SYNC_ON") == 0){
        rv = cmd_sync_on(ctx, &cmd);

      }else if(strcmp(cmd.arg[0], "SYNC_OFF") == 0){
          rv = cmd_sync_off(ctx, &cmd);

      }else if(strcmp(cmd.arg[0], "SYNC_ABORT") == 0){
        rv = cmd_sync_abort(ctx, &cmd);
        break;  //close the connection

      }else if(strcmp(cmd.arg[0], "SYNC_WRITE") == 0){
        rv = cmd_sync_write(ctx, &cmd);

      }else if(strcmp(cmd.arg[0], "SYNC_REPLACE") == 0){
        rv = cmd_sync_replace(ctx, &cmd);

      }else{
        dprintf(ctx->fd, "2.2 ERROR Invalid message\n");
        rv = -1;
      }

      //send sync command status
      if(rv >= 0){
        write(ctx->fd, "ACK\n", 4);
      }else{
        write(ctx->fd, "NACK\n", 5);
      }

    }else{
      dprintf(ctx->fd, "2.2 ERROR Invalid message\n");
    }

    if(rv < 0){
      dprintf(ctx->fd, "2.2 ERROR Invalid command arguments\n");
    }
  }

  if((len > 0) && (rv == 0)){  //send bye only on quit
    dprintf(ctx->fd, "4.0 BYE %s\n", ctx->rec.usr);
  }

  if(sync_on > 0){
    bulletin_sync(0);
  }

  return 0;
}

static void* bbserv_thread(void * arg){
  struct context * ctx = (struct context*) arg;

  while((ctx->fd = bb_pop()) > 0){

    if(cfg_debug){
      printf("[THREAD] Request on sock %d\n", ctx->fd);
    }
    request_handler(ctx);

    shutdown(ctx->fd, SHUT_RDWR);
    close(ctx->fd);
  }

  pthread_exit(NULL);
}

static int thr_preallocate(){

  tctx = (struct context *) calloc(cfg_max_threads, sizeof(struct context));
  if(tctx == NULL){
    return -1;
  }

  //initialize the bb
  rbb.in = rbb.out = rbb.count = 0;
  memset(&rbb, 0, sizeof(struct bounded_buf));

  if( (pthread_mutex_init(&rbb.mutex, NULL) != 0) ||
      (pthread_cond_init(&rbb.empty, NULL)  != 0) ||
      (pthread_cond_init(&rbb.full, NULL)   != 0)){
    return -1;
  }

  int i, rc = 0;
  for(i=0; i < cfg_max_threads; i++){
    if(pthread_create(&tctx[i].thread, NULL, bbserv_thread, (void*)&tctx[i]) != 0){
      rc = -1;
      break;
    }
  }

  return rc;
}

static int thr_deallocate(){
  int i;
  for(i=0; i < cfg_max_threads; i++){
    bb_push(-1);
  }

  for(i=0; i < cfg_max_threads; i++){
    pthread_join(tctx[i].thread, NULL);
  }

  rbb.in = rbb.out = rbb.count = 0;
  memset(&rbb, 0, sizeof(struct bounded_buf));

  pthread_mutex_destroy(&rbb.mutex);
  pthread_cond_destroy(&rbb.empty);
  pthread_cond_destroy(&rbb.full);

  free(tctx);
  return 0;
}

static int open_ports(){
  struct sockaddr_in sa;

  memset(&sa, 0, sizeof(struct sockaddr_in));
  sa.sin_family       = AF_INET;
  sa.sin_addr.s_addr  = htonl(INADDR_ANY);

  int i;
  for(i=0; i < 2; i++){
    sa.sin_port = htons(cfg_port[i]);

    sfd[i] = socket(AF_INET, SOCK_STREAM, 0);
    if(sfd[i] == -1){
      perror("socket");
      return -1;
    }

    const int opt = 1;
    setsockopt(sfd[i], SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(int));

    if(bind(sfd[i], (struct sockaddr *)&sa, sizeof(struct sockaddr_in)) == -1){
      perror("bind");
      return -1;
    }

    if (listen(sfd[i], 5) < 0 ) {
      perror("listen");
      return -1;
    }


    if(cfg_debug){
      printf("Using port %i on skt %d\n", cfg_port[i], sfd[i]);
    }
  }

  return 0;
}

static void close_ports(){
  int i;

  for(i=0; i < 2; i++){
    shutdown(sfd[i], SHUT_RDWR);
    close(sfd[i]);
  }
}

static int select_ports(){
  int i;
  const int nfds = ((sfd[0] > sfd[1]) ? sfd[0] : sfd[1]) + 1;

  struct timespec timeout;
  timeout.tv_sec = 5;
  timeout.tv_nsec = 0;

  while(1){

    fd_set rdfds;
    FD_ZERO(&rdfds);
    FD_SET(sfd[0],  &rdfds);
    FD_SET(sfd[1],  &rdfds);

    const int rv = pselect(nfds, &rdfds, NULL, NULL, &timeout, NULL);
    if((rv < 0) && (errno != EINTR)){
      perror("select");
      break;

    }else if(rv > 0){

      for(i=0; i < 2; i++){
        if(FD_ISSET(sfd[i], &rdfds)){

          const int sd = accept(sfd[i], NULL, NULL);
          if(sd == -1){
            perror("accept");
            return -1;
          }

          if(cfg_debug){
            printf("[ACCEPTING] Socket descriptor %d\n", sd);
          }
          return sd; //return index of socket
        }
      }
    }
  }

  return -1;  //error
}

static int pidfile_write(){

  const int fd = open(pid_fileame, O_CREAT | O_WRONLY, S_IRUSR | S_IWUSR);
  if(fd == -1){
    perror("open");
    return -1;
  }

  dprintf(fd, "%i", getpid());
  close(fd);

  return 0;
}

static int pidfile_read(){
  char buf[100];

  const int fd = open(pid_fileame, O_RDONLY);
  if(fd == -1){
    if(errno == ENOENT){
      return 0;
    }else{
      perror("open");
      return 1;
    }
  }

  const int len = read(fd, buf, sizeof(buf));
  close(fd);

  buf[len] = '\0';
  return stoi(buf);
}

static int pidfile_test(){
  const int pid = pidfile_read();
  switch(pid){
    case 0:
      return 0; //process is dead

    case -1:    //read err
      break;

    default:
      if( (kill(pid, SIGUSR1) == -1) &&
          (errno == ESRCH)  ){
        return 0; //process not running
      }
  }

  return 1;  //process is running
}

static int startup(){

  umask(0177);

  signal(SIGHUP,  sig_handler);
  signal(SIGQUIT, sig_handler);
  signal(SIGTERM, SIG_IGN);
  signal(SIGINT,  SIG_IGN);
  signal(SIGALRM, SIG_IGN);
  signal(SIGSTOP, SIG_IGN);

  if(cfg_daemon){

    if(setpgid(getpid(), 0) == -1){
      perror("setpgid");
      return -1;
    }

    int i;
    for (i = 0; i < getdtablesize(); i++){
      if ((sfd[0] != i) && (sfd[1] != i)){
        close(i);
      }
    }

    //redirect in and out/err
    int fd = open("/dev/null", O_RDONLY);
    fd = open("bbserv.log", O_CREAT | O_WRONLY | O_APPEND, S_IRUSR | S_IWUSR);
    dup(fd);

    //detach controlling tty
    fd = open("/dev/tty", O_RDWR);
    ioctl(fd,TIOCNOTTY,0);
    close(fd);

    //in background
    switch(fork()){
      case -1:
        perror("fork");
        return 1;
      case 0:
        break;
      default:
        exit(0);  //parent exists
    }
  }

  //are we the onle server ?
  if(pidfile_test("bbserv.pid") || pidfile_write()){
    return -1;
  }
  return 0;
}

static int config_reload(){

  if( (config_file("bbserv.conf") < 0) ||
      (open_ports() < 0) ||
      (thr_preallocate(cfg_max_threads) < 0) ||
      (bulletin_open(cfg_bulletin_file, cfg_daemon) < 0)){
    return -1;
  }else{
    return 0;
  }
}

static int before_exit(){
  close_ports();
  thr_deallocate();
  bulletin_close();
  psync_disconnect();

  if(cfg_peer)
    free(cfg_peer);

  if(cfg_bulletin_file)
    free(cfg_bulletin_file);

  return 0;
}

static void sig_handler(const int sign){

  before_exit();

  switch(sign){
    case SIGQUIT:
      unlink(pid_fileame);
      exit(EXIT_SUCCESS);
    case SIGHUP:
      if(config_reload() == -1){
        unlink(pid_fileame);
        exit(EXIT_FAILURE);
      }
      break;
    default:
      break;
  }
}

int main(const int argc, char * argv[]){

  if( (config_file("bbserv.conf") == -1) ||
      (config_argv(argc, argv) == -1) ){
    return EXIT_FAILURE;
  }

  if( (open_ports() == -1)  ||  (startup() == -1) ||
      (thr_preallocate() == -1) || (bulletin_open() == -1)){
    return EXIT_FAILURE;
  }

  int sd;
  while((sd = select_ports()) > 0){

    if(bb_push(sd) < 0){
      close(sd);
      break;
    }
  }

  before_exit();
  unlink(pid_fileame);
  return 0;
}
