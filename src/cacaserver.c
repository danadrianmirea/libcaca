/*
 *  cacaserver    Colour ASCII-Art library
 *  Copyright (c) 2006 Jean-Yves Lamoureux <jylam@lnxscene.org>
 *                2006 Sam Hocevar <sam@zoy.org>
 *                All Rights Reserved
 *
 *  $Id$
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the Do What The Fuck You Want To
 *  Public License, Version 2, as published by Sam Hocevar. See
 *  http://sam.zoy.org/wtfpl/COPYING for more details.
 */

#include "config.h"
#include "common.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#if defined(HAVE_ARPA_INET_H)
#   include <arpa/inet.h>
#elif defined(HAVE_NETINET_IN_H)
#   include <netinet/in.h>
#elif defined(HAVE_WINSOCK2_H)
#   include <winsock2.h>
#endif
#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>

#include "cucul.h"
#include "caca.h"

#include "config.h"

#if defined(HAVE_UNISTD_H)
#   include <unistd.h>
#endif

#include <stdarg.h>

#include "caca.h"
#include "cucul.h"

#define BACKLOG 1337     /* Number of pending connections */
#define INBUFFER 32      /* Size of per-client input buffer */
#define OUTBUFFER 300000 /* Size of per-client output buffer */

/* Following vars are static */
#define INIT_PREFIX \
    "\xff\xfb\x01"     /* WILL ECHO */ \
    "\xff\xfb\x03"     /* WILL SUPPRESS GO AHEAD */ \
    "\xff\xfd\x31"     /* DO NAWS */ \
    "\xff\x1f\xfa____" /* SB NAWS */ \
    "\xff\xf0"         /* SE */  \
    "\x1b]2;caca for the network\x07" /* Change window title */ \
    "\x1b[H\x1b[J" /* Clear screen */
    /*"\x1b[?25l"*/ /* Hide cursor */

#define ANSI_PREFIX \
    "\x1b[1;1H" /* move(0,0) */ \
    "\x1b[1;1H" /* move(0,0) again */

#define ANSI_RESET \
    "    " /* Garbage */ \
    "\x1b[?1049h" /* Clear screen */ \
    "\x1b[?1049h" /* Clear screen again */

static char const telnet_commands[16][5] =
{
    "SE  ", "NOP ", "DM  ", "BRK ", "IP  ", "AO  ", "AYT ", "EC  ",
    "EL  ", "GA  ", "SB  ", "WILL", "WONT", "DO  ", "DONT", "IAC "
};

static char const telnet_options[37][5] =
{
    "????", "ECHO", "????", "SUGH", "????", "STTS", "TIMK", "????",
    "????", "????", "????", "????", "????", "????", "????", "????",
    "????", "????", "????", "????", "????", "????", "????", "????",
    "TTYP", "????", "????", "????", "????", "????", "????", "NAWS",
    "TRSP", "RMFC", "LIMO", "????", "EVAR"
};

#define COMMAND_NAME(x) (x>=240)?telnet_commands[x-240]:"????"
#define OPTION_NAME(x) (x<=36)?telnet_options[x]:"????"

struct client
{
    int fd;
    int ready;
    uint8_t inbuf[INBUFFER];
    int inbytes;
    uint8_t outbuf[OUTBUFFER];
    int start, stop;
};

struct server
{
    unsigned int width, height;
    unsigned int port;
    int sockfd;
    struct sockaddr_in my_addr;
    socklen_t sin_size;

    /* Input buffer */
    uint8_t *input;
    unsigned int read;

    char prefix[sizeof(INIT_PREFIX)];

    cucul_canvas_t *canvas;
    cucul_buffer_t *buffer;
    unsigned long int buflen;
    void *bufdata;

    int client_count;
    struct client *clients;

    RETSIGTYPE (*sigpipe_handler)(int);
};

static void manage_connections(struct server *server);
static int send_data(struct server *server, struct client *c);
ssize_t nonblock_write(int fd, void *buf, size_t len);

int main(void)
{
    int i, yes = 1, flags;
    struct server *server;
    char *tmp;

#ifdef HAVE_WINDOWS_H
	WORD winsockVersion;
	WSADATA wsaData;
	winsockVersion = MAKEWORD(1, 1);

	WSAStartup(winsockVersion, &wsaData);
#endif
    server = malloc(sizeof(struct server));

    server->input = malloc(16);
    server->read = 0;

    server->client_count = 0;
    server->clients = NULL;
    server->port = 0xCACA; /* 51914 */

    /* FIXME, handle >255 sizes */
    memcpy(server->prefix, INIT_PREFIX, sizeof(INIT_PREFIX));
    tmp = strstr(server->prefix, "____");
    tmp[0] = (unsigned char) (server->width & 0xff00) >> 8;
    tmp[1] = (unsigned char) server->width & 0xff;
    tmp[2] = (unsigned char) (server->height & 0xff00) >> 8;
    tmp[3] = (unsigned char) server->height & 0xff;

    if((server->sockfd = socket(PF_INET, SOCK_STREAM, 0)) == -1)
    {
        perror("socket");
        return -1;
    }

    if(setsockopt(server->sockfd, SOL_SOCKET,
                  SO_REUSEADDR, &yes, sizeof(int)) == -1)
    {
        perror("setsockopt SO_REUSEADDR");
        return -1;
    }

    server->my_addr.sin_family = AF_INET;
    server-> my_addr.sin_port = htons(server->port);
    server->my_addr.sin_addr.s_addr = INADDR_ANY;
    memset(&(server->my_addr.sin_zero), '\0', 8);

    if(bind(server->sockfd, (struct sockaddr *)&server->my_addr,
             sizeof(struct sockaddr)) == -1)
    {
        perror("bind");
        return -1;
    }

    /* Non blocking socket */
    flags = fcntl(server->sockfd, F_GETFL, 0);
    fcntl(server->sockfd, F_SETFL, flags | O_NONBLOCK);

    if(listen(server->sockfd, BACKLOG) == -1)
    {
        perror("listen");
        return -1;
    }

    server->canvas = NULL;
    server->buffer = NULL;

    /* Ignore SIGPIPE */
    server->sigpipe_handler = signal(SIGPIPE, SIG_IGN);

    fprintf(stderr, "initialised network, listening on port %i\n",
                    server->port);

    /* Main loop */
    for(;;)
    {
        uint8_t *buf = server->input;
        uint32_t width, height;
        unsigned int size;

        /* Manage new connections as this function will be called sometimes
         * more often than display */
        manage_connections(server);

        /* Read data from stdin */
        read(0, buf, 16);

        while(buf[0] != 'C' || buf[1] != 'A' || buf[2] != 'C' || buf[3] != 'A')
        {
            memmove(buf, buf + 1, 15);
            read(0, buf + 15, 1);
        }

        width = ((uint32_t)buf[8] << 24) | ((uint32_t)buf[9] << 16)
               | ((uint32_t)buf[10] << 8) | (uint32_t)buf[11];
        height = ((uint32_t)buf[12] << 24) | ((uint32_t)buf[13] << 16)
                | ((uint32_t)buf[14] << 8) | (uint32_t)buf[15];

        size = 16 + width * height * 8;
        buf = server->input = realloc(server->input, size);
        read(0, buf + 16, size - 16);

        /* Free the previous canvas, if any */
        if(server->canvas)
            cucul_free_canvas(server->canvas);

        server->canvas = cucul_import_canvas(buf, size, "caca");

        if(!server->canvas)
            continue; /* Load error */

        /* Free the previous export buffer, if any */
        if(server->buffer)
        {
            cucul_free_buffer(server->buffer);
            server->buffer = NULL;
        }

        /* Get ANSI representation of the image and skip the end-of buffer
         * linefeed ("\r\n", 2 bytes) */
        server->buffer = cucul_export_canvas(server->canvas, "ansi");
        server->bufdata = cucul_get_buffer_data(server->buffer);
        server->buflen = cucul_get_buffer_size(server->buffer);
        server->buflen -= 2;

        for(i = 0; i < server->client_count; i++)
        {
            if(server->clients[i].fd == -1)
                continue;

            if(send_data(server, &server->clients[i]))
            {
                fprintf(stderr, "client %i dropped connection\n",
                                server->clients[i].fd);
                close(server->clients[i].fd);
                server->clients[i].fd = -1;
            }
        }
    }

    /* Kill all remaining clients */
    for(i = 0; i < server->client_count; i++)
    {
        if(server->clients[i].fd == -1)
            continue;

        close(server->clients[i].fd);
        server->clients[i].fd = -1;
    }

    if(server->buffer)
        cucul_free_buffer(server->buffer);

    /* Restore SIGPIPE handler */
    signal(SIGPIPE, server->sigpipe_handler);

    free(server);

#ifdef HAVE_WINDOWS_H
    WSACleanup();
#endif
    return 0;
}

/*
 * XXX: The following functions are local
 */

static void manage_connections(struct server *server)
{
    int fd, flags;
    struct sockaddr_in remote_addr;
    socklen_t len = sizeof(struct sockaddr_in);

    fd = accept(server->sockfd, (struct sockaddr *)&remote_addr, &len);
    if(fd == -1)
        return;

    fprintf(stderr, "client %i connected from %s\n",
                    fd, inet_ntoa(remote_addr.sin_addr));

    /* Non blocking socket */
    flags = fcntl(fd, F_SETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    if(server->clients == NULL)
    {
        server->clients = malloc(sizeof(struct client));
        if(server->clients == NULL)
            return;
    }
    else
    {
        server->clients = realloc(server->clients,
                            (server->client_count+1) * sizeof(struct client));
    }

    server->clients[server->client_count].fd = fd;
    server->clients[server->client_count].ready = 0;
    server->clients[server->client_count].inbytes = 0;
    server->clients[server->client_count].start = 0;
    server->clients[server->client_count].stop = 0;

    /* If we already have data to send, send it to the new client */
    if(send_data(server, &server->clients[server->client_count]))
    {
        fprintf(stderr, "client %i dropped connection\n", fd);
        close(fd);
        server->clients[server->client_count].fd = -1;
        return;
    }

    server->client_count++;
}

static int send_data(struct server *server, struct client *c)
{
    ssize_t ret;

    /* Not for us */
    if(c->fd < 0)
        return -1;

    /* Listen to incoming data */
    for(;;)
    {
        ret = read(c->fd, c->inbuf + c->inbytes, 1);
        if(ret <= 0)
            break;

        c->inbytes++;

        /* Check for telnet sequences */
        if(c->inbuf[0] == 0xff)
        {
            if(c->inbytes == 1)
            {
                ;
            }
            else if(c->inbuf[1] == 0xfd || c->inbuf[1] == 0xfc)
            {
                if(c->inbytes == 3)
                {
                    fprintf(stderr, "client %i said: %.02x %.02x %.02x (%s %s %s)\n",
                            c->fd, c->inbuf[0], c->inbuf[1], c->inbuf[2],
                            COMMAND_NAME(c->inbuf[0]), COMMAND_NAME(c->inbuf[1]), OPTION_NAME(c->inbuf[2]));
                    /* Just ignore, lol */
                    c->inbytes = 0;
                }
            }
            else
                c->inbytes = 0;
        }
        else if(c->inbytes == 1)
        {
            if(c->inbuf[0] == 0x03)
            {
                fprintf(stderr, "client %i pressed C-c\n", c->fd);
                return -1; /* User requested to quit */
            }

            c->inbytes = 0;
        }
    }

    /* Send the telnet initialisation commands */
    if(!c->ready)
    {
        ret = nonblock_write(c->fd, server->prefix, sizeof(INIT_PREFIX));
        if(ret == -1)
            return (errno == EAGAIN) ? 0 : -1;

        if(ret < (ssize_t)sizeof(INIT_PREFIX))
            return 0;

        c->ready = 1;
    }

    /* No error, there's just nothing to send yet */
    if(!server->buffer)
        return 0;

    /* If we have backlog, send the backlog */
    if(c->stop)
    {
        ret = nonblock_write(c->fd, c->outbuf + c->start, c->stop - c->start);

        if(ret == -1)
        {
            if(errno == EAGAIN)
                ret = 0;
            else
            {
                fprintf(stderr, "client %i failed (%s)\n",
                        c->fd, strerror(errno));
                return -1;
            }
        }

        if(ret == c->stop - c->start)
        {
            /* We got rid of the backlog! */
            c->start = c->stop = 0;
        }
        else
        {
            c->start += ret;

            if(c->stop - c->start + strlen(ANSI_PREFIX) + server->buflen
                > OUTBUFFER)
            {
                /* Overflow! Empty buffer and start again */
                memcpy(c->outbuf, ANSI_RESET, strlen(ANSI_RESET));
                c->start = 0;
                c->stop = strlen(ANSI_RESET);
                return 0;
            }

            /* Need to move? */
            if(c->stop + strlen(ANSI_PREFIX) + server->buflen > OUTBUFFER)
            {
                memmove(c->outbuf, c->outbuf + c->start, c->stop - c->start);
                c->stop -= c->start;
                c->start = 0;
            }

            memcpy(c->outbuf + c->stop, ANSI_PREFIX, strlen(ANSI_PREFIX));
            c->stop += strlen(ANSI_PREFIX);
            memcpy(c->outbuf + c->stop, server->bufdata, server->buflen);
            c->stop += server->buflen;

            return 0;
        }
    }

    /* We no longer have backlog, send our new data */

    /* Send ANSI prefix */
    ret = nonblock_write(c->fd, ANSI_PREFIX, strlen(ANSI_PREFIX));
    if(ret == -1)
    {
        if(errno == EAGAIN)
            ret = 0;
        else
        {
            fprintf(stderr, "client %i failed (%s)\n", c->fd, strerror(errno));
            return -1;
        }
    }

    if(ret < (ssize_t)strlen(ANSI_PREFIX))
    {
        if(strlen(ANSI_PREFIX) + server->buflen > OUTBUFFER)
        {
            /* Overflow! Empty buffer and start again */
            memcpy(c->outbuf, ANSI_RESET, strlen(ANSI_RESET));
            c->start = 0;
            c->stop = strlen(ANSI_RESET);
            return 0;
        }

        memcpy(c->outbuf, ANSI_PREFIX, strlen(ANSI_PREFIX) - ret);
        c->stop = strlen(ANSI_PREFIX) - ret;
        memcpy(c->outbuf + c->stop, server->bufdata, server->buflen);
        c->stop += server->buflen;

        return 0;
    }

    /* Send actual data */
    ret = nonblock_write(c->fd, server->bufdata, server->buflen);
    if(ret == -1)
    {
        if(errno == EAGAIN)
            ret = 0;
        else
        {
            fprintf(stderr, "client %i failed (%s)\n", c->fd, strerror(errno));
            return -1;
        }
    }

    if(ret < (int)server->buflen)
    {
        if(server->buflen > OUTBUFFER)
        {
            /* Overflow! Empty buffer and start again */
            memcpy(c->outbuf, ANSI_RESET, strlen(ANSI_RESET));
            c->start = 0;
            c->stop = strlen(ANSI_RESET);
            return 0;
        }

        memcpy(c->outbuf, server->bufdata, server->buflen - ret);
        c->stop = server->buflen - ret;

        return 0;
    }

    return 0;
}

ssize_t nonblock_write(int fd, void *buf, size_t len)
{
    size_t total = 0;
    ssize_t ret;

    while(total < len)
    {
        do
        {
            ret = write(fd, buf, len);
        }
        while(ret < 0 && errno == EINTR);

        if(ret < 0)
            return ret;
        else if(ret == 0)
            return total;

        total += len;
        buf = (void *)((uintptr_t)buf + len);
    }

    return total;
}

