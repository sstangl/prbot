/*
 * Copyright 2013 Sean Stangl (sean.stangl@gmail.com)
 * 
 * This file is part of PRBot.
 *
 * PRBot is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * PRBot is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with PRBot.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "irc.h"

#define BUF_LEN 1024

static bool
handle_ping(int fd, struct ircmsg_ping *ping)
{
	irc_pong(fd, ping->text);
	return true;
}

static bool
handle_part(int fd, struct ircmsg_part *part)
{
	return true;
}

static bool
handle_join(int fd, struct ircmsg_join *join)
{
	return true;
}

static bool
handle_privmsg(int fd, struct ircmsg_privmsg *msg)
{
	if (strstr(msg->text, "PRBot7: ") == msg->text && msg->chan[0] == '#')
		irc_privmsg(fd, msg->chan, "%s: shut the fuck up.", msg->name.nick);

	return true;
}

static bool
handle_kick(int fd, struct ircmsg_kick *kick)
{
	return true;
}

static bool
dispatch_handler(int fd, struct ircmsg *msg)
{
	switch (msg->type) {
		case IRCMSG_UNKNOWN:  return true;
		case IRCMSG_PING:     return handle_ping(fd, &msg->u.ping);
		case IRCMSG_PART:     return handle_part(fd, &msg->u.part);
		case IRCMSG_JOIN:     return handle_join(fd, &msg->u.join);
		case IRCMSG_PRIVMSG:  return handle_privmsg(fd, &msg->u.privmsg);
		case IRCMSG_KICK:     return handle_kick(fd, &msg->u.kick);
		default:              return false;
	}
}

int
main(int argc, char *argv[])
{
	char buf[BUF_LEN];
	struct ircbuf ircbuf;
	ircbuf_init(&ircbuf, buf, BUF_LEN);

	int fd = irc_connect("irc.rizon.net", "6667");
	if (fd < 0) {
		fprintf(stderr, "Failed to open connection.\n");
		return 1;
	}

	irc_nick(fd, "PRBot7", NULL);
	irc_join(fd, "#prbottest");

	char *line;
	while (line = irc_getline(fd, &ircbuf)) {
		printf("%s\n", line);

		struct ircmsg msg;
		irc_parseline(line, &msg);
		if (!dispatch_handler(fd, &msg))
			return 2;
	}

	return 0;
}
