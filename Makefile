CC=gcc
CFLAGS=-std=c99 -W -Wall

debug: cyflowrec.c
	$(CC) $(CFLAGS) -g -o cyflowrec cyflowrec.c

stable: cyflowrec.c
	$(CC) $(CFLAGS) -O2 -o cyflowrec cyflowrec.c

clean:
	rm -vf cyflowrec
