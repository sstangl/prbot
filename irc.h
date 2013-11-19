/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
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

// A meager interface to the IRC protocol.
// Encapsulates buffer logic.

#include <stdarg.h>
#include <stdbool.h>

#ifndef prbot_irc_h__
#define prbot_irc_h__

enum ircmsgtype {
    IRCMSG_UNKNOWN,
    IRCMSG_PING,
    IRCMSG_PART,
    IRCMSG_JOIN,
    IRCMSG_PRIVMSG,
    IRCMSG_KICK
};

// Represents names such as "foo!~bar@the.host.name".
struct ircname {
    char *nick; // "foo" in the above example.
    char *user; // "bar" in the above example.
    char *host; // "the.host.name" in the above example.
};

// Messages of type IRCMSG_PING.
struct ircmsg_ping {
    char *text;
};

// Messages of type IRCMSG_PART.
struct ircmsg_part {
    struct ircname name;
    char *chan;
};

// Messages of type IRCMSG_JOIN.
struct ircmsg_join {
    struct ircname name;
    char *chan;
};

// Messages of type IRCMSG_PRIVMSG.
struct ircmsg_privmsg {
    struct ircname name;
    char *chan;
    char *text;
};

// Messages of type IRCMSG_KICK.
struct ircmsg_kick {
    struct ircname name;
    char *chan;
    char *kickee;
    char *reason;
};

// Represents generic messages.
struct ircmsg {
    enum ircmsgtype type;
    union {
        struct ircmsg_ping ping;
        struct ircmsg_part part;
        struct ircmsg_join join;
        struct ircmsg_privmsg privmsg;
        struct ircmsg_kick kick;
    } u;
};

// Buffer for incoming network traffic.
struct ircbuf {
    char *buf; // Buffer for incoming messages.
    int max;   // Maximum length of the buffer.
    int count; // Number of written characters from the start of |buf|.

    // Length of the active line in the buffer, or -1.
    // Includes final null-terminator.
    int msglen;
};

void ircbuf_init(struct ircbuf *ircbuf, char *buf, int len);

// Connection functions.
int irc_connect(const char *server, const char *port);
void irc_disconnect(int fd);

// Raw sending functions.
bool irc_vsend(int fd, const char *fmt, va_list argp);
bool irc_send(int fd, const char *fmt, ...);

// Helpful wrappers for the raw sending functions.
bool irc_pong(int fd, const char *response);
bool irc_join(int fd, const char *chan);
bool irc_nick(int fd, const char *nick, const char *passwd);
bool irc_privmsg(int fd, const char *chan, const char *fmt, ...);

// Receiving functions.
char *irc_getline(int fd, struct ircbuf *ircbuf);
void irc_parseline(char *line, struct ircmsg *msg);

#endif // prbot_irc_h__
