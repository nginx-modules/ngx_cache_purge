FROM debian:bookworm-slim

ARG NGINX_VERSION=1.25.5
ARG TEST_NGINX_REPO=https://github.com/openresty/test-nginx.git

ENV DEBIAN_FRONTEND=noninteractive
ENV NGINX_VERSION=${NGINX_VERSION}
ENV NGINX_SRC_DIR=/opt/nginx-src/nginx-${NGINX_VERSION}
ENV NGINX_BUILD_PREFIX=/opt/nginx
ENV PATH=${NGINX_BUILD_PREFIX}/sbin:${PATH}

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        astyle \
        bash \
        build-essential \
        ca-certificates \
        cpanminus \
        curl \
        dos2unix \
        git \
        libpcre3-dev \
        libssl-dev \
        perl \
        zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

RUN mkdir -p /opt/nginx-src \
    && curl -fsSLo /tmp/nginx.tar.gz "https://nginx.org/download/nginx-${NGINX_VERSION}.tar.gz" \
    && tar -xzf /tmp/nginx.tar.gz -C /opt/nginx-src \
    && rm /tmp/nginx.tar.gz

RUN git clone --depth=1 "${TEST_NGINX_REPO}" /opt/test-nginx \
    && cpanm --notest /opt/test-nginx

WORKDIR /workspace

CMD ["/bin/bash"]
