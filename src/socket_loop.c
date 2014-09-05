/*
 * Copyright (c) 2014 DeNA Co., Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include "h2o.h"

static void run_socket(h2o_socket_t* sock)
{
    if ((sock->_flags & H2O_SOCKET_FLAG_IS_DISPOSED) != 0) {
        /* is freed in updatestates phase */
        return;
    }

    if (sock->_cb.write != NULL && sock->_wreq.cnt == 0) {
        h2o_socket__write_on_complete(sock, (sock->_flags & H2O_SOCKET_FLAG_IS_WRITE_ERROR) != 0 ? -1 : 0);
    }

    if ((sock->_flags & H2O_SOCKET_FLAG_IS_READ_READY) != 0) {
        sock->_flags &= ~H2O_SOCKET_FLAG_IS_READ_READY;
        h2o_socket__read_on_ready(sock);
    }
}

int h2o_socket_loop_run(h2o_socket_loop_t *loop, uint64_t max_wait_millis)
{
    /* update socket states, poll, set readable flags, perform pending writes */
    if (loop->_proceed(loop, max_wait_millis) != 0)
        return -1;

    /* call the pending callbacks */
    while (loop->_pending.head != NULL) {
        /* detach the first sock and run */
        h2o_socket_t *sock = loop->_pending.head;
        if ((loop->_pending.head = sock->_next_pending) == NULL)
            loop->_pending.tail_ref = &loop->_pending.head;
        sock->_next_pending = sock;
        run_socket(sock);
    }

    return 0;
}

/* select-based impl. starts here */

#include <sys/select.h>

struct st_h2o_socket_loop_select_t {
    h2o_socket_loop_t super;
    fd_set readfds, writefds;
    h2o_socket_t *socks[FD_SETSIZE];
};

static void update_fdset(struct st_h2o_socket_loop_select_t *loop)
{
    while (loop->super._statechanged.head != NULL) {
        /* detach the top */
        h2o_socket_t *sock = loop->super._statechanged.head;
        loop->super._statechanged.head = sock->_next_statechanged;
        sock->_next_statechanged = sock;
        /* update the state */
        if ((sock->_flags & H2O_SOCKET_FLAG_IS_DISPOSED) != 0) {
            assert(loop->socks[sock->fd] == sock);
            loop->socks[sock->fd] = NULL;
            free(sock);
        } else {
            if (loop->socks[sock->fd] == NULL) {
                loop->socks[sock->fd] = sock;
            } else {
                assert(loop->socks[sock->fd] == sock);
            }
            if (h2o_socket_is_reading(sock)) {
//fprintf(stderr, "setting READ for fd: %d\n", sock->fd);
                FD_SET(sock->fd, &loop->readfds);
                sock->_flags |= H2O_SOCKET_FLAG_IS_POLLED_FOR_READ;
            } else {
//fprintf(stderr, "clearing READ for fd: %d\n", sock->fd);
                FD_CLR(sock->fd, &loop->readfds);
                sock->_flags &= ~H2O_SOCKET_FLAG_IS_POLLED_FOR_READ;
            }
            if (h2o_socket_is_writing(sock)) {
//fprintf(stderr, "setting WRITE for fd: %d\n", sock->fd);
                FD_SET(sock->fd, &loop->writefds);
                sock->_flags |= H2O_SOCKET_FLAG_IS_POLLED_FOR_WRITE;
            } else {
//fprintf(stderr, "clearing WRITE for fd: %d\n", sock->fd);
                FD_CLR(sock->fd, &loop->writefds);
                sock->_flags &= ~H2O_SOCKET_FLAG_IS_POLLED_FOR_WRITE;
            }
        }
    }
    loop->super._statechanged.tail_ref = &loop->super._statechanged.head;
}

static int proceed(h2o_socket_loop_t *_loop, uint64_t max_wait_millis)
{
    struct st_h2o_socket_loop_select_t *loop = (struct st_h2o_socket_loop_select_t*)_loop;
    fd_set rfds, wfds;
    struct timeval timeout;
    int fd, ret;

    if (max_wait_millis >= INT32_MAX)
        max_wait_millis = INT32_MAX;

    /* update status */
    update_fdset(loop);

    /* call select */
    do {
        FD_COPY(&loop->readfds, &rfds);
        FD_COPY(&loop->writefds, &wfds);
        timeout.tv_sec = max_wait_millis / 1000;
        timeout.tv_usec = max_wait_millis % 1000 * 1000;
        ret = select(FD_SETSIZE, &rfds, &wfds, NULL, &timeout);
    } while (ret == -1 && errno == EINTR);
    if (ret == -1)
        return -1;
//fprintf(stderr, "select returned: %d\n", ret);

    /* update readable flags, perform writes */
    if (ret > 0) {
        for (fd = 0; fd != FD_SETSIZE; ++fd) {
            /* set read_ready flag before calling the write cb, since app. code invoked by hte latter may close the socket, clearing the former flag */
            if (FD_ISSET(fd, &rfds)) {
                h2o_socket_t *sock = loop->socks[fd];
                assert(sock != NULL);
                if (sock->_flags != H2O_SOCKET_FLAG_IS_DISPOSED) {
                    sock->_flags |= H2O_SOCKET_FLAG_IS_READ_READY;
                    h2o_socket__link_to_pending(sock);
//fprintf(stderr, "added fd %d as read_ready\n", fd);
                }
            }
            if (FD_ISSET(fd, &wfds)) {
                h2o_socket_t *sock = loop->socks[fd];
                assert(sock != NULL);
                if (sock->_flags != H2O_SOCKET_FLAG_IS_DISPOSED) {
//fprintf(stderr, "handling pending writes on fd %d\n", fd);
                    h2o_socket__write_pending(loop->socks[fd]);
                }
            }
        }
    }

    return 0;
}

static void on_create(h2o_socket_t *sock)
{
    struct st_h2o_socket_loop_select_t *loop = (struct st_h2o_socket_loop_select_t*)sock->loop;

    if (loop->socks[sock->fd] != NULL) {
        assert(loop->socks[sock->fd]->_flags == H2O_SOCKET_FLAG_IS_DISPOSED);
    }
    assert(! FD_ISSET(sock->fd, &loop->readfds));
    assert(! FD_ISSET(sock->fd, &loop->writefds));
}

static void on_close(h2o_socket_t *sock)
{
    struct st_h2o_socket_loop_select_t *loop = (struct st_h2o_socket_loop_select_t*)sock->loop;
    assert(loop->socks[sock->fd] != NULL);
//fprintf(stderr, "clearing READ/WRITE for fd: %d\n", sock->fd);
    FD_CLR(sock->fd, &loop->readfds);
    FD_CLR(sock->fd, &loop->writefds);
}

h2o_socket_loop_t *h2o_socket_loop_create(void)
{
    struct st_h2o_socket_loop_select_t *loop = h2o_malloc(sizeof(*loop));

    memset(loop, 0, sizeof(*loop));
    loop->super._proceed = proceed;
    loop->super._on_create = on_create;
    loop->super._on_close = on_close;
    loop->super._pending.tail_ref = &loop->super._pending.head;
    loop->super._statechanged.tail_ref = &loop->super._statechanged.head;

    return &loop->super;
}
