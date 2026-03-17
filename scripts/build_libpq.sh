#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PG_SRC_DIR="${ROOT_DIR}/third_party/postgresql"
BUILDER_IMAGE="${BUILDER_IMAGE:-docker.io/library/debian:bookworm}"

if [[ ! -d "${PG_SRC_DIR}" ]]; then
  echo "missing source tree: ${PG_SRC_DIR}" >&2
  echo "run ./scripts/fetch_libpq_example_deps.sh first" >&2
  exit 1
fi

podman run --rm \
  -v "${ROOT_DIR}:/work" \
  -w /work \
  "${BUILDER_IMAGE}" \
  bash -lc '
    set -euo pipefail
    apt-get update
    DEBIAN_FRONTEND=noninteractive apt-get install -y \
      build-essential bison flex pkg-config perl python3

    mkdir -p /work/build/postgresql-container
    cd /work/build/postgresql-container

    /work/third_party/postgresql/configure \
      --without-readline \
      --without-zlib \
      --without-gssapi \
      --without-ldap \
      --without-icu \
      --without-pam \
      --without-openssl \
      --without-libcurl \
      --prefix=/work/third_party/postgresql-install

    make -C src/interfaces/libpq -j"$(nproc)"
    make -C src/interfaces/libpq install
  '

echo "Bundled libpq ready at ${ROOT_DIR}/third_party/postgresql-install"
