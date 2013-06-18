#include <errno.h>

#include <glib.h>

#include "./macros.h"
#include "./zreactor.h"

struct zreactor_s
{
    GArray *items;  
    GArray *monitors;
    gboolean running;
};

struct zmon_s
{
    enum zmon_type_e { ZMT_ZMQ, ZMT_ZK, ZMT_FD } type;

    union {
        zhandle_t *zh; // zookeeper handle

        struct { // file descriptor + handler
            int *evt;
            void *ctx;
            zreactor_fn_fd handler;
            int fd;
        } fd;

        struct { // 0MQ socket + handler
            int *evt;
            void *ctx;
            zreactor_fn_zmq handler;
            void *sock;
        } zmq;
    } data;
};

//------------------------------------------------------------------------------

struct zreactor_s *
zreactor_create(void)
{
    struct zreactor_s *zr = g_malloc0(sizeof(struct zreactor_s));
    zr->items = g_array_new(FALSE, FALSE, sizeof(zmq_pollitem_t));
    zr->monitors = g_array_new(FALSE, FALSE, sizeof(struct zmon_s));
    zr->running = TRUE;
    return zr;
}

void
zreactor_destroy(struct zreactor_s *zr)
{
    if (!zr)
        return;
    if (zr->items)
        g_array_free(zr->items, TRUE);
    if (zr->monitors)
        g_array_free(zr->monitors, TRUE);
    g_free(zr);
}

void
zreactor_stop(struct zreactor_s *zr)
{
    ASSERT(zr != NULL);
    zr->running = FALSE;
}

void
zreactor_add_zk(struct zreactor_s *zr, zhandle_t *zh)
{
    struct zmon_s mon;
    zmq_pollitem_t item = {NULL,-1,0,0};

    mon.type = ZMT_ZK;
    mon.data.zh = zh;
    g_array_prepend_vals(zr->monitors, &mon, 1);
    g_array_prepend_vals(zr->items, &item, 1);
}

void
zreactor_add_zmq(struct zreactor_s *zr, void *s, int *evt,
        zreactor_fn_zmq fn, gpointer fnu)
{
    struct zmon_s mon;
    zmq_pollitem_t item = {NULL,-1,0,0};

    mon.type = ZMT_ZMQ;
    mon.data.zmq.evt = evt;
    mon.data.zmq.ctx = fnu;
    mon.data.zmq.handler = fn;
    mon.data.zmq.sock = s;
    g_array_append_vals(zr->monitors, &mon, 1);

    item.socket = s;
    item.fd = -1;
    g_array_append_vals(zr->items, &item, 1);
}

void
zreactor_add_fd(struct zreactor_s *zr, int fd, int *evt,
        zreactor_fn_fd fn, gpointer fnu)
{
    struct zmon_s mon;
    zmq_pollitem_t item = {NULL,-1,0,0};

    mon.type = ZMT_FD;
    mon.data.fd.evt = evt;
    mon.data.fd.ctx = fnu;
    mon.data.fd.handler = fn;
    mon.data.fd.fd = fd;
    g_array_append_vals(zr->monitors, &mon, 1);

    item.fd = fd;
    g_array_append_vals(zr->items, &item, 1);
}

static inline int
_manage_one_event(struct zreactor_s *zr, guint i)
{
    int rc, evt;
    zmq_pollitem_t *item = &g_array_index(zr->items, zmq_pollitem_t, i);
    struct zmon_s *mon = &g_array_index(zr->monitors, struct zmon_s, i);
    //g_debug("EVT [%u] EVT[%x/%x]", i, item->revents, item->events);

    switch (mon->type) {

        case ZMT_ZMQ:
            if (item->revents)
                mon->data.zmq.handler(mon->data.zmq.ctx, mon->data.zmq.sock,
                        item->revents);
            return 0;

        case ZMT_ZK:
            evt = (item->revents & ZMQ_POLLIN ? ZOOKEEPER_READ : 0)
                | (item->revents & ZMQ_POLLOUT ? ZOOKEEPER_WRITE : 0);
            rc = zookeeper_process(mon->data.zh, evt);
            return (rc==ZOK || rc==ZNOTHING) ? 0 : -1;

        case ZMT_FD:
            if (item->revents)
                mon->data.fd.handler(mon->data.fd.ctx, mon->data.fd.fd,
                        item->revents);
            return 0;

        default:
            g_assert_not_reached();
            return -1;
    }
}

static inline int
_manage_all_events(struct zreactor_s *zr)
{
    guint i, max;
    gboolean stopped = FALSE;

    for (i=0,max=zr->items->len; i < max ;++i) {
        int rc = _manage_one_event(zr, i);
        if (rc)
            return rc;
        if (stopped)
            return 1;
    }

    return 0;
}

static inline glong
_rearm_zk_item_and_get_delay(struct zmon_s *mon, zmq_pollitem_t *item)
{
    int fd, evt;
    struct timeval tv;

    zookeeper_interest(mon->data.zh, &fd, &evt, &tv);
    item->socket = NULL;
    item->fd = fd;
    item->revents = 0;
    item->events = ZMQ_POLLERR
        | ((evt & ZOOKEEPER_READ) ? ZMQ_POLLIN : 0)
        | ((evt & ZOOKEEPER_WRITE) ? ZMQ_POLLOUT : 0);
    return (tv.tv_sec * 1000) + (tv.tv_usec / 1000);
}

static inline glong
_rearm_one_item_and_get_delay(struct zmon_s *mon, zmq_pollitem_t *item)
{
    switch (mon->type) {
        case ZMT_ZMQ:
            item->events = *(mon->data.zmq.evt);
            return -1;
        case ZMT_ZK:
            return _rearm_zk_item_and_get_delay(mon, item);
        case ZMT_FD:
            item->events = *(mon->data.fd.evt);
            return -1;
        default:
            g_assert_not_reached();
            return -1;
    }
}

static glong
_rearm_all_items_and_get_delay(struct zreactor_s *zr)
{
    guint i, max;
    glong d, delay = 60000;

    for (i=0,max=zr->monitors->len; i < max ;++i) {
        d = _rearm_one_item_and_get_delay(
                &g_array_index(zr->monitors, struct zmon_s, i),
                &g_array_index(zr->items, zmq_pollitem_t, i));
        if (d < 0)
            continue;
        if (delay > d)
            delay = d;
    }

    return delay;
}

static int
_zreactor_run_step(struct zreactor_s *zr)
{
    int rc = zmq_poll((zmq_pollitem_t*)zr->items->data, zr->items->len,
            _rearm_all_items_and_get_delay(zr));
    if (rc <= 0) // Timeout or error
        return rc;
    return _manage_all_events(zr);
}

int
zreactor_run(struct zreactor_s *zr)
{
    ASSERT(zr != NULL);
    ASSERT(zr->items != NULL);
    ASSERT(zr->monitors != NULL);
    ASSERT(zr->items->len == zr->monitors->len);
    while (zr->running && !_zreactor_run_step(zr)) {}
    g_debug("Reactor LOOP exited");
    return zr->running;
}

