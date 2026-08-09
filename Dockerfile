# GameMesh multi-role image (build once; start with role argv via entrypoint)
FROM centos:8 AS build
RUN sed -i 's/mirrorlist/#mirrorlist/g; s|#baseurl=http://mirror.centos.org|baseurl=http://vault.centos.org|g' /etc/yum.repos.d/CentOS-*.repo \
 && yum install -y gcc-c++ cmake make openssl-devel jsoncpp-devel protobuf-devel \
      mysql-devel hiredis-devel leveldb-devel gflags-devel zlib-devel \
 && yum clean all
# brpc 需预装到构建环境（镜像或挂载 /usr/local）；缺失时完整构建必须失败
WORKDIR /src
COPY . .
RUN test -f /usr/local/include/brpc/server.h -o -f /usr/include/brpc/server.h \
 && ENABLE_BRPC=ON ./scripts/build.sh Release

FROM centos:8
RUN sed -i 's/mirrorlist/#mirrorlist/g; s|#baseurl=http://mirror.centos.org|baseurl=http://vault.centos.org|g' /etc/yum.repos.d/CentOS-*.repo \
 && yum install -y openssl mysql-libs hiredis protobuf gflags leveldb zlib curl \
 && yum clean all \
 && useradd -r -u 10001 -d /opt/gamemesh -s /sbin/nologin gamemesh
WORKDIR /opt/gamemesh
COPY --from=build /src/build/test/gateway /src/build/test/session /src/build/test/gamelogic \
                  /src/build/test/world /src/build/test/gamedb /opt/gamemesh/
COPY config /opt/gamemesh/config
COPY deploy/docker-entrypoint.sh /opt/gamemesh/docker-entrypoint.sh
RUN chmod +x /opt/gamemesh/docker-entrypoint.sh \
 && chmod +x /opt/gamemesh/gateway /opt/gamemesh/session /opt/gamemesh/gamelogic \
             /opt/gamemesh/world /opt/gamemesh/gamedb \
 && chown -R gamemesh:gamemesh /opt/gamemesh
USER gamemesh
ENV GAMEMESH_FORMAL=1 GAMEMESH_HTTP_BIND=0.0.0.0 GAMEMESH_ADVERTISE_HOST=127.0.0.1 \
    GAMEMESH_BIN_DIR=/opt/gamemesh
ENTRYPOINT ["/opt/gamemesh/docker-entrypoint.sh"]
CMD ["gateway", "8080", "8081"]
