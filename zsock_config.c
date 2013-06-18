#include <string.h>

#include <jansson.h>
#include <glib.h>

#include "./zsock.h"

#define JGET(R,O,F,T)do {\
    (R) = json_object_get(O,F); \
    if ((R) && !json_is_##T(R)) { \
        g_warning("No '%s' section (%s)", F, #T); \
        return NULL; \
    } \
} while (0)

void
cfg_listen_destroy(struct cfg_listen_s *cfg)
{
    if (!cfg)
        return ;
    if (cfg->type)
        g_free(cfg->type);
    if (cfg->ztype)
        g_free(cfg->ztype);
    if (cfg->uuid)
        g_free(cfg->uuid);
    if (cfg->cell)
        g_free(cfg->cell);
    if (cfg->url)
        g_free(cfg->url);
    memset(cfg, 0, sizeof(*cfg));
    g_free(cfg);
}

void
cfg_sock_destroy(struct cfg_sock_s *cfg)
{
    if (!cfg)
        return;
    if (cfg->sockname)
        g_free(cfg->sockname);
    if (cfg->ztype)
        g_free(cfg->ztype);
    if (cfg->connect)
        g_strfreev(cfg->connect);
    if (cfg->listen)
        g_strfreev(cfg->listen);
    g_free(cfg);
}

void
cfg_srv_destroy(struct cfg_srv_s *cfg)
{
    if (!cfg)
        return;
    if (cfg->srvtype)
        g_free(cfg->srvtype);
    if (cfg->socks) {
        GPtrArray *gpa = cfg->socks;
        while (gpa->len > 0) {
            struct cfg_sock_s *s = g_ptr_array_remove_index_fast(gpa, 0);
            cfg_sock_destroy(s);
        }
        g_ptr_array_free(gpa, TRUE);
    }
    g_free(cfg);
}

static gchar **
_get_connectv(json_t *jconnect)
{
    GPtrArray *tmp = g_ptr_array_new();

    for (void *iter=json_object_iter(jconnect); iter != NULL;
            iter = json_object_iter_next(jconnect, iter)) {
        const char *key = json_object_iter_key(iter);
        json_t *jval = json_object_iter_value(iter);
        if (!json_is_string(jval)) {
            g_error("Connect URL is not a string");
            return NULL;
        }
        g_ptr_array_add(tmp, g_strdup(key));
        g_ptr_array_add(tmp, g_strdup(json_string_value(jval)));
    }

    g_ptr_array_add(tmp, NULL);
    return (gchar**) g_ptr_array_free(tmp, FALSE);
}

static gchar **
_get_bindv(json_t *jbind)
{
    GPtrArray *tmp = g_ptr_array_new();
    size_t i, max;

    for (i=0,max=json_array_size(jbind); i<max ;++i) {
        json_t *jurl = json_array_get(jbind, i);
        g_ptr_array_add(tmp, g_strdup(json_string_value(jurl)));
    }

    g_ptr_array_add(tmp, NULL);
    return (gchar**) g_ptr_array_free(tmp, FALSE);
}

static struct cfg_sock_s*
_parse_socket(json_t *jroot)
{
    json_t *jname, *jtype, *jconnect, *jbind;

    if (!json_is_object(jroot)) {
        g_debug("Socket definition error : %s", "not a JSON object");
        return NULL;
    }

    JGET(jname, jroot, "name", string);
    JGET(jtype, jroot, "type", string);
    jconnect = json_object_get(jroot, "connect");
    jbind = json_object_get(jroot, "bind");

    if (!jconnect && !jbind) {
        g_debug("Socket definition error : %s", "No bind or connect");
        return NULL;
    }
    if (jbind && !json_is_array(jbind)) {
        g_debug("Socket definition error : %s", "Invalid bind section");
        return NULL;
    }
    if (jconnect && !json_is_object(jconnect)) {
        g_debug("Socket definition error : %s", "Invalid connect section");
        return NULL;
    }

    struct cfg_sock_s *csock = g_malloc0(sizeof(struct cfg_sock_s));
    csock->sockname = g_strdup(json_string_value(jname));
    csock->ztype = g_strdup(json_string_value(jtype));
    csock->connect = _get_connectv(jconnect);
    csock->listen = _get_bindv(jbind);

    return csock;
}

static struct cfg_srv_s*
_parse_service(json_t *jroot)
{
    json_t *jsocks, *jtype;

    JGET(jsocks, jroot, "sockets", array);
    JGET(jtype, jroot, "name", string);

    struct cfg_srv_s *cfg = g_malloc0(sizeof(struct cfg_srv_s));
    cfg->srvtype = g_strdup(json_string_value(jtype));
    cfg->socks = g_ptr_array_new();

    size_t max = json_array_size(jsocks);
    for (size_t i=0; i<max ;++i) {
        json_t *jsock = json_array_get(jsocks, i);
        struct cfg_sock_s *cfg_sock = _parse_socket(jsock);
        if (!cfg_sock)
            g_warning("Invalid socket definition");
        else
            g_ptr_array_add(cfg->socks, cfg_sock);
    }

    return cfg;
}

struct cfg_srv_s *
zservice_parse_config_buffer(const gchar *b, gsize bl)
{
    json_error_t err = {0,0,0,"",""};
    json_t *jroot;

    if (!(jroot = json_loadb(b, bl, 0, &err))) {
        g_warning("Invalid JSON configuration : (l%d c%d p%d) %s",
                err.line, err.column, err.position, err.text);
        return NULL;
    }

    struct cfg_srv_s *result = _parse_service(jroot);
    json_decref(jroot);
    return result;
}

struct cfg_srv_s *
zservice_parse_config_string(const gchar *cfg)
{
    g_assert(cfg != NULL);
    return zservice_parse_config_buffer(cfg, strlen(cfg));
}

struct cfg_srv_s*
zservice_parse_config_from_path(const gchar *path)
{
    struct cfg_srv_s *result = NULL;
    gchar *cfg = NULL;
    gsize size = 0;

    if (g_file_get_contents(path, &cfg, &size, NULL)) {
        result = zservice_parse_config_buffer(cfg, size);
        g_free(cfg);
    }

    return result;
}

static struct cfg_listen_s *
_parse_listen(json_t *jroot)
{
    json_t *jtype, *jztype, *jurl, *juuid, *jcell;

    if (!json_is_object(jroot))
        return NULL;
    JGET(jtype, jroot, "type", string);
    JGET(jztype, jroot, "ztype", string);
    JGET(jurl, jroot, "url", string);
    JGET(juuid, jroot, "uuid", string);
    JGET(jcell, jroot, "cell", string);

    struct cfg_listen_s *result = NULL;
    result = g_malloc0(sizeof(struct cfg_listen_s));
    result->type = g_strdup(json_string_value(jtype));
    result->ztype = g_strdup(json_string_value(jztype));
    result->url = g_strdup(json_string_value(jurl));
    result->uuid = g_strdup(json_string_value(juuid));
    result->cell = g_strdup(json_string_value(jcell));
    return result;
}

struct cfg_listen_s *
zlisten_parse_config_buffer(const gchar *b, gsize blen)
{
    json_error_t err = {0,0,0,"",""};
    json_t *jroot;

    if (!(jroot = json_loadb(b, blen, 0, &err))) {
        g_warning("Invalid JSON configuration : (l%d c%d p%d) %s",
                err.line, err.column, err.position, err.text);
        return NULL;
    }

    struct cfg_listen_s *result = _parse_listen(jroot);
    json_decref(jroot);
    return result;
}

