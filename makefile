# write a makefile to make server and client files
CC = gcc
CFLAGS = -Wall

all: smserver smclient
server: smserver.c
	$(CC) $(CFLAGS) -o smserver smserver.c
client: smclient.c
	$(CC) $(CFLAGS) -o smclient smclient.c

clean:
	rm -f smserver smclient