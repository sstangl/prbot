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
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sqlite3.h>
#include <regex.h>

#include "irc.h"

#define BUF_LEN 1024

#define DATABASE_NAME "prbot.sqlite3"
#define IRC_HOST "irc.rizon.net"
#define IRC_PORT "6667"
#define IRC_NICK "prbot"
#define IRC_CHANNEL "#prbottest"

static const char INITIALIZE_DB[] =
    "CREATE TABLE IF NOT EXISTS prs ("
    "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "    nick VARCHAR(255) NOT NULL,"
    "    lift VARCHAR(255) NOT NULL,"
    "    date INTEGER NOT NULL,"
    "    sets INTEGER NOT NULL,"
    "    reps INTEGER NOT NULL,"
    "    kgs REAL NOT NULL"
    ");";

static const char TOP_PRS[] =
    "SELECT * "
    "FROM "
    "   (SELECT nick, lift, date, sets, reps, kgs"
    "    FROM prs"
    "    WHERE nick = ?"
    "    ORDER BY date DESC) "
    "GROUP BY lift, nick "
    "ORDER BY lift ASC;";

static const char INSERT_PR[] =
    "INSERT INTO prs (nick, lift, date, sets, reps, kgs)"
    "VALUES (?, ?, ?, ?, ?, ?)";

static const char *LIFTS[] = {
    "bench press",
    "overhead press",
    "squat",
    "front squat",
    "power clean"
};

struct prbot_pr {
    char *nick;
    char *lift;
    time_t date;
    int sets;
    int reps;
    double kgs;
};

static double inline
kg2lb(double kgs)
{
    return kgs * 2.205;
}

static double inline
lb2kg(double lbs)
{
    return lbs / 2.205;
}

// Global database handle ( :( ).
static sqlite3 *db;

static bool
insert_pr(struct prbot_pr *pr)
{
    sqlite3_stmt *stmt;
    if (sqlite3_prepare(db, INSERT_PR, -1, &stmt, NULL) != SQLITE_OK) {
        // Something broke. :(
        return false;
    }
    sqlite3_bind_text(stmt, 1, pr->nick, -1, NULL);
    sqlite3_bind_text(stmt, 2, pr->lift, -1, NULL);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64) pr->date);
    sqlite3_bind_int(stmt, 4, pr->sets);
    sqlite3_bind_int(stmt, 5, pr->reps);
    sqlite3_bind_double(stmt, 6, pr->kgs);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        // Couldn't run this statement.
        return false;
    }
    if (sqlite3_finalize(stmt)) {
        // Couldn't finalize statement.
        return false;
    }
    return true;
}

static const char NEW_PR_PATTERN[] = "^(.+) of ([0-9]+)(\\.[0-9]+)?(kg|lb) ([0-9]+)x([0-9]+)";
static regex_t new_pr_regex;

static bool
tryparse_pr(char *msg, struct prbot_pr *pr)
{
#define NUM_MATCHES 7
    // There are 7 match groups:
    //
    // 0. full string
    // 1. lift
    // 2. weight (whole part)
    // 3. weight (decimal part)
    // 4. unit
    // 5. sets
    // 6. reps
    regmatch_t matches[NUM_MATCHES];

    if (regexec(&new_pr_regex, msg, NUM_MATCHES, matches, 0)) {
        return false;
    }

    // The regexp will match the full unit, so we can just predicate on the
    // first character (l for lb, k for kg).
    bool needs_conv_from_lb = msg[matches[4].rm_so] == 'l';

    // Null-terminate some parts of the string so they can be parsed.
    for (size_t i = 0; i < NUM_MATCHES; ++i) {
        if (i == 2) {
            // But don't null-terminate the whole weight.
            continue;
        }
        msg[matches[i].rm_eo] = '\0';
    }
#undef NUM_MATCHES

    pr->lift = msg + matches[1].rm_so;
    for (char *c = pr->lift; *c != '\0'; ++c) {
        *c = tolower(*c);
    }
    pr->kgs = atof(msg + matches[2].rm_so);
    if (needs_conv_from_lb) {
        pr->kgs = lb2kg(pr->kgs);
    }
    pr->sets = atoi(msg + matches[5].rm_so);
    pr->reps = atoi(msg + matches[6].rm_so);
    return true;
}

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
handle_cmd_record(int fd, struct ircmsg_privmsg *msg, char *head)
{
    struct prbot_pr pr;

    if (!tryparse_pr(head, &pr)) {
        irc_privmsg(fd, msg->chan, "%s: check your syntax, expected: "
                                   "<lift> of <weight><unit> <sets>x<reps>",
                    msg->name.nick);
        return false;
    }

    bool lift_ok = false;
    for (size_t i = 0; i < sizeof LIFTS / sizeof LIFTS[0]; ++i) {
        if (strcmp(LIFTS[i], pr.lift) == 0) {
            lift_ok = true;
            break;
        }
    }

    if (!lift_ok) {
        irc_privmsg(fd, msg->chan, "%s: sorry, I don't think \"%s\" is a real lift",
                    msg->name.nick, pr.lift);
        return false;
    }

    // Normalize nicknames to lowercase, so we don't get duplicates of nicknames.
    char nick_lower[BUF_LEN];
    strcpy(nick_lower, msg->name.nick);

    for (char *c = nick_lower; *c != '\0'; ++c) {
        *c = tolower(*c);
    }

    // TODO: remove me later and use a proper verification thing
    if (strcmp(nick_lower, "number1stunna")) {
        irc_privmsg(fd, msg->chan, "%s: haha, no.",
                    msg->name.nick);
        return false;
    }

    pr.nick = nick_lower;
    pr.date = time(NULL);

    if (!insert_pr(&pr)) {
        irc_privmsg(fd, msg->chan, "%s: couldn't record your PR, try again later :(",
                    msg->name.nick);
        return false;
    }

    irc_privmsg(fd, msg->chan, "%s: recorded your PR for %s of %.2fkg %dx%d",
                msg->name.nick, pr.lift, pr.kgs, pr.sets, pr.reps);

    return true;
}

static bool
handle_cmd_records(int fd, struct ircmsg_privmsg *msg, char *head) {
    // Normalize the nickname to lowercase, because that keeps the database
    // consistent (as is done in other places).
    for (char *c = head; *c != '\0'; ++c) {
        *c = tolower(*c);
        // Also turn the trailing CR/LF to NUL so we can use the nickname as a
        // null-terminated string.
        if (*c == '\r' || *c == '\n') {
            *c = '\0';
            break;
        }
    }

    sqlite3_stmt *stmt;
    char out[BUF_LEN] = "\0";
    char *cur = out;

    if (sqlite3_prepare(db, TOP_PRS, -1, &stmt, NULL) != SQLITE_OK) {
        // Something broke. :(
        irc_privmsg(fd, msg->chan, "%s: sorry, couldn't get PRs (prepare)");
        return false;
    }

    sqlite3_bind_text(stmt, 1, head, -1, NULL);

    int retval;
    do {
        switch ((retval = sqlite3_step(stmt))) {
            case SQLITE_DONE:
                break;

            case SQLITE_ROW: {
                // There are 6 columns:
                //
                // 0. nick
                // 1. lift
                // 2. date
                // 3. sets
                // 4. reps
                // 5. kgs
                const char *nick = (const char *) sqlite3_column_text(stmt, 0);
                const char *lift = (const char *) sqlite3_column_text(stmt, 1);
                long date = (long) sqlite3_column_int64(stmt, 2);
                int sets = sqlite3_column_int(stmt, 3);
                int reps = sqlite3_column_int(stmt, 4);
                double kgs = sqlite3_column_double(stmt, 5);

                int n = snprintf(cur, BUF_LEN - (int) (cur - out), "| %s of %.2fkg %dx%d ", lift, kgs, sets, reps);
                if (n < 0) {
                    // TODO: split output over multiple lines, rather than just silencing it
                    goto finalize;
                }
                cur += n;

                break;
           }

            default:
                // Some error occured during PR retrieval.
                irc_privmsg(fd, msg->chan, "%s: sorry, couldn't get PRs (iterate)");
                return false;
        }
    } while (retval == SQLITE_ROW);

finalize:
    if (sqlite3_finalize(stmt)) {
        // Couldn't finalize statement.
        irc_privmsg(fd, msg->chan, "%s: sorry, couldn't get PRs (finalize)");
        return false;
    }

    irc_privmsg(fd, msg->chan, "PRs for %s %s",
                head, out[0] == '\0' ? "| none" : out);
    return false;
}

static bool
handle_privmsg(int fd, struct ircmsg_privmsg *msg)
{
    // Only handle messages directed at the bot.
    if (strncmp(msg->text, IRC_NICK, strlen(IRC_NICK)) != 0)
        return true;

    // Only handle messages in a channel.
    if (msg->chan[0] != '#')
        return true;

    char *head = msg->text + strlen(IRC_NICK ": ");

    if (strstr(head, "record ") == head)
        handle_cmd_record(fd, msg, head + 7);
    else if (strstr(head, "records ") == head)
        handle_cmd_records(fd, msg, head + 8);
    else
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
    // Compile some regexes.
    if (regcomp(&new_pr_regex, NEW_PR_PATTERN, REG_EXTENDED)) {
        fprintf(stderr, "Failed to compile regex.\n");
        return 1;
    }

    // Initialize SQLite gunk.
    if (sqlite3_open(DATABASE_NAME, &db)) {
        fprintf(stderr, "Failed to open database: %s\n", sqlite3_errmsg(db));
        return 1;
    }

    if (sqlite3_exec(db, INITIALIZE_DB, 0, 0, 0)) {
        fprintf(stderr, "Failed to initialize database: %s\n", sqlite3_errmsg(db));
        return 1;
    }
 
    // Kick off the IRC connection.
    char buf[BUF_LEN];
    struct ircbuf ircbuf;
    ircbuf_init(&ircbuf, buf, BUF_LEN);

    int fd = irc_connect(IRC_HOST, IRC_PORT);
    if (fd < 0) {
        fprintf(stderr, "Failed to open connection.\n");
        return 1;
    }

    irc_nick(fd, IRC_NICK, NULL);
    irc_join(fd, IRC_CHANNEL);

    char *line;
    while ((line = irc_getline(fd, &ircbuf))) {
        printf("%s\n", line);

        struct ircmsg msg;
        irc_parseline(line, &msg);
        if (!dispatch_handler(fd, &msg))
            return 2;
    }

    return 0;
}
