#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TP_DIR="${ROOT_DIR}/third_party"

LIBUV_URL="https://github.com/libuv/libuv.git"
LIBUV_COMMIT="651f2fc1617331bc4cc6335dcacbbffe79d1fa60"

AS_URL="https://github.com/alibaba/async_simple.git"
AS_COMMIT="ddbd887eb17d30f86772c880b22dbfbb709f395f"

PICOHTTPPARSER_URL="https://github.com/h2o/picohttpparser.git"
PICOHTTPPARSER_COMMIT="f8326098f63eefabfa2b6ec595d90e9ed5ed958a"

BEARSSL_URL="https://github.com/oreparaz/bearssl.git"
BEARSSL_COMMIT="8590654686387c767294d95fe0e4e81a7ca07891"

mkdir -p "${TP_DIR}"

ensure_repo_at_commit() {
  local url="$1"
  local dir="$2"
  local commit="$3"

  if [[ ! -d "${dir}/.git" ]]; then
    git clone --no-checkout "${url}" "${dir}"
  fi

  git -C "${dir}" fetch --depth 1 origin "${commit}"
  git -C "${dir}" checkout --detach "${commit}"
}

ensure_repo_at_commit "${LIBUV_URL}" "${TP_DIR}/libuv" "${LIBUV_COMMIT}"
ensure_repo_at_commit "${AS_URL}" "${TP_DIR}/async_simple" "${AS_COMMIT}"
ensure_repo_at_commit "${PICOHTTPPARSER_URL}" "${TP_DIR}/picohttpparser" "${PICOHTTPPARSER_COMMIT}"
ensure_repo_at_commit "${BEARSSL_URL}" "${TP_DIR}/bearssl" "${BEARSSL_COMMIT}"

cat > "${TP_DIR}/deps.lock" <<EOF
libuv ${LIBUV_URL} ${LIBUV_COMMIT}
async_simple ${AS_URL} ${AS_COMMIT}
picohttpparser ${PICOHTTPPARSER_URL} ${PICOHTTPPARSER_COMMIT}
bearssl ${BEARSSL_URL} ${BEARSSL_COMMIT}
EOF

echo "Deps ready in ${TP_DIR}"
