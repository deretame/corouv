#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="${ROOT_DIR}/third_party/mysql-client/lib"
PLUGIN_DIR="${OUT_DIR}/plugin"
BUILDER_IMAGE="${BUILDER_IMAGE:-docker.io/library/debian:bookworm}"

podman run --rm \
  -v "${ROOT_DIR}:/work" \
  -w /work \
  "${BUILDER_IMAGE}" \
  bash -lc '
    set -euo pipefail
    apt-get update
    DEBIAN_FRONTEND=noninteractive apt-get install -y libmariadb3

    mkdir -p /work/third_party/mysql-client/lib
    cp -a /usr/lib/x86_64-linux-gnu/libmariadb.so.3 \
      /work/third_party/mysql-client/lib/libmariadb.so.3
    ln -sf libmariadb.so.3 /work/third_party/mysql-client/lib/libmariadb.so
    if [ -d /usr/lib/x86_64-linux-gnu/libmariadb3/plugin ]; then
      rm -rf /work/third_party/mysql-client/lib/plugin
      cp -a /usr/lib/x86_64-linux-gnu/libmariadb3/plugin \
        /work/third_party/mysql-client/lib/plugin
    fi
  '

echo "MySQL client library ready at ${OUT_DIR}"
echo "You can export:"
echo "  COROUV_SQL_MYSQL_LIB=${OUT_DIR}/libmariadb.so.3"
echo "  MARIADB_PLUGIN_DIR=${PLUGIN_DIR}"
