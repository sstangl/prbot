all:
	gcc -std=gnu99 --pedantic -g irc.c prbot.c -o prbot

clean:
	rm -f prbot *.o
