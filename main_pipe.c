#ifndef G_LOG_DOMAIN
# define G_LOG_DOMAIN "zs.cli"
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include <glib.h>
#include <zmq.h>
#include <zookeeper.h>

#include "./macros.h"
#include "./zreactor.h"
#include "./zsock.h"
#include "./common.h"

static struct zclt_env_s ctx;
static int in_evt = 0;

static void
_manage_out(struct zsock_s *zs)
{
    in_evt = ZMQ_POLLIN;
    g_debug("ZSOCK [%s] ready for output", zs->fullname);
}

static inline void
_wait_for_output(struct zsock_s *zs)
{
    zs->evt = ZMQ_POLLOUT;
    zs->ready_out = _manage_out;
}

static int
on_input(void *c, int fd, int evt)
{
    FILE *in = c;
    gchar *s, b[4096];
    (void) fd, (void) evt;

    if (!zsock_ready(ctx.zsock)) {
        g_debug("Output not ready");
        in_evt = 0;
        _wait_for_output(ctx.zsock);
        return 0;
    }

    if (ferror(in) || feof(in)) {
        g_debug("EOF!");
        zreactor_stop(ctx.zenv.zr);
        return -1;
    }

    if (!(s = fgets(b, sizeof(b), in))) {
        g_debug("EOF!");
        zreactor_stop(ctx.zenv.zr);
        return -1;
    }

    zmq_msg_t msg;
    int l = strlen(s);
    for (; g_ascii_isspace(s[l-1]) ;--l) {}
    zmq_msg_init_size(&msg, l);
    memcpy(zmq_msg_data(&msg), s, zmq_msg_size(&msg));
    int rc = zmq_sendmsg(ctx.zsock->zs, &msg, ZMQ_DONTWAIT);
    zmq_msg_close(&msg);

    (void) rc;
    ASSERT(rc == l);
    return 0;
}

static void
sighandler_stop(int s)
{
    zreactor_stop(ctx.zenv.zr);
    signal(s, sighandler_stop);
}

int
main(int argc, char **argv)
{
    main_set_log_handlers();
    if (argc < 3) {
        g_error("Usage: %s ZTYPE TARGET", argv[0]);
        return 1;
    }

    zclt_env_init(argv[1], argv[2], &ctx);
    signal(SIGTERM, sighandler_stop);
    signal(SIGQUIT, sighandler_stop);
    signal(SIGINT, sighandler_stop);
    _wait_for_output(ctx.zsock);

    fcntl(0, F_SETFL, O_NONBLOCK|fcntl(0, F_GETFL));
    zreactor_add_fd(ctx.zenv.zr, 0, &in_evt, on_input, stdin);
    int rc = zreactor_run(ctx.zenv.zr);
    zclt_env_close(&ctx);
    return rc != 0;
}

