#ifndef G_LOG_DOMAIN
# define G_LOG_DOMAIN "zsock"
#endif

#include "./macros.h"
#include "./zsock.h"

gint
strcmp3(gconstpointer p0, gconstpointer p1, gpointer ignored)
{
    (void) ignored;
    return g_strcmp0((const gchar*)p0, (const gchar*)p1);
}

GError*
zsocket_resolve(const gchar *zname, int *ztype)
{
    static struct named_type_s { const gchar *zname; int ztype; } defs[] = {
        {"PUB", ZMQ_PUB},
        {"SUB", ZMQ_SUB},
        {"PUSH", ZMQ_PUSH},
        {"PULL", ZMQ_PULL},
        {NULL,0}
    };
    
    g_assert(zname != NULL);
    g_assert(ztype != NULL);

    if (!g_str_has_prefix(zname, "zmq:"))
        return NEWERROR(EINVAL, "Invalid ZMQ socket type [%s]", zname);
    zname += sizeof("zmq:")-1;

    for (struct named_type_s *nt = defs; nt->zname ;++nt) {
        if (!g_ascii_strcasecmp(zname, nt->zname)) {
            *ztype = nt->ztype;
            return NULL;
        }
    }

    return NEWERROR(EINVAL, "Invalid ZMQ socket type [%s]", zname);
}

