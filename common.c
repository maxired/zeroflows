#ifndef G_LOG_DOMAIN
# define G_LOG_DOMAIN "zsock"
#endif

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#include <glib.h>
#include <zmq.h>
#include <zookeeper.h>

#include "./macros.h"
#include "./zsock.h"
#include "./zreactor.h"
#include "./common.h"

static void
logger_noop(const gchar *log_domain, GLogLevelFlags log_level,
        const gchar *message, gpointer user_data)
{
    (void) log_domain, (void) log_level;
    (void) message, (void) user_data;
}

static void
logger_stderr(const gchar *log_domain, GLogLevelFlags log_level,
        const gchar *message, gpointer user_data)
{
    GTimeVal tv;
    union {
        GThread *th;
        guint16 u[4];
    } b;

    b.u[0] = b.u[1] = b.u[2] = b.u[3] = 0;
    b.th = NULL;

    b.th = g_thread_self();

    (void) user_data;
    g_get_current_time(&tv);
    fprintf(stdout, "%ld.%06ld %-8s %04X %04X %s\n",
            tv.tv_sec, tv.tv_usec,
            log_domain ? log_domain : "-",
            log_level,
            ((b.u[0] ^ b.u[1]) ^ b.u[2]) ^ b.u[3],
            message);
}

void
uuid_randomize(gchar *d, gsize dl)
{
    static guint32 seq = 0;

    struct {
        int pid, ppid;
        GTimeVal now;
        guint32 seq;
        gpointer p0, p1, p2;
        gchar h[512];
    } bulk;

    memset(&bulk, 0, sizeof(bulk));
    bulk.pid = getpid();
    bulk.ppid = getppid();
    g_get_current_time(&(bulk.now));
    bulk.seq = ++seq;
    bulk.p0 = &bulk;
    bulk.p1 = d;
    bulk.p2 = &d;
    gethostname(bulk.h, sizeof(bulk.h));

    GChecksum *cs = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(cs, (guint8*) &bulk, sizeof(bulk));
    g_strlcpy(d, g_checksum_get_string(cs), dl);
    g_checksum_free(cs);

    for (; dl ;--dl)
        d[dl-1] = g_ascii_toupper(d[dl-1]);
}
 
void
main_set_log_handlers(void)
{
    void set(const gchar *d, GLogLevelFlags f, GLogFunc fn, gpointer u) {
        g_log_set_handler(d, G_LOG_FLAG_RECURSION
            |G_LOG_LEVEL_DEBUG |G_LOG_LEVEL_INFO |G_LOG_LEVEL_MESSAGE
            |G_LOG_LEVEL_WARNING |G_LOG_LEVEL_CRITICAL |G_LOG_LEVEL_ERROR,
            logger_noop, NULL);
        //f &= ~G_LOG_LEVEL_DEBUG;
        g_log_set_handler(d, f, fn, u);
    }
    set("GLib", G_LOG_FLAG_RECURSION|G_LOG_FLAG_FATAL
            |G_LOG_LEVEL_WARNING
            |G_LOG_LEVEL_CRITICAL
            |G_LOG_LEVEL_ERROR,
            logger_stderr, NULL);

    set("zsock", G_LOG_FLAG_RECURSION
            |G_LOG_LEVEL_DEBUG |G_LOG_LEVEL_INFO |G_LOG_LEVEL_MESSAGE
            |G_LOG_LEVEL_WARNING |G_LOG_LEVEL_CRITICAL |G_LOG_LEVEL_ERROR,
            logger_stderr, NULL);

    set("ZK", G_LOG_FLAG_RECURSION
            |G_LOG_LEVEL_INFO |G_LOG_LEVEL_MESSAGE
            |G_LOG_LEVEL_WARNING |G_LOG_LEVEL_CRITICAL |G_LOG_LEVEL_ERROR,
            logger_stderr, NULL);

    g_log_set_default_handler(logger_stderr, NULL);
}

void
zenv_init(struct zenv_s *zenv)
{
    ASSERT(zenv != NULL);

    memset(zenv, 0, sizeof(struct zsrv_env_s));

    zenv->zh = zookeeper_init("127.0.0.1:2181", NULL, 5000, NULL, NULL, 0);
    ASSERT(zenv->zh != NULL);

    zenv->zctx = zmq_ctx_new();
    ASSERT(zenv->zctx != NULL);

    zenv->zr = zreactor_create();
    ASSERT(zenv->zr != NULL);
}

void
zenv_close(struct zenv_s *zenv)
{
    ASSERT(zenv != NULL);

    zreactor_destroy(zenv->zr);
    zmq_ctx_destroy(zenv->zctx);
    zookeeper_close(zenv->zh);
}

void
zsrv_env_init(const gchar *type, struct zsrv_env_s *ctx)
{
    zenv_init(&ctx->zenv);

    // Create the service and bind it to the environment
    ctx->zsrv = zservice_create(ctx->zenv.zctx, ctx->zenv.zh, type);
    ASSERT(ctx->zsrv != NULL);

    // TODO get the UUID and the CELL from a configuration
    uuid_randomize(ctx->zsrv->uuid, sizeof(ctx->zsrv->uuid));
    g_strlcpy(ctx->zsrv->cell, "localhost", sizeof(ctx->zsrv->cell));

    zservice_register_in_reactor(ctx->zenv.zr, ctx->zsrv);

    zreactor_add_zk(ctx->zenv.zr, ctx->zenv.zh);
}

void
zsrv_env_close(struct zsrv_env_s *ctx)
{
    ASSERT(ctx != NULL);
    zservice_destroy(ctx->zsrv);
    zenv_close(&ctx->zenv);
}

void
zclt_env_init(const gchar *typename, const gchar *target, struct zclt_env_s *ctx)
{
    ASSERT(target != NULL);
    ASSERT(ctx != NULL);

    memset(ctx, 0, sizeof(*ctx));
    zenv_init(&ctx->zenv);

    // TODO Get the UUID and the CELL from the configuration
    uuid_randomize(ctx->uuid, sizeof(ctx->uuid)-1);
    g_strlcpy(ctx->cell, "localhost", sizeof(ctx->cell)-1);

    // Now open the zsocket itself
    int ztype = 0;
    GError *e = zsocket_resolve(typename, &ztype);
    if (e != NULL) {
        g_error("ZMQ socket error : (%d) %s", e->code, e->message);
        g_clear_error(&e);
        abort();
    }

    ctx->zsock = zsock_create(ctx->uuid, ctx->cell);
    ASSERT(ctx->zsock != NULL);

    ctx->zsock->zs = zmq_socket(ctx->zenv.zctx, ztype);
    ctx->zsock->zh = ctx->zenv.zh;
    ctx->zsock->zctx = ctx->zenv.zctx;
    ctx->zsock->fullname = g_strdup("client");
    zsock_connect(ctx->zsock, target, "all");

    // bind them
    zsock_register_in_reactor(ctx->zenv.zr, ctx->zsock);
    zreactor_add_zk(ctx->zenv.zr, ctx->zenv.zh);
}

void
zclt_env_close(struct zclt_env_s *ctx)
{
    ASSERT(ctx != NULL);
    zsock_destroy(ctx->zsock);
    zenv_close(&ctx->zenv);
}

