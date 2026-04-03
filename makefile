# write a makefile to make server and client files
CC = gcc
CFLAGS = -Wall

all: smserver smclient dserver dclient
server: smserver.c
	$(CC) $(CFLAGS) -o smserver smserver.c
client: smclient.c
	$(CC) $(CFLAGS) -o smclient smclient.c

dserver: smserver.c
	$(CC) $(CFLAGS) -DDEBUG -o dserver smserver.c
dclient: smclient.c
	$(CC) $(CFLAGS) -DDEBUG -o dclient smclient.c

init: init.c
	$(CC) $(CFLAGS) -o init init.c

runinit: init
	./init users.txt

runserver: smserver
	./smserver 8080 users.txt

runclient: smclient
	./smclient 127.0.0.1 8080

drunserver: dserver
	./dserver 8080 users.txt

drunclient: dclient
	./dclient 127.0.0.1 8080

clean:
	rm -f smserver smclient dserver dclient init

deepclean: clean
	rm -rf mailboxes