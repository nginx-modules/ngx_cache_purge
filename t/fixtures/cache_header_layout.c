#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <stdio.h>

#define FIELD(name) printf("\"" #name "\":[%lu,%lu]", \
    (unsigned long) offsetof(ngx_http_file_cache_header_t, name), \
    (unsigned long) sizeof(((ngx_http_file_cache_header_t *) 0)->name))

int main(void)
{
    printf("{\"size\":%lu,\"fields\":{",
           (unsigned long) sizeof(ngx_http_file_cache_header_t));
    FIELD(crc32); printf(",");
    FIELD(header_start); printf(",");
    FIELD(body_start); printf(",");
    FIELD(etag_len); printf(",");
    FIELD(vary_len);
    printf("}}\n");
    return 0;
}
