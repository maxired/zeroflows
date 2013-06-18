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

static inline const gchar*
ztype2str(int ztype)
{
    switch (ztype) {
        case ZMQ_PUB:
            return "zmq:PUB";
        case ZMQ_SUB:
            return "zmq:SUB";
        case ZMQ_PUSH:
            return "zmq:PUSH";
        case ZMQ_PULL:
            return "zmq:PULL";
        default:
            return "?";
    }
}

static inline int
get_ztype(void *s)
{
    int type = 0;
    size_t len = sizeof(int);
    zmq_getsockopt(s, ZMQ_TYPE, &type, &len);
    return type;
}

static inline gboolean
ztype_compatible(int ztype0, int ztype1)
{
    switch (ztype0) {
        case ZMQ_PUB:
            return ztype1 == ZMQ_SUB;
        case ZMQ_SUB:
            return ztype1 == ZMQ_PUB;
        case ZMQ_PUSH:
            return ztype1 == ZMQ_PULL;
        case ZMQ_PULL:
            return ztype1 == ZMQ_PUSH;
        default:
            return FALSE;
    }
}

static inline int
zevents(void *s)
{
    switch (get_ztype(s)) {
        case ZMQ_PULL:
        case ZMQ_SUB:
            return ZMQ_POLLIN;
        case ZMQ_PUSH:
        case ZMQ_PUB:
            return ZMQ_POLLOUT;
        case ZMQ_XSUB:
        case ZMQ_XPUB:
        case ZMQ_ROUTER:
        case ZMQ_DEALER:
        case ZMQ_REP:
        case ZMQ_PAIR:
        case ZMQ_REQ:
            return ZMQ_POLLOUT|ZMQ_POLLIN;
        default:
            g_assert_not_reached();
            return 0;
    }
}

static inline gchar **
_extract_urlv(GPtrArray *v)
{
    gint pstr_cmp(gconstpointer p0, gconstpointer p1) {
        return g_strcmp0(*(gchar**)p0, *(gchar**)p1);
    }

    GPtrArray *tmp = g_ptr_array_sized_new(16);
    for (guint i=0; i<v->len ;++i) {
        struct cfg_listen_s *cl = g_ptr_array_index(v, i);
        if (cl && cl->url) {
            g_ptr_array_add(tmp, cl->url);
            cl->url = NULL;
        }
    }
    g_ptr_array_set_size(v, 0);
    g_ptr_array_sort(tmp, pstr_cmp);
    g_ptr_array_add(tmp, NULL);
    return (gchar**) g_ptr_array_free(tmp, FALSE);
}

static inline void
_zsock_real_connect(struct zsock_s *zsock, const gchar *url)
{
    guint *pi = g_tree_lookup(zsock->connect_real, url);
    if (pi)
        ++ *pi;
    else {
        zmq_connect(zsock->zs, url);
        pi = g_malloc(sizeof(guint));
        *pi = 1;
        g_tree_insert(zsock->connect_real, g_strdup(url), pi);
    }
}

static inline void
_zsock_real_disconnect(struct zsock_s *zsock, const gchar *url)
{
    guint *pi = g_tree_lookup(zsock->connect_real, url);
    if (!pi)
        g_warning("Not connected to [%s]", url);
    else {
        if (*pi) {
            zmq_disconnect(zsock->zs, url);
            -- *pi;
        }
        if (!*pi) 
            g_tree_remove(zsock->connect_real, url);
    }
}

struct delta_s
{
    gchar **add;
    gchar **rem;
    gchar **to_delete;
};

static inline gchar**
_pack_result(GTree *t)
{
    gchar **c;
    gchar **result = g_malloc0((1 + g_tree_nnodes(t)) * sizeof(void*));
    gboolean runner(gpointer k, gpointer v, gpointer u) {
        (void) v, (void) u;
        *(c++) = k;
        return FALSE;
    }
    c = result;
    g_tree_foreach(t, runner, NULL);
    g_tree_destroy(t);
    return result;
}

static inline gchar**
_merge_deltas(gchar **current, gchar **newv, struct delta_s *delta)
{
    GTree *t = g_tree_new_full(strcmp3, NULL, NULL, NULL);

    for (gchar **c = current; *c ;++c)
        g_tree_insert(t, *c, GINT_TO_POINTER(1));
    for (gchar **c = delta->add; *c ;++c)
        g_tree_insert(t, *c, GINT_TO_POINTER(1));
    for (gchar **c = delta->rem; *c ;++c)
        g_tree_remove(t, *c);

    g_free(current);
    g_free(newv);
    g_free(delta->add);
    g_strfreev(delta->rem);
    g_strfreev(delta->to_delete);

    return _pack_result(t);
}

static inline void
_debug_sets(gchar **urlv, gchar **newv)
{
    g_debug("SET current");
    for (gchar **c = urlv; *c ;++c)
        g_debug(" ! %p %s", *c, *c);

    g_debug("SET new");
    for (gchar **c = newv; *c ;++c)
        g_debug(" > %p %s", *c, *c);
}

static inline void
_debug_deltas(struct delta_s *delta)
{
    g_debug("DELTA add");
    for (gchar **c = delta->add; *c ;++c)
        g_debug(" + %p %s", *c, *c);

    g_debug("DELTA rem");
    for (gchar **c = delta->rem; *c ;++c)
        g_debug(" - %p %s", *c, *c);

    g_debug("DELTA 2del");
    for (gchar **c = delta->to_delete; *c ;++c)
        g_debug(" x %p %s", *c, *c);
}

static inline void
_compute_deltas(gchar **urlv, gchar **newv, struct delta_s *delta)
{
    GPtrArray *pnew = g_ptr_array_new();
    GPtrArray *plost = g_ptr_array_new();
    GPtrArray *pdel = g_ptr_array_new();

    while (*urlv || *newv) {
        //g_debug("CMP new=%s current=%s", *newv, *urlv);
        if (!*newv)
            g_ptr_array_add(plost, *(urlv++));
        else if (!*urlv)
            g_ptr_array_add(pnew, *(newv++));
        else {
            int rc = strcmp(*urlv, *newv);
            if (!rc) {
                urlv++;
                g_ptr_array_add(pdel, *(newv++));
            }
            else if (rc < 0)
                g_ptr_array_add(plost, *(urlv++));
            else
                g_ptr_array_add(pnew, *(newv++));
        }
    }

    g_ptr_array_add(pnew, NULL);
    g_ptr_array_add(plost, NULL);
    g_ptr_array_add(pdel, NULL);

    delta->add = (gchar**) g_ptr_array_free(pnew, FALSE);
    delta->rem = (gchar**) g_ptr_array_free(plost, FALSE);
    delta->to_delete = (gchar**) g_ptr_array_free(pdel, FALSE);
}

static void
zco_reconnect(struct zconnect_s *zco)
{
    if (!zco || !zco->urlv_new || !zco->urlv_new->len)
        return ;

    gchar **urlv, **newv;
    struct delta_s delta;

    ASSERT(zco != NULL);
    memset(&delta, 0, sizeof(struct delta_s));
    urlv = zco->urlv_current;
    newv = _extract_urlv(zco->urlv_new);

    //_debug_sets(urlv, newv);
    _compute_deltas(urlv, newv, &delta);
    //_debug_deltas(&delta);
     
    // Apply the delta
    for (gchar **c = delta.add; *c ;++c)
        _zsock_real_connect(zco->zs, *c);
    for (gchar **c = delta.rem; *c ;++c)
        _zsock_real_disconnect(zco->zs, *c);

    zco->urlv_current = _merge_deltas(urlv, newv, &delta);
}


static void
zco_destroy(struct zconnect_s *zco)
{
    if (!zco)
        return ;
    if (zco->type) {
        g_free(zco->type);
        zco->type = NULL;
    }
    if (zco->policy) {
        g_free(zco->policy);
        zco->policy = NULL;
    }
    if (zco->urlv_current) {
        g_strfreev(zco->urlv_current);
        zco->urlv_current = NULL;
    }
    if (zco->urlv_new) {
        while (zco->urlv_new->len) {
            struct cfg_listen_s *cl = zco->urlv_new->pdata[0];
            g_ptr_array_remove_index_fast(zco->urlv_new, 0);
            cfg_listen_destroy(cl);
        }
        g_ptr_array_free(zco->urlv_new, TRUE);
        zco->urlv_new = NULL;
    }

    zco->zs = NULL;
    g_free(zco);
}

static struct zconnect_s*
zco_create(struct zsock_s *zs, const gchar *type)
{
    struct zconnect_s *zco;
    zco = g_malloc0(sizeof(struct zconnect_s));
    zco->zs = zs;
    zco->type = g_strdup(type);
    zco->urlv_current = g_malloc0(sizeof(gchar*));
    zco->urlv_new = g_ptr_array_new_full(8, (GDestroyNotify)cfg_listen_destroy);
    return zco;
}

void
zsock_connect(struct zsock_s *zsock, const gchar *type, const gchar *policy)
{
    struct zconnect_s *zco;

    ASSERT(type != NULL);
    ASSERT(policy != NULL);
    ASSERT(zsock != NULL);
    ASSERT(zsock->connect_cfg != NULL);

    if (!(zco = g_tree_lookup(zsock->connect_cfg, type))) {
        zco = zco_create(zsock, type);
        g_tree_insert(zsock->connect_cfg, g_strdup(type), zco);
    }

    if (zco->policy)
        g_free(zco->policy);
    zco->policy = g_strdup(policy);
}

static void
_zsock_bind(struct zsock_s *zsock, const gchar *url)
{
    int rc;
    gchar d[256];
    gsize dlen = 256;

    if (0 > (rc = zmq_bind(zsock->zs, url)))
        return;

    if (0 > zmq_getsockopt(zsock->zs, ZMQ_LAST_ENDPOINT, d, &dlen))
        return;

    g_tree_insert(zsock->bind_set, g_strdup(url), g_strdup(d));
}

gboolean
zsock_ready(struct zsock_s *zsock)
{
    ASSERT(zsock != NULL);
    ASSERT(zsock->connect_real != NULL);
    ASSERT(zsock->bind_set != NULL);
    if (g_tree_nnodes(zsock->connect_real) == 0
            && g_tree_nnodes(zsock->bind_set) == 0)
        return FALSE;

    zmq_pollitem_t item = {zsock->zs, -1, ZMQ_POLLOUT, 0};
    return 1 == zmq_poll(&item, 1, 0);
}

void
zsock_configure(struct zsock_s *zsock, struct cfg_sock_s *cfg)
{
    ASSERT(zsock != NULL);
    ASSERT(cfg != NULL);

    // connect
    for (gchar **p = cfg->connect ;;) {
        gchar *type, *policy;
        if (!(type = *(p++)))
            break;
        if (!(policy = *(p++)))
            break;
        zsock_connect(zsock, type, policy);
    }

    // listen
    for (gchar **p = cfg->listen; *p ;++p)
        _zsock_bind(zsock, *p);
}

struct zsock_s*
zsock_create(const gchar *pu, const gchar *pc)
{
    struct zsock_s *zsock = g_malloc0(sizeof(struct zsock_s));

    zsock->puuid = pu;
    zsock->pcell = pc;

    zsock->connect_real = g_tree_new_full(strcmp3, NULL, g_free, g_free);
    zsock->connect_cfg = g_tree_new_full(strcmp3, NULL, g_free,
            (GDestroyNotify)zco_destroy);
    zsock->bind_set = g_tree_new_full(strcmp3, NULL, g_free, g_free);

    return zsock;
}

void
zsock_destroy(struct zsock_s *zsock)
{
    if (!zsock)
        return;

    if (zsock->zs) {
        zmq_close(zsock->zs);
        zsock->zs = NULL;
    }

    if (zsock->fullname) {
        g_free(zsock->fullname);
        zsock->fullname = NULL;
    }

    if (zsock->connect_cfg) {
        g_tree_destroy(zsock->connect_cfg);
        zsock->connect_cfg = NULL;
    }

    if (zsock->connect_real) {
        gboolean runner(gpointer u, gpointer i0, gpointer i1) {
            (void) i0, (void) i1;
            (int) zmq_disconnect(zsock->zs, (gchar*)u);
            return FALSE;
        }
        g_tree_foreach(zsock->connect_real, runner, NULL);
        g_tree_destroy(zsock->connect_real);
        zsock->connect_real = NULL;
    }

    if (zsock->bind_set) {
        g_tree_destroy(zsock->bind_set);
        zsock->bind_set = NULL;
    }

    g_free(zsock);
}

//------------------------------------------------------------------------------

static void on_get(int r, const char *v, int vl, const struct Stat *s, const void *u);
static void on_list_completion(int r, const struct String_vector *sv, const void *u);
static void on_list_change(zhandle_t *zh, int t, int s, const char *p, void *u);

static void on_bind_create(int r, const char *v, const void *u);

static void
restart_list(struct zconnect_s *zco)
{
    // Get rid of values already got but not yet taken into account
    g_ptr_array_set_size(zco->urlv_new, 0);

    // Start the ZooKeeper request
    gchar *p = g_strdup_printf("/listen/%s", zco->type);
    int rc = zoo_awget_children(zco->zs->zh, p,
            on_list_change, zco,
            on_list_completion, zco);
    ZK_DEBUG("awget_children(%s) = %d", p, rc);
    g_free(p);

    if (rc == ZOK)
        ++ zco->list_pending;
}

static inline void
maybe_relist(struct zconnect_s *zco)
{
    if (!zco->get_pending && !zco->list_pending && zco->list_wanted) {
        -- zco->list_wanted;
        restart_list(zco);
    }
}

static inline void
maybe_reconnect(struct zconnect_s *zco)
{
    if (!zco->get_pending && !zco->list_pending && !zco->list_wanted)
        zco_reconnect(zco);
}

/* Builds a JSON representation of the 'listen' block */
static inline GString*
_build_listen(struct zsock_s *zs, const gchar *url)
{
    GString *body = g_string_new("{");
    g_string_append(body, "\"type\":\"");
    g_string_append(body, zs->fullname);
    g_string_append(body, "\",\"ztype\":\"");
    g_string_append(body, ztype2str(get_ztype(zs->zs)));
    g_string_append(body, "\",\"url\":\"");
    g_string_append(body, url);
    g_string_append(body, "\",\"uuid\":\"");
    g_string_append(body, zs->puuid);
    g_string_append(body, "\",\"cell\":\"");
    g_string_append(body, zs->pcell);
    g_string_append(body, "\"}");
    return body;
}

static void
on_get(int r, const char *v, int vl, const struct Stat *s, const void *u)
{
    struct zconnect_s *zco = u;
    (void) r, (void) s;

    ASSERT(zco != NULL);
    g_debug("%s(%s -> %s) %.*s", __FUNCTION__, zco->zs->fullname,
            zco->type, v ? vl : 0, v);
    -- zco->get_pending;

    if (r == ZOK) {
        struct cfg_listen_s *cfg = zlisten_parse_config_buffer(v, vl);
        if (cfg != NULL) {
            int own_ztype, opposite_ztype;
            own_ztype = get_ztype(zco->zs->zs);
            GError *e = zsocket_resolve(cfg->ztype, &opposite_ztype);
            if (e != NULL) {
                g_debug("Socket ignored (invalid ztype)");
                cfg_listen_destroy(cfg);
            }
            else if (!ztype_compatible(own_ztype, opposite_ztype)) {
                g_debug("Socket ignored (ztypes not compatible: %s vs. %s)",
                        ztype2str(own_ztype), ztype2str(opposite_ztype));
                cfg_listen_destroy(cfg);
            }
            else
                g_ptr_array_add(zco->urlv_new, cfg);
        }
    }

    maybe_reconnect(zco);
    maybe_relist(zco);
}

static void
on_list_completion(int r, const struct String_vector *sv, const void *u)
{
    struct zconnect_s *zco = u;

    ASSERT(zco != NULL);
    g_debug("%s(%s -> %s) %u", __FUNCTION__, zco->zs->fullname, zco->type, sv ? sv->count : 0);
    -- zco->list_pending;

    if (r == ZOK)  {
        for (gint32 i=0; i < sv->count ;++i) {
            gchar *p = g_strdup_printf("/listen/%s/%s", zco->type, sv->data[i]);
            int rc = zoo_awget(zco->zs->zh, p, NULL, NULL, on_get, zco);
            ZK_DEBUG("awget(%s) = %d", p, rc);
            if (rc == ZOK)
                ++ zco->get_pending;
            g_free(p);
        }
    }
    maybe_relist(zco);
}

static void
on_list_change(zhandle_t *zh, int t, int s, const char *p, void *u)
{
    struct zconnect_s *zco = u;
    (void) zh, (void) t, (void) s, (void) p;

    ASSERT(zco != NULL);
    g_debug("%s(%s -> %s)", __FUNCTION__, zco->zs->fullname, zco->type);

    ++ zco->list_wanted;
    maybe_relist(zco);
}

static void
on_bind_create(int r, const char *v, const void *u)
{
    (void) u;
    g_debug("%s(%d,%s,%p)", __FUNCTION__, r, v, u);
}

static int
zsock_handler(struct zsock_s *zsock, void *s, int evt)
{
    (void) s;
    ASSERT(s != NULL);
    ASSERT(zsock != NULL);
    ASSERT(zsock->zs == s);

    if (evt & ZMQ_POLLOUT) {
        zsock->evt &= ~ZMQ_POLLOUT;
        if (zsock->ready_out)
            zsock->ready_out(zsock);
    }

    if (evt & ZMQ_POLLIN) {
        if (zsock->ready_in)
            zsock->ready_in(zsock);
    }

    return 0;
}

void
zsock_register_in_reactor(struct zreactor_s *zr, struct zsock_s *zsock)
{
    gboolean on_target(gpointer k, gpointer v, gpointer u) {
        (void) k, (void) u;
        struct zconnect_s *zco = v;
        g_debug(" %s -> [%s]", zsock->fullname, zco->type);
        restart_list(zco);
        return FALSE;
    }

    gboolean on_endpoint(gpointer k, gpointer v, gpointer u) {
        gchar *endpoint = k;
        gchar *url = v;
        (void) u;

        g_debug(" %s <- [%s,%s]", zsock->fullname, endpoint, url);
        GString *body = _build_listen(zsock, url);
        gchar *path = g_strdup_printf("/listen/%s/%s-",
                zsock->fullname, zsock->puuid);

        int rc = zoo_acreate(zsock->zh, path, body->str, body->len,
                &ZOO_OPEN_ACL_UNSAFE, ZOO_EPHEMERAL|ZOO_SEQUENCE,
                on_bind_create, NULL);

        ZK_DEBUG("acreate(%s) = %d", path, rc);
        g_string_free(body, TRUE);
        g_free(path);
        return FALSE;
    }

    g_tree_foreach(zsock->bind_set, on_endpoint, NULL);
    g_tree_foreach(zsock->connect_cfg, on_target, NULL);

    zreactor_add_zmq(zr, zsock->zs, &(zsock->evt),
            (zreactor_fn_zmq) zsock_handler, zsock);
}

