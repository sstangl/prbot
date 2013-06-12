all:
	gcc -std=gnu99 --pedantic -g irc.c prbot.c -lsqlite3 -Wall -Werror -Wno-error=unused-variable -o prbot

clean:
	rm -f prbot *.o
