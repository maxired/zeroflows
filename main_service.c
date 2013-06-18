#ifndef G_LOG_DOMAIN
# define G_LOG_DOMAIN "zs.srv"
#endif

#include <unistd.h>
#include <string.h>
#include <errno.h>

#include <glib.h>
#include <zmq.h>
#include <zookeeper.h>

#include "./zreactor.h"
#include "./zsock.h"
#include "./common.h"

struct zsrv_env_s ctx;
struct zsock_s *zs_in0;
struct zsock_s *zs_in1;
struct zsock_s *zs_out0;
struct zsock_s *zs_out1;

static void
_skip_tail(struct zsock_s *zs)
{
    guint count;

    g_assert(zs != NULL);
    g_assert(zs->zs != NULL);

    for (count=0;  ;++count) {
        zmq_msg_t msg;
        zmq_msg_init(&msg);
        int rc = zmq_msg_recv(&msg, zs->zs, ZMQ_NOBLOCK);
        if (rc < 0)
            break;
        //g_debug("ZSOCK [%s] -> %d [%.*s]", zs->fullname, rc,
        //        (int) zmq_msg_size(&msg), (char*) zmq_msg_data(&msg));
        zmq_msg_close(&msg);
    }
    //g_debug("ZSOCK [%s] skip %u", zs->fullname, count);
}

static void
_on_event_in0(struct zsock_s *zs)
{
    _skip_tail(zs);
}

static void
_on_event_in1(struct zsock_s *zs)
{
    _skip_tail(zs);
}

static void
_on_event_out0(struct zsock_s *zs)
{
    g_debug("ZSOCK [%s] ready for output", zs->fullname);
}

static void
_on_event_out1(struct zsock_s *zs)
{
    g_debug("ZSOCK [%s] ready for output", zs->fullname);
}

static void
sighandler_stop(int s)
{
    zreactor_stop(ctx.zenv.zr);
    signal(s, sighandler_stop);
}

static void
_on_zservice_configured(struct zservice_s *zsrv, gpointer u)
{
    (void) u;
    g_debug("ZSRV configured, now applying event handlers");

    zs_in0 = zservice_get_socket(zsrv, "in0");
    zs_in0->ready_in = _on_event_in0;
    zs_in0->evt = ZMQ_POLLIN;

    zs_in1 = zservice_get_socket(zsrv, "in1");
    zs_in1->ready_in = _on_event_in1;
    zs_in1->evt = ZMQ_POLLIN;

    zs_out0 = zservice_get_socket(zsrv, "out0");
    zs_out0->ready_out = _on_event_out0;
    zs_out0->evt = ZMQ_POLLOUT;

    zs_out1 = zservice_get_socket(zsrv, "out1");
    zs_out1->ready_out = _on_event_out1;
    zs_out1->evt = ZMQ_POLLOUT;
}

int
main(int argc, char **argv)
{
    main_set_log_handlers();
    if (argc < 2) {
        g_error("Usage: %s SRVTYPE", argv[0]);
        return 1;
    }

    zsrv_env_init(argv[1], &ctx);
    zs_in0 = zs_in1 = zs_out0 = zs_out1 = NULL;

    signal(SIGTERM, sighandler_stop);
    signal(SIGQUIT, sighandler_stop);
    signal(SIGINT, sighandler_stop);

    zservice_on_config(ctx.zsrv, ctx.zsrv, _on_zservice_configured);

    int rc = zreactor_run(ctx.zenv.zr);
    zsrv_env_close(&ctx);
    return rc != 0;
}

