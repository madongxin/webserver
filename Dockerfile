# GameMesh multi-role image (build once, run with role argv)
FROM centos:8 AS build
RUN sed -i 's/mirrorlist/#mirrorlist/g; s|#baseurl=http://mirror.centos.org|baseurl=http://vault.centos.org|g' /etc/yum.repos.d/CentOS-*.repo \
 && yum install -y gcc-c++ cmake make openssl-devel jsoncpp-devel protobuf-devel \
      mysql-devel hiredis-devel leveldb-devel gflags-devel zlib-devel \
 && yum clean all
# brpc 需预装或挂载；此处假设系统已有 /usr/local
WORKDIR /src
COPY . .
RUN ./scripts/build.sh Release || ./scripts/build.sh Debug

FROM centos:8
RUN sed -i 's/mirrorlist/#mirrorlist/g; s|#baseurl=http://mirror.centos.org|baseurl=http://vault.centos.org|g' /etc/yum.repos.d/CentOS-*.repo \
 && yum install -y openssl mysql-libs hiredis protobuf gflags leveldb zlib \
 && yum clean all
WORKDIR /opt/gamemesh
COPY --from=build /src/build/test/gateway /src/build/test/session /src/build/test/gamelogic \
                  /src/build/test/world /src/build/test/gamedb /opt/gamemesh/
COPY config /opt/gamemesh/config
ENV GAMEMESH_FORMAL=1 GAMEMESH_HTTP_BIND=0.0.0.0 GAMEMESH_ADVERTISE_HOST=127.0.0.1
ENTRYPOINT ["/opt/gamemesh/gateway"]
