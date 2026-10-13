#!/bin/sh
set -eu

endpoint="${RUSTFS_ENDPOINT:?RUSTFS_ENDPOINT is required}"
region="${RUSTFS_REGION:-us-east-1}"
bucket_file="${RUSTFS_BUCKET_FILE:?RUSTFS_BUCKET_FILE is required}"
secrets="${RUSTFS_SECRETS_DIR:-/run/secrets}"

for name in rustfs_access_key rustfs_secret_key argus_s3_access_key argus_s3_secret_key; do
  if [ ! -r "$secrets/$name" ] || [ ! -s "$secrets/$name" ]; then
    echo "Required RustFS secret is missing, unreadable or empty: $secrets/$name" >&2
    exit 1
  fi
done

bucket=$(tr -d '\r\n' < "$bucket_file")
if [ -z "$bucket" ]; then
  echo "RustFS bucket file is empty" >&2
  exit 1
fi

umask 077
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

secret() {
  tr -d '\r\n' < "$secrets/$1"
}

{
  printf 'user = "'
  secret rustfs_access_key
  printf ':'
  secret rustfs_secret_key
  printf '"\n'
} > "$work/root.curl"

signed() {
  curl -q -sS -K "$work/root.curl" --aws-sigv4 "aws:amz:$region:s3" "$@"
}

status=$(signed -X PUT -o "$work/bucket.out" -w '%{http_code}' "$endpoint/$bucket")
case "$status" in
  200) ;;
  409)
    grep -q 'BucketAlreadyOwnedByYou' "$work/bucket.out" || {
      cat "$work/bucket.out" >&2
      echo "RustFS bucket $bucket exists and is not ours" >&2
      exit 1
    }
    ;;
  *)
    cat "$work/bucket.out" >&2
    echo "RustFS bucket could not be created (HTTP $status)" >&2
    exit 1
    ;;
esac

policy=$(printf '{"Version":"2012-10-17","Statement":[{"Effect":"Allow","Action":["s3:ListBucket"],"Resource":["arn:aws:s3:::%s"]},{"Effect":"Allow","Action":["s3:GetObject","s3:PutObject","s3:DeleteObject"],"Resource":["arn:aws:s3:::%s/*"]}]}' "$bucket" "$bucket" | sed 's/"/\\"/g')

status=$({
  printf '{"name":"argus-backend","description":"Argus private object storage","accessKey":"'
  secret argus_s3_access_key
  printf '","secretKey":"'
  secret argus_s3_secret_key
  printf '","policy":"%s"}' "$policy"
} | signed -X PUT -H 'content-type: application/json' --data-binary @- \
  -o "$work/account.out" -w '%{http_code}' "$endpoint/rustfs/admin/v3/add-service-accounts")
case "$status" in
  200) ;;
  *)
    grep -qE 'already in use|already exists' "$work/account.out" || {
      cat "$work/account.out" >&2
      echo "RustFS application service account could not be created (HTTP $status)" >&2
      exit 1
    }
    ;;
esac

{
  printf 'url = "%s/rustfs/admin/v3/info-service-account?accessKey=' "$endpoint"
  secret argus_s3_access_key
  printf '"\n'
} > "$work/info.curl"
status=$(signed -K "$work/info.curl" -o "$work/info.out" -w '%{http_code}')
if [ "$status" != 200 ]; then
  echo "RustFS application service account could not be verified (HTTP $status)" >&2
  exit 1
fi

echo "RustFS private bucket and application service account are ready."
