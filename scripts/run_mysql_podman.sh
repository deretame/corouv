#!/usr/bin/env bash
set -euo pipefail

NAME="${NAME:-my-mysql}"
PORT="${PORT:-3306}"
IMAGE="${IMAGE:-docker.io/library/mysql:8.4}"

ROOT_PASSWORD="${ROOT_PASSWORD:-mysecretpassword}"
DATABASE="${DATABASE:-corouv}"
USER_NAME="${USER_NAME:-corouv}"
USER_PASSWORD="${USER_PASSWORD:-corouvpass}"

if podman ps -a --format '{{.Names}}' | grep -qx "${NAME}"; then
  echo "container ${NAME} already exists"
  exit 0
fi

podman run -d \
  --name "${NAME}" \
  -e MYSQL_ROOT_PASSWORD="${ROOT_PASSWORD}" \
  -e MYSQL_DATABASE="${DATABASE}" \
  -e MYSQL_USER="${USER_NAME}" \
  -e MYSQL_PASSWORD="${USER_PASSWORD}" \
  -p "${PORT}:3306" \
  "${IMAGE}"

echo "mysql started: ${NAME}"
echo "example url: mysql://${USER_NAME}:${USER_PASSWORD}@127.0.0.1:${PORT}/${DATABASE}"
