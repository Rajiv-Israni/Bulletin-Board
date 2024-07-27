CC=gcc
CFLAGS=-Wall -g

bbserv: bbserv.c bbserv.h
	$(CC) $(CFLAGS) bbserv.c -o bbserv -pthread

clean:
	rm -f bbserv bbserv.o data.bb x/data.bb y/data.bb
