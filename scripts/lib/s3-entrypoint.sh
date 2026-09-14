#!/bin/sh
set -eu

rpc_secret_file="${RUSTFS_RPC_SECRET_FILE_PATH:?RUSTFS_RPC_SECRET_FILE_PATH is required}"

if [ ! -r "$rpc_secret_file" ]; then
  echo "RustFS RPC secret file is missing or unreadable" >&2
  exit 1
fi

RUSTFS_RPC_SECRET=$(tr -d '\r\n' < "$rpc_secret_file")
if [ -z "$RUSTFS_RPC_SECRET" ]; then
  echo "RustFS RPC secret file is empty" >&2
  exit 1
fi

export RUSTFS_RPC_SECRET
exec /usr/bin/rustfs server /data
