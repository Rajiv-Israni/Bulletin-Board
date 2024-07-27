# Bulletin-Board

First compile the program:
$ gcc -Wall -g bbserv.c -o bbserv -pthread

For phase 1, you can test with one client and a server. Open two terminals
In first
$ cd bulleting_board
$> ./bbserv
Using port 9000 on skt 3
Using port 10000 on skt 4
[ACCEPTING] Socket descriptor 6
[THREAD] Request on sock 6
[READING] item.num=13
[READING DONE] item.num=13
[WRITING] item.num=1
[WRITE DONE] item.num=1
[READING] item.num=1
[READING DONE] item.num=1
[REPLACING] item.num=1
[REPLACE END] num=1

In second
$ telnet localhost 9000
Trying 127.0.0.1...
Connected to localhost.
Escape character is '^]'.
Welcome to bulletin board.
USER Johnny
1.0 Hello Johnny, Welcome to Bulletin Board
READ 13
2.1 UNKNOWN 13 No such message
WRITE Johnny is here.
3.0 WROTE 1
READ 1
2.0 MESSAGE 1 Johnny/Johnny is here.
REPLACE 1/Johnny was here...
3.0 WROTE 1
BAD_COMMAND
2.2 ERROR Invalid message
QUIT bye bye
4.0 BYE Johnny
Connection closed by foreign host.

For phase 2, you need at least 2 servers. I have setup x/ and y/ folders for the second server.
Each server must execute in a separate directory, because he writes the pidfile.
In this test we will run 3 servers, base one in root directory and x/ and y/.
In bbserv.conf, update PEERS=localhost:10001 localhost:10002

Then open 3 terminals, for each server, and run them
$ bulletin_board> ./bbserv
$ bulletin_board/x> ./bbserv
$ bulletin_board/y> ./bbserv

In 4th terminal run the client
$ telnet localhost 9000
Trying 127.0.0.1...
Connected to localhost.
Escape character is '^]'.
Welcome to bulletin board.
USER Johnny
1.0 Hello Johnny, Welcome to Bulletin Board
READ 13
2.1 UNKNOWN 13 No such message
WRITE Johnny is here.
3.0 WROTE 1
READ 1
2.0 MESSAGE 1 Johnny/Johnny is here.
REPLACE 1/Johnny was here...
3.0 WROTE 1
BAD_COMMAND
2.2 ERROR Invalid message
QUIT bye bye
4.0 BYE Johnny
Connection closed by foreign host.
