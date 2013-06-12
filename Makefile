all:
	gcc -std=gnu99 --pedantic -g irc.c prbot.c -lsqlite3 -o prbot

clean:
	rm -f prbot *.o
