#!/usr/bin/env bash
set -euo pipefail

NAME="${NAME:-my-postgres}"
PASSWORD="${PASSWORD:-mysecretpassword}"
PORT="${PORT:-5432}"
IMAGE="${IMAGE:-docker.io/library/postgres}"

if podman ps -a --format '{{.Names}}' | grep -qx "${NAME}"; then
  echo "container ${NAME} already exists"
  exit 0
fi

podman run -d \
  --name "${NAME}" \
  -e POSTGRES_PASSWORD="${PASSWORD}" \
  -p "${PORT}:5432" \
  "${IMAGE}"

