# t/Dockerfile — build context is always the repo root
ARG UBUNTU_VERSION=24.04
FROM ubuntu:${UBUNTU_VERSION} AS tools

ENV DEBIAN_FRONTEND=noninteractive

# The workflow selects a mirror with vegardit/fast-apt-mirror.sh. BuildKit
# secrets carry that URL and the runner's CA bundle without varying cache keys.
# Local builds keep the distribution defaults when no secrets are provided.
RUN printf '%s\n' 'Acquire::Retries "3";' \
        'Acquire::http::Timeout "30";' 'Acquire::https::Timeout "30";' \
        > /etc/apt/apt.conf.d/80-network-retries

# Older Ubuntu clang packages bundle sanitizer runtimes in their dependencies.
# Install the separate package where available and verify both runtimes link.
RUN --mount=type=secret,id=apt_mirror \
    --mount=type=secret,id=apt_ca \
    if [ -s /run/secrets/apt_mirror ]; then \
        mirror="$(cat /run/secrets/apt_mirror)"; \
        if [ -f /etc/apt/sources.list.d/ubuntu.sources ]; then \
            sources=/etc/apt/sources.list.d/ubuntu.sources; \
        else sources=/etc/apt/sources.list; fi; \
        sed -i "s|http://archive.ubuntu.com/ubuntu/|$mirror|g" "$sources"; \
    fi && \
    ca_option="" && \
    if [ -s /run/secrets/apt_ca ]; then \
        ca_option="-o Acquire::https::CaInfo=/run/secrets/apt_ca"; \
    fi && \
    apt-get $ca_option update && apt-get $ca_option install -y \
    ca-certificates \
    build-essential \
    clang \
    valgrind \
    cppcheck \
    libpcre3-dev \
    libssl-dev \
    zlib1g-dev \
    libgd-dev \
    libgeoip-dev \
    libxml2-dev \
    libxslt1-dev \
    wget \
    curl \
    git \
    perl \
    cpanminus \
    && if apt-cache show libclang-rt-dev >/dev/null 2>&1; then \
        apt-get $ca_option install -y libclang-rt-dev; \
    fi \
    && printf '%s\n' 'int main(void) { return 0; }' \
        | clang -fsanitize=address,undefined -x c - -o /tmp/clang-runtime-check \
    && rm -f /tmp/clang-runtime-check \
    && rm -rf /var/lib/apt/lists/*

# Install Test::Nginx and dependencies
RUN cpanm --mirror https://cpan.metacpan.org --mirror-only --notest \
    Test::Nginx \
    Test::More \
    Test::Base \
    Test::LongString \
    List::MoreUtils \
    IO::Socket::SSL \
    TAP::Formatter::JUnit

WORKDIR /src
COPY . /src/

FROM tools AS source
ARG NGINX_VERSION=1.31.6
RUN mkdir -p /opt/nginx && \
    sh t/download.sh "https://nginx.org/download/nginx-${NGINX_VERSION}.tar.gz" /tmp/nginx.tar.gz && \
    tar -xzf /tmp/nginx.tar.gz --strip-components=1 -C /opt/nginx && \
    rm /tmp/nginx.tar.gz
ENV NGINX_SOURCE=/opt/nginx \
    TEST_NGINX_BINARY=/opt/nginx/objs/nginx

# Strict GCC/Clang and sanitizer jobs build their selected configuration at run time.
FROM source AS verify
CMD ["perl", "t/build_verify.pl", "--nginx-source", "/opt/nginx"]

FROM source AS integration
# Build nginx with the module
RUN cd /opt/nginx && \
    ./configure \
        --prefix=/etc/nginx \
        --sbin-path=/usr/sbin/nginx \
        --modules-path=/usr/lib/nginx/modules \
        --conf-path=/etc/nginx/nginx.conf \
        --error-log-path=/var/log/nginx/error.log \
        --http-log-path=/var/log/nginx/access.log \
        --pid-path=/var/run/nginx.pid \
        --lock-path=/var/run/nginx.lock \
        --http-client-body-temp-path=/var/cache/nginx/client_temp \
        --http-proxy-temp-path=/var/cache/nginx/proxy_temp \
        --http-fastcgi-temp-path=/var/cache/nginx/fastcgi_temp \
        --http-uwsgi-temp-path=/var/cache/nginx/uwsgi_temp \
        --http-scgi-temp-path=/var/cache/nginx/scgi_temp \
        --with-http_ssl_module \
        --with-http_realip_module \
        --with-http_addition_module \
        --with-http_sub_module \
        --with-http_dav_module \
        --with-http_flv_module \
        --with-http_mp4_module \
        --with-http_gunzip_module \
        --with-http_gzip_static_module \
        --with-http_random_index_module \
        --with-http_secure_link_module \
        --with-http_stub_status_module \
        --with-http_auth_request_module \
        --with-http_xslt_module=dynamic \
        --with-http_image_filter_module=dynamic \
        --with-http_geoip_module=dynamic \
        --with-debug \
        --with-threads \
        --with-stream \
        --with-stream_ssl_module \
        --with-stream_ssl_preread_module \
        --with-stream_realip_module \
        --with-stream_geoip_module=dynamic \
        --with-http_slice_module \
        --with-file-aio \
        --with-http_v2_module \
        --add-module=/src && \
    make -j2 && \
    make install

# Create nginx cache directories
RUN mkdir -p /var/cache/nginx/client_temp \
    /var/cache/nginx/proxy_temp \
    /var/cache/nginx/fastcgi_temp \
    /var/cache/nginx/uwsgi_temp \
    /var/cache/nginx/scgi_temp

ENV TEST_NGINX_THREADS=1

# With no file arguments, prove discovers every top-level t/*.t suite.
CMD ["sh", "t/run_tests.sh"]
