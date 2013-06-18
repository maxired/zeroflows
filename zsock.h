#ifndef TECHFORUM_zsock_h
# define TECHFORUM_zsock_h 1
# include <glib.h>
# include <zookeeper.h>
# include <zmq.h>

//
gint strcmp3(gconstpointer p0, gconstpointer p1, gpointer ignored);

// Transform the string form of the ZMQ type to the ZMQ_* constant
GError* zsocket_resolve(const gchar *zname, int *ztype);

//------------------------------------------------------------------------------

struct cfg_listen_s
{
    gchar *type;
    gchar *ztype;
    gchar *uuid;
    gchar *cell;
    gchar *url;
};

struct cfg_sock_s
{
    gchar *sockname;
    gchar *ztype;
    gchar **connect; // pairs of char*
    gchar **listen; // char*
};

struct cfg_srv_s
{
    gchar *srvtype;
    GPtrArray *socks; // (struct cfg_sock_s *)
};

void cfg_listen_destroy(struct cfg_listen_s *cfg);
void cfg_sock_destroy(struct cfg_sock_s *cfg);
void cfg_srv_destroy(struct cfg_srv_s *cfg);

struct cfg_listen_s * zlisten_parse_config_buffer(const gchar *b, gsize blen);
struct cfg_srv_s * zservice_parse_config_buffer(const gchar *b, gsize bl);
struct cfg_srv_s * zservice_parse_config_string(const gchar *cfg);
struct cfg_srv_s* zservice_parse_config_from_path(const gchar *path);

//------------------------------------------------------------------------------

struct zconnect_s
{
    gchar *type;
    gchar *policy;
    gchar **urlv_current;

    GPtrArray *urlv_new; // (struct cfg_listen_s*)
    guint list_wanted;
    guint list_pending;
    guint get_pending;

    struct zsock_s *zs; // the socket it belongs to
};

struct zsock_s
{
    void *zctx; // a ZMQ context 
    void *zs; // ZMQ socket
    zhandle_t *zh; // ZooKeeper handle

    gchar *fullname;
    gchar *localname;
    const gchar *puuid;
    const gchar *pcell;
    gboolean paused_input;

    GTree *connect_real; // char* -> gulong
    GTree *connect_cfg; // char* -> (struct zconnect_s*)
    GTree *bind_set; // char* -> char*

    void (*ready_out)(struct zsock_s*);
    void (*ready_in)(struct zsock_s*);
    int evt; // to be monitored ZMQ_POLLIN|ZMQ_POLLOUT
};

struct zservice_s
{
    void *zctx; // a ZMQ context 
    struct zreactor_s *zr; // the reactor managing this reactor
    zhandle_t *zh; // the ZooKeeper handle

    gpointer on_config_data;
    void (*on_config)(struct zservice_s *zsrv, gpointer data);

    gchar *srvtype;
    GTree *socks;
    gchar uuid[32];
    gchar cell[32];
};

//------------------------------------------------------------------------------

/* Create the structure and _SOME_ of its internal field. */
struct zsock_s* zsock_create(const gchar *uuid, const gchar *cell);

void zsock_destroy(struct zsock_s *zsock);

gboolean zsock_ready(struct zsock_s *zsock);

void zsock_configure(struct zsock_s *zsock, struct cfg_sock_s *cfg);

void zsock_register_in_reactor(struct zreactor_s *zr, struct zsock_s *zsock);

void zsock_connect(struct zsock_s *zsock, const gchar *type,
        const gchar *policy);

//------------------------------------------------------------------------------

struct zservice_s* zservice_create(void *zctx, zhandle_t *zh, const gchar *srvtype);

void zservice_destroy(struct zservice_s *zsrv);

struct zsock_s* zservice_get_socket(struct zservice_s *zsrv, const gchar *n);

void zservice_register_in_reactor(struct zreactor_s *zr,
        struct zservice_s *zsrv);

void zservice_on_config(struct zservice_s *zsrv, gpointer u,
        void (*hook)(struct zservice_s*, gpointer));

#endif // TECHFORUM_zsock_h
