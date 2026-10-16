#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>

typedef unsigned char u_char;
typedef unsigned long ngx_uint_t;
typedef long ngx_int_t;
typedef struct { void *elts; ngx_uint_t nelts; } ngx_array_t;
typedef struct { in_addr_t addr, mask; } ngx_in_cidr_t;
typedef struct { struct in6_addr addr, mask; } ngx_in6_cidr_t;

#define NGX_HAVE_INET6 1
#define NGX_OK 0
#define NGX_DECLINED -5
