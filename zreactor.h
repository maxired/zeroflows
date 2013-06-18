#ifndef TECHFORUM_zreactor_h
# define TECHFORUM_zreactor_h 1
# include <zmq.h>
# include <zookeeper.h>

struct zreactor_s;

typedef int (*zreactor_fn_fd)  (void *u, int fd, int e);

typedef int (*zreactor_fn_zmq) (void *u, void *s, int e);

struct zreactor_s* zreactor_create(void);

void zreactor_destroy(struct zreactor_s *zr);

void zreactor_stop(struct zreactor_s *zr);

int zreactor_run(struct zreactor_s *zr);

void zreactor_add_zk(struct zreactor_s *zr, zhandle_t *zh);

void zreactor_add_fd(struct zreactor_s *zr, int fd, int *evt,
        zreactor_fn_fd fn, gpointer fnu);

void zreactor_add_zmq(struct zreactor_s *zr, void *s, int *evt,
        zreactor_fn_zmq fn, gpointer fnu);

#endif
