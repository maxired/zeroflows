#ifndef G_LOG_DOMAIN
# define G_LOG_DOMAIN "zsock"
#endif

#include <string.h>

#include <glib.h>
#include <zmq.h>
#include <zookeeper.h>

#include "./macros.h"
#include "./zsock.h"
#include "./zreactor.h"

#define ZK_DEBUG(FMT,...) g_log("ZK", G_LOG_LEVEL_DEBUG, FMT, ##__VA_ARGS__)

static struct zsock_s *
zservice_create_socket(struct zservice_s *zsrv, struct cfg_sock_s *itf)
{
    ASSERT(zsrv->zctx != NULL);

    int ztype = 0;

    GError *e = zsocket_resolve(itf->ztype, &ztype);
    if (e != NULL)
        g_error("Invalid socket type : (%d) %s", e->code, e->message);

    void *zs = zmq_socket(zsrv->zctx, ztype);
    if (!zs) {
        g_error("ZMQ socket error [%s/%d] : (%d) %s", itf->ztype, ztype,
                errno, strerror(errno));
    }

    if (ztype == ZMQ_SUB) {
        int rc = zmq_setsockopt(zs, ZMQ_SUBSCRIBE, "", 0);
        g_assert(rc == 0);
    }

    struct zsock_s *zsock = zsock_create(zsrv->uuid, zsrv->cell);
    zsock->zs = zs;
    zsock->zh = zsrv->zh;
    zsock->zctx = zsrv->zctx;
    zsock->localname = g_strdup(itf->sockname);
    zsock->fullname = g_strconcat(zsrv->srvtype, ".", itf->sockname, NULL);
    zsock_configure(zsock, itf);

    g_debug("SOCK [%s] [%s]", itf->ztype, zsock->fullname);
    return zsock;
}

static void
zservice_create_and_register(struct zservice_s *zsrv, struct cfg_sock_s *itf)
{
    struct zsock_s *zsock;
    ASSERT(zsrv != NULL);

    // ensure a socket exist and configure it. If it exists, we must
    // check its configuration did not change.
    if (!(zsock = g_tree_lookup(zsrv->socks, itf->sockname))) {
        zsock = zservice_create_socket(zsrv, itf);
        g_tree_insert(zsrv->socks, g_strdup(itf->sockname), zsock);
    }
    else {
        // TODO check the config matches, then update it
    }
}

static void
zservice_configure(struct zservice_s *zsrv, struct cfg_srv_s *cfg)
{
    ASSERT(zsrv != NULL);
    ASSERT(cfg != NULL);

    for (guint i=0; i < cfg->socks->len ;++i) {
        struct cfg_sock_s *itf = cfg->socks->pdata[i];
        zservice_create_and_register(zsrv, itf);
    }
}

static void
on_config_completion(int r, const char *v, int vlen, const struct Stat *s, const void *u)
{
    struct zservice_s *zsrv = u;

    g_debug("%s(%d,%p,%p) %.*s", __FUNCTION__, r, s, u, vlen, v);
    ASSERT(zsrv != NULL);

    (void) s;
    if (r != ZOK) {
        ZK_DEBUG("get error : %d", r);
        return;
    }

    // First configuration of the service
    struct cfg_srv_s *cfg = zservice_parse_config_buffer(v, vlen);
    if (!cfg)
        g_warning("CFG error : invalid JSON object");
    else {
        zservice_configure(zsrv, cfg);
        cfg_srv_destroy(cfg);
        g_debug("CFG done");
    }

    // Now the sockets are known, load/monitor their behavior
    g_debug("Connecting / Binding the sockets");
    gboolean on_socket(gpointer k0, gpointer v0, gpointer u0) {
        (void) k0, (void) u0;
        zsock_register_in_reactor(zsrv->zr, v0);
        return FALSE;
    }
    g_tree_foreach(zsrv->socks, on_socket, NULL);

    if (zsrv->on_config)
        zsrv->on_config(zsrv, zsrv->on_config_data);
}

static void
on_config_change(zhandle_t *zh, int t, int s, const char *p, void *u)
{
    //struct zservice_s *zsrv = u;
    g_debug("%s(%p,%d,%d,%s,%p)", __FUNCTION__, zh, t, s, p, u);
}

void
zservice_register_in_reactor(struct zreactor_s *zr, struct zservice_s *zsrv)
{
    ASSERT(zr != NULL);
    ASSERT(zsrv != NULL);
    (void) zr;

    zsrv->zr = zr;

    // Trigger the first service configuration
    gchar *p = g_strdup_printf("/services/%s", zsrv->srvtype);
    int zrc = zoo_awget(zsrv->zh, p,
            on_config_change, zsrv,
            on_config_completion, zsrv);
    g_free(p);

    if (zrc != ZOK) {
        g_debug("Failed to ask the first configuration for [%s] (%d)",
                zsrv->srvtype, zrc);
        zreactor_stop(zr);
    }
}

struct zservice_s*
zservice_create(void *zctx, zhandle_t *zh, const gchar *srvtype)
{
    ASSERT(zctx != NULL);
    ASSERT(zh != NULL);
    ASSERT(srvtype != NULL);

    struct zservice_s *zsrv = g_malloc0(sizeof(struct zservice_s));
    zsrv->zh = zh;
    zsrv->zctx = zctx;
    zsrv->srvtype = g_strdup(srvtype);
    zsrv->socks = g_tree_new_full(strcmp3, NULL, g_free,
            (GDestroyNotify)zsock_destroy);

    // A few sockets always exist
    static gchar *empty[] = {NULL};
    struct cfg_sock_s cfg_tick = { "_tick", "zmq:SUB", empty, empty };
    zservice_create_and_register(zsrv, &cfg_tick);

    return zsrv;
}

void
zservice_destroy(struct zservice_s *zsrv)
{
    if (!zsrv)
        return;
    if (zsrv->socks)
        g_tree_destroy(zsrv->socks);
    if (zsrv->srvtype)
        g_free(zsrv->srvtype);
    g_free(zsrv);
}

struct zsock_s*
zservice_get_socket(struct zservice_s *zsrv, const gchar *n)
{
    struct zsock_s *zs = g_tree_lookup(zsrv->socks, n);
    if (!zs)
        g_error("BUG : required a socket that is not configured [%s]", n);
    return zs;
}

void
zservice_on_config(struct zservice_s *zsrv, gpointer data,
        void (*hook)(struct zservice_s*, gpointer))
{
    ASSERT(zsrv != NULL);
    zsrv->on_config = hook;
    zsrv->on_config_data = data;
}

