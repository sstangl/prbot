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

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include "irc.h"

void
ircbuf_init(struct ircbuf *ircbuf, char *buf, int len)
{
    ircbuf->buf = buf;
    ircbuf->max = len;
    ircbuf->count = 0;
    ircbuf->msglen = -1;
}

bool
irc_vsend(int fd, const char *fmt, va_list argp)
{
    char buf[1024];

    int len = vsnprintf(buf, 1024, fmt, argp);
    assert(len >= 0 && len < 1024);
    assert(fmt[strlen(fmt) - 2] == '\r');
    assert(fmt[strlen(fmt) - 1] == '\n');

    int written = write(fd, buf, len);
    assert(written == len);
    return true;
}

bool
irc_send(int fd, const char *fmt, ...)
{
    va_list argp;
    va_start(argp, fmt);
    bool ret = irc_vsend(fd, fmt, argp);
    va_end(argp);
    return ret;
}

bool
irc_pong(int fd, const char *response)
{
    return irc_send(fd, "PONG :%s\r\n", response);
}

bool
irc_join(int fd, const char *chan)
{
    return irc_send(fd, "JOIN %s\r\n", chan);
}

bool
irc_nick(int fd, const char *nick, const char *passwd)
{
    assert(passwd == NULL); // Unhandled, for now.
    assert(strlen(nick) <= 30);
    return irc_send(fd, "NICK %s\r\nUSER %s 0 * : %s\r\n", nick, nick, nick);
}

bool
irc_privmsg(int fd, const char *chan, const char *fmt, ...)
{
    int len = 0;
    char buf[1024];
    int written;

    // Write the header boilerplate.
    written = snprintf(buf, 1024, "PRIVMSG %s :", chan);
    if (written < 0 || written >= 1024)
        return false;
    len += written;

    // Add the user message.
    va_list argp;
    va_start(argp, fmt);
    written = vsnprintf(buf + len, 1024 - len, fmt, argp);
    va_end(argp);

    if (written < 0 || written >= 1024 - len)
        return false;
    len += written;

    // Finish with a newline.
    written = snprintf(buf + len, 1024 - len, "\r\n");
    if (written < 0 || written >= 1024 - len)
        return false;
    len += written;

    // Send buffer to server.
    printf("Sending: %s", buf);
    written = write(fd, buf, len);
    assert(written == len);
    return true;
}

// Constructs a file descriptor with an open connection to the specified server.
int
irc_connect(const char *server, const char *port)
{
    struct addrinfo hints;
    struct addrinfo *serverlist;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; // IPv4.
    hints.ai_socktype = SOCK_STREAM; // TCP.

    // Get linked list of server candidates.
    int status = getaddrinfo(server, port, &hints, &serverlist);
    if (status) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(status));
        return -1;
    }

    // Establish a connection to the first server that will accept.
    struct addrinfo *s;
    for (s = serverlist; s != NULL; s = s->ai_next) {
        int fd = socket(s->ai_family, s->ai_socktype, s->ai_protocol);
        if (fd < 0)
            continue;

        if (connect(fd, s->ai_addr, s->ai_addrlen) == 0) {
            freeaddrinfo(serverlist);
            return fd;
        }

        close(fd);
    }

    freeaddrinfo(serverlist);
    return -1;
}

void
irc_disconnect(int fd)
{
    close(fd);
}

static int
find_whole_line(char *buf, int len)
{
    for (int c = 0; c < len; c++) {
        if (buf[c] == '\n') {
            if (c > 0 && buf[c] == '\r')
                buf[c - 1] = '\0';
            buf[c] = '\0';
            return c; // String length.
        }
    }
    return -1;
}

static void
discardline(struct ircbuf *ircbuf)
{
    int msglen = ircbuf->msglen;
    assert(msglen >= 0);
    assert(ircbuf->buf[msglen] == '\0');

    memmove(ircbuf->buf, &ircbuf->buf[msglen + 1], ircbuf->count - (msglen + 1));
    ircbuf->count -= (msglen + 1);
    ircbuf->msglen = -1;
}

// Blocks until a full line is received from the server.
// Returned line is kept in the buffer; length is remembered via ircbuf->msglen.
char *
irc_getline(int fd, struct ircbuf *ircbuf)
{
    char *buf = ircbuf->buf;

    // If a line was previously returned, remove it from the buffer.
    if (ircbuf->msglen > 0)
        discardline(ircbuf);

    int msglen = find_whole_line(buf, ircbuf->count);
    if (msglen >= 0) {
        ircbuf->msglen = msglen;
        return buf;
    }

    while (ircbuf->count < ircbuf->max) {
        int bytes = read(fd, buf + ircbuf->count, ircbuf->max - ircbuf->count);
        if (bytes == 0) {
            fprintf(stderr, "Connection closed by remote host.\n");
            return NULL;
        }
        if (bytes < 0) {
            perror("irc_getline():");
            return NULL;
        }

        int origcount = ircbuf->count;
        ircbuf->count += bytes;

        msglen = find_whole_line(buf + origcount, bytes);
        if (msglen >= 0) {
            ircbuf->msglen = origcount + msglen;
            return buf;
        }
    }

    // Buffer full without a newline; currently unhandled. 
    // In the future, this function should clear the buffer and pretend like
    // the extremely long line never occurred.
    // I don't think Rizon permits such strings anyway.
    assert(0);
    return NULL;
}

// Parses the name and inserts \0 appropriately.
static bool
parsename(char *name, struct ircname *out)
{
    // Names are formatted:
    // foo!~bar@host.name
    assert(name[0] != ':');
    assert(!strchr(name, ' '));

    char *exclam = strchr(name, '!');
    char *atsign = strchr(name, '@');
    if (!exclam || !atsign || atsign < exclam)
        return false;

    *exclam = '\0';
    *atsign = '\0';

    out->nick = name;
    out->user = exclam + 1;
    out->host = atsign + 1;
    return true;
}

static bool
try_parseping(char *line, struct ircmsg *msg)
{
    // PING messages are formatted:
    // PING :responsetext
    if (strlen(line) < 7 || strstr(line, "PING :") != line)
        return false;
    
    msg->type = IRCMSG_PING;
    msg->u.ping.text = &line[6]; // First character after ':'.
    return true;
}

static bool
try_parsepart(char *line, struct ircmsg *msg)
{
    // PART messages are formatted:
    // :foo!~bar@host.name PART #channel
    if (line[0] != ':')
        return false;

    char *space = strchr(line, ' ');
    if (!space)
        return false;

    char *part = space + 1;
    if (strstr(part, "PART #") != part)
        return false;

    // Failure after this point implies a malformed message.
    *space = '\0';
    if (!parsename(line + 1, &msg->u.part.name)) {
        msg->type = IRCMSG_UNKNOWN;
        return true;
    }

    msg->u.part.chan = part + strlen("PART ");
    msg->type = IRCMSG_PART;
    return true;
}

static bool
try_parsejoin(char *line, struct ircmsg *msg)
{
    // JOIN messages are formatted:
    // :foo!~bar@host.name JOIN :#channel
    if (line[0] != ':')
        return false;

    char *space = strchr(line, ' ');
    if (!space)
        return false;

    char *join = space + 1;
    if (strstr(join, "JOIN :#") != join)
        return false;

    // Failure after this point implies a malformed message.
    *space = '\0';
    if (!parsename(line + 1, &msg->u.join.name)) {
        msg->type = IRCMSG_UNKNOWN;
        return true;
    }

    msg->u.join.chan = join + strlen("JOIN :");
    msg->type = IRCMSG_JOIN;
    return true;
}

static bool
try_parseprivmsg(char *line, struct ircmsg *msg)
{
    // PRIVMSG messages are formatted:
    // :foo!~bar@host.name PRIVMSG channel :text
    if (line[0] != ':')
        return false;

    char *space = strchr(line, ' ');
    if (!space)
        return false;

    char *privmsg = space + 1;
    if (strstr(privmsg, "PRIVMSG ") != privmsg)
        return false;

    char *chan = privmsg + strlen("PRIVMSG ");
    char *chanspace = strchr(chan, ' ');
    if (!chanspace || chanspace[1] != ':')
        return false;

    // Failure after this point implies a malformed message.
    *space = '\0';
    if (!parsename(line + 1, &msg->u.privmsg.name)) {
        msg->type = IRCMSG_UNKNOWN;
        return true;
    }

    *chanspace = '\0';
    msg->u.privmsg.chan = chan;
    msg->u.privmsg.text = chanspace + strlen(" :");
    msg->type = IRCMSG_PRIVMSG;
    return true;
}

static bool
try_parsekick(char *line, struct ircmsg *msg)
{
    // KICK messages are formatted:
    // :foo!~bar@host.name KICK channel kickee :reason
    if (line[0] != ':')
        return false;

    char *space = strchr(line, ' ');
    if (!space)
        return false;

    char *kick = space + 1;
    if (strstr(kick, "KICK ") != kick)
        return false;

    char *chan = kick + strlen("KICK ");
    char *chanspace = strchr(chan, ' ');
    if (!chanspace)
        return false;

    char *kickee = chanspace + 1;
    char *kickeespace = strchr(kickee, ' ');
    if (!kickeespace || kickeespace[1] != ':')
        return false;

    char *reason = kickeespace + strlen(" :");

    // Failure after this point implies a malformed message.
    *space = '\0';
    if (!parsename(line + 1, &msg->u.kick.name)) {
        msg->type = IRCMSG_UNKNOWN;
        return true;
    }

    *chanspace = '\0';
    msg->u.kick.chan = chan;

    *kickeespace = '\0';
    msg->u.kick.kickee = kickee;

    msg->u.kick.reason = reason;
    msg->type = IRCMSG_KICK;
    return true;
}

void
irc_parseline(char *line, struct ircmsg *msg)
{
    try_parseping(line, msg)
    || try_parsepart(line, msg)
    || try_parsejoin(line, msg)
    || try_parseprivmsg(line, msg)
    || try_parsekick(line, msg)
    || (msg->type = IRCMSG_UNKNOWN);
}
