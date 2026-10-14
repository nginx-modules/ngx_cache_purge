int
main(void)
{
    struct sockaddr_in6  peer;
    ngx_in_cidr_t        cidr;
    ngx_array_t          access;

    memset(&peer, 0, sizeof(peer));
    peer.sin6_family = AF_INET6;
    peer.sin6_addr.s6_addr[10] = 0xff;
    peer.sin6_addr.s6_addr[11] = 0xff;
    peer.sin6_addr.s6_addr[12] = 203;
    peer.sin6_addr.s6_addr[13] = 0;
    peer.sin6_addr.s6_addr[14] = 113;
    peer.sin6_addr.s6_addr[15] = 5;
    cidr.addr = htonl(0xcb007105);
    cidr.mask = (in_addr_t) -1;
    access.elts = &cidr;
    access.nelts = 1;

    assert(ngx_http_cache_purge_access(&access, NULL,
                     (struct sockaddr *) &peer) == NGX_OK);
    peer.sin6_addr.s6_addr[15] = 6;
    assert(ngx_http_cache_purge_access(&access, NULL,
                     (struct sockaddr *) &peer) == NGX_DECLINED);
    assert(ngx_http_cache_purge_access(NULL, NULL,
                     (struct sockaddr *) &peer) == NGX_DECLINED);
    return 0;
}
