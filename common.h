#ifndef TECHFORUM_common_h
# define TECHFORUM_common_h 1
# include <glib.h>
# include <zmq.h>
# include <zookeeper.h>

void uuid_randomize(gchar *d, gsize dl);

void main_set_log_handlers(void);


// Environment common to all entities

struct zenv_s
{
    zhandle_t *zh; // ZooKeeper handle
    void *zctx; // ZeroMQ context
    struct zreactor_s *zr;
};

void zenv_init(struct zenv_s *zenv);

void zenv_close(struct zenv_s *zenv);


// Environment common to standalone services

struct zsrv_env_s
{
    struct zenv_s zenv;
    struct zservice_s *zsrv;
};

void zsrv_env_init(const gchar *type, struct zsrv_env_s *ctx);

void zsrv_env_close(struct zsrv_env_s *ctx);


// Environment common to clients

struct zclt_env_s
{
    struct zenv_s zenv;

    gchar uuid[32];
    gchar cell[32];
    struct zsock_s *zsock;
};

void zclt_env_init(const gchar *type, const gchar *target,
        struct zclt_env_s *ctx);

void zclt_env_close(struct zclt_env_s *ctx);

#endif // TECHFORUM_common_h
