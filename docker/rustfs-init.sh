#!/bin/sh
set -eu

read_secret() {
  secret_path="$1"
  if [ ! -r "$secret_path" ]; then
    echo "Required RustFS secret is missing or unreadable: $secret_path" >&2
    exit 1
  fi

  secret_value=$(tr -d '\r\n' < "$secret_path")
  if [ -z "$secret_value" ]; then
    echo "Required RustFS secret is empty: $secret_path" >&2
    exit 1
  fi
  printf '%s' "$secret_value"
}

bucket=$(tr -d '\r\n' < "${RUSTFS_BUCKET_FILE:?RUSTFS_BUCKET_FILE is required}")
if [ -z "$bucket" ]; then
  echo "RustFS bucket file is empty" >&2
  exit 1
fi

access_key=$(read_secret /run/secrets/rustfs_access_key)
secret_key=$(read_secret /run/secrets/rustfs_secret_key)
app_access_key=$(read_secret /run/secrets/argus_s3_access_key)
app_secret_key=$(read_secret /run/secrets/argus_s3_secret_key)

/usr/bin/rc alias set argus "$RUSTFS_ENDPOINT" "$access_key" "$secret_key" \
  --region "$RUSTFS_REGION" --bucket-lookup path >/dev/null
/usr/bin/rc bucket create "argus/$bucket" --ignore-existing >/dev/null

policy_json=$(printf '{"Version":"2012-10-17","Statement":[{"Effect":"Allow","Action":["s3:ListBucket"],"Resource":["arn:aws:s3:::%s"]},{"Effect":"Allow","Action":["s3:GetObject","s3:PutObject","s3:DeleteObject"],"Resource":["arn:aws:s3:::%s/*"]}]}' "$bucket" "$bucket")

if ! /usr/bin/rc admin service-account info argus "$app_access_key" >/dev/null 2>&1; then
  /usr/bin/rc admin service-account create argus "$app_access_key" "$app_secret_key" \
    --name argus-backend --description "Argus private object storage" \
    --policy-json "$policy_json" >/dev/null
fi

/usr/bin/rc admin service-account info argus "$app_access_key" >/dev/null
/usr/bin/rc alias set argus-app "$RUSTFS_ENDPOINT" "$app_access_key" "$app_secret_key" \
  --region "$RUSTFS_REGION" --bucket-lookup path >/dev/null
/usr/bin/rc ls "argus-app/$bucket" >/dev/null

echo "RustFS private bucket and application service account are ready."
