# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

FROM ubuntu:22.10 as build

ARG MORE_BUILD_ARGS

# workaround tzdata install hanging
ENV TZ=Asia/Shanghai
RUN ln -snf /usr/share/zoneinfo/$TZ /etc/localtime && echo $TZ > /etc/timezone

RUN apt update && apt install -y git gcc g++ make cmake autoconf automake libtool python3 libssl-dev curl libjemalloc-dev libaio-dev libgflags-dev zlib1g-dev libbz2-dev libcurl4-gnutls-dev liburing-dev apt-utils pkg-config

WORKDIR /kvrocks

ENV redis_version=7.2-rc2
RUN curl -O https://download.redis.io/releases/redis-$redis_version.tar.gz && \
    tar -xzvf redis-$redis_version.tar.gz && \
    mkdir tools && \
    cd redis-$redis_version && \
    make redis-cli && \
    mv src/redis-cli /kvrocks/tools/redis-cli

ENV TOPLINGDB_HOME=/tmp/toplingdb 
ENV TOPLINGDB_LIBDIR=/opt/topling/lib 

RUN cd /tmp && git clone --depth=1 https://github.com/topling/toplingdb.git && \
    cd toplingdb && make -j`nproc` DEBUG_LEVEL=0 INSTALL_LIBDIR=$TOPLINGDB_LIBDIR install

COPY . .

RUN ./x.py build -j`nproc` -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_OPENSSL=ON -DPORTABLE=ON $MORE_BUILD_ARGS

FROM ubuntu:22.10

RUN apt update && apt install -y libssl-dev libjemalloc-dev libaio-dev libgflags-dev zlib1g-dev libbz2-dev libcurl4-gnutls-dev liburing-dev libgomp1

WORKDIR /kvrocks

COPY --from=build /kvrocks/build/kvrocks ./bin/
COPY --from=build /opt/topling/lib /opt/topling/lib
ENV LD_LIBRARY_PATH=/opt/topling/lib

COPY --from=build /kvrocks/tools/redis-cli ./bin/
ENV PATH="$PATH:/kvrocks/bin"

VOLUME /var/lib/kvrocks

COPY ./LICENSE ./NOTICE ./DISCLAIMER ./
COPY ./licenses ./licenses
COPY ./kvrocks.conf  /var/lib/kvrocks/
COPY ./docker/* /kvrocks/

EXPOSE 6666:6666

ENTRYPOINT ["/kvrocks/run.sh"]
