#ifndef TECHFORUM_macros_h
# define TECHFORUM_macros_h 1

# ifdef HAVE_ASSERT
#  define ASSERT(X) g_assert(X)
# else
#  define ASSERT(X)
# endif

#ifndef GQUARK
# define GQUARK() g_quark_from_static_string(G_LOG_DOMAIN)
#endif

#ifndef NEWERROR
# define NEWERROR(Code,Fmt,...) g_error_new(GQUARK(), (Code), (Fmt), ##__VA_ARGS__)
#endif

#endif // TECHFORUM_macros_h
