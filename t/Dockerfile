# t/Dockerfile — build context is always the repo root
ARG UBUNTU_VERSION=22.04
FROM ubuntu:${UBUNTU_VERSION}

ENV DEBIAN_FRONTEND=noninteractive

# Allow nginx version to be passed in from CI matrix
ARG NGINX_VERSION=1.24.0
ENV NGINX_VERSION=${NGINX_VERSION}

# Prioritized mirrors
ARG UBUNTU_MIRROR=azure
RUN . /etc/os-release && \
    \
    # Resolve mirror URL from ARG
    case "${UBUNTU_MIRROR}" in \
        azure)   MIRROR_URL="http://azure.archive.ubuntu.com/ubuntu/" ;; \
        kernel)  MIRROR_URL="http://mirrors.kernel.org/ubuntu/" ;; \
        *)       echo "Unknown UBUNTU_MIRROR: ${UBUNTU_MIRROR}"; exit 1 ;; \
    esac && \
    \
    # 1. Identify the active config file (Standard vs 24.04+ DEB822 format)
    if [ -f /etc/apt/sources.list.d/ubuntu.sources ]; then \
        TARGET="/etc/apt/sources.list.d/ubuntu.sources"; \
        # For 24.04+, replace URIs with selected mirror
        sed -i "s|http://archive.ubuntu.com/ubuntu/|${MIRROR_URL}|g"  "$TARGET"; \
        sed -i "s|http://security.ubuntu.com/ubuntu/|${MIRROR_URL}|g" "$TARGET"; \
    else \
        TARGET="/etc/apt/sources.list"; \
        # For 22.04 and below, replace URIs with selected mirror
        sed -i "s|http://archive.ubuntu.com/ubuntu/|${MIRROR_URL}|g"  "$TARGET"; \
        sed -i "s|http://security.ubuntu.com/ubuntu/|${MIRROR_URL}|g" "$TARGET"; \
    fi

# Install dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
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
    && rm -rf /var/lib/apt/lists/*

# Install Test::Nginx and dependencies
RUN cpanm --notest \
    Test::Nginx \
    Test::More \
    Test::Base \
    Test::LongString \
    List::MoreUtils \
    IO::Socket::SSL \
    TAP::Formatter::JUnit

# Download and extract nginx
RUN wget http://nginx.org/download/nginx-${NGINX_VERSION}.tar.gz \
    && tar xzf nginx-${NGINX_VERSION}.tar.gz \
    && rm nginx-${NGINX_VERSION}.tar.gz

# Create working directory
WORKDIR /src

# Copy module source
COPY . /src/

# Build nginx with the module
RUN cd /nginx-${NGINX_VERSION} && \
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
    make && \
    make install

# Create nginx cache directories
RUN mkdir -p /var/cache/nginx/client_temp \
    /var/cache/nginx/proxy_temp \
    /var/cache/nginx/fastcgi_temp \
    /var/cache/nginx/uwsgi_temp \
    /var/cache/nginx/scgi_temp

# Set working directory for tests
WORKDIR /src

# Create convenience test runner script
RUN echo '#!/bin/bash\n\
set -e\n\
\n\
echo "Starting nginx cache purge module tests..."\n\
\n\
rm -rf cache/ logs/ t/servroot/\n\
\n\
mkdir -p cache logs\n\
\n\
echo "Running basic tests..."\n\
prove -v t/basic.t\n\
\n\
echo "Running background queue tests..."\n\
prove -v t/background_queue.t\n\
\n\
echo "Running configuration tests..."\n\
prove -v t/config.t\n\
\n\
echo "Running memory tests..."\n\
prove -v t/memory.t\n\
\n\
echo "Running performance tests..."\n\
prove -v t/performance.t\n\
\n\
echo "All tests completed successfully!"\n\
' > /usr/local/bin/run_tests.sh && chmod +x /usr/local/bin/run_tests.sh

# Default command
CMD ["/usr/local/bin/run_tests.sh"]
