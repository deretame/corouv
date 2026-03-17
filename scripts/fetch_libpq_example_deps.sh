#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TP_DIR="${ROOT_DIR}/third_party"

PG_URL="https://github.com/postgres/postgres.git"
PG_COMMIT="d8a859d22b1accd8ea749394a488e4de014b2396"

mkdir -p "${TP_DIR}"

if [[ ! -d "${TP_DIR}/postgresql/.git" ]]; then
  git clone --no-checkout "${PG_URL}" "${TP_DIR}/postgresql"
fi

git -C "${TP_DIR}/postgresql" fetch --depth 1 origin "${PG_COMMIT}"
git -C "${TP_DIR}/postgresql" checkout --detach "${PG_COMMIT}"

cat > "${ROOT_DIR}/examples/example-deps.lock" <<EOF
postgresql ${PG_URL} ${PG_COMMIT}
EOF

echo "Example deps ready in ${TP_DIR}/postgresql"

