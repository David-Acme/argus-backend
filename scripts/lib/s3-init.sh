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

accounts="${RUSTFS_ACCOUNTS_DIR:-}"
[ -n "$accounts" ] || exit 0

policy_for() {
  conditions=""
  resources=""
  for prefix in "$@"; do
    conditions="$conditions${conditions:+,}\"$prefix*\""
    resources="$resources${resources:+,}\"arn:aws:s3:::$bucket/$prefix*\""
  done
  printf '{"Version":"2012-10-17","Statement":[{"Effect":"Allow","Action":["s3:ListBucket"],"Resource":["arn:aws:s3:::%s"],"Condition":{"StringLike":{"s3:prefix":[%s]}}},{"Effect":"Allow","Action":["s3:GetObject","s3:PutObject","s3:DeleteObject"],"Resource":[%s]}]}' \
    "$bucket" "$conditions" "$resources" | sed 's/"/\\"/g'
}

access_key_of() {
  awk -F'"' '{ for (i = 1; i < NF; i++) if ($i == "accessKey") { printf "%s", $(i + 2); exit } }' "$1"
}

remove_accounts_named() {
  status=$(signed -o "$work/list.out" -w '%{http_code}' "$endpoint/rustfs/admin/v3/list-service-accounts")
  if [ "$status" != 200 ]; then
    echo "RustFS service accounts could not be listed (HTTP $status)" >&2
    exit 1
  fi
  rm -f "$work"/orphan-*.curl
  tr '{' '\n' < "$work/list.out" | awk -F'"' -v name="$1" -v endpoint="$endpoint" -v dir="$work" '
    {
      key = ""
      named = 0
      for (i = 1; i < NF; i++) {
        if ($i == "accessKey")
          key = $(i + 2)
        if ($i == "name" && $(i + 2) == name)
          named = 1
      }
      if (named && key != "") {
        count++
        target = dir "/orphan-" count ".curl"
        printf "url = \"%s/rustfs/admin/v3/delete-service-accounts?accessKey=%s\"\n", endpoint, key > target
        close(target)
      }
    }'
  for orphan in "$work"/orphan-*.curl; do
    [ -f "$orphan" ] || continue
    status=$(signed -X DELETE -K "$orphan" -o "$work/delete.out" -w '%{http_code}')
    if [ "$status" != 200 ]; then
      echo "RustFS left-over account $1 could not be removed (HTTP $status)" >&2
      exit 1
    fi
  done
}

ensure_account() {
  service="$1"
  shift
  dir="$accounts/$service"
  file="$dir/credentials.json"
  if [ ! -d "$dir" ] || [ ! -w "$dir" ]; then
    echo "RustFS account directory $dir is missing or not writable" >&2
    exit 1
  fi
  if [ -s "$file" ]; then
    {
      printf 'url = "%s/rustfs/admin/v3/info-service-account?accessKey=' "$endpoint"
      access_key_of "$file"
      printf '"\n'
    } > "$work/info-$service.curl"
    status=$(signed -K "$work/info-$service.curl" -o "$work/info-$service.out" -w '%{http_code}')
    case "$status" in
      200)
        echo "RustFS account for $service is ready."
        return 0
        ;;
      404) ;;
      *)
        echo "RustFS account for $service could not be checked (HTTP $status)" >&2
        exit 1
        ;;
    esac
  fi
  remove_accounts_named "argus-$service"
  status=$(printf '{"name":"argus-%s","description":"Argus %s private objects","policy":"%s"}' \
      "$service" "$service" "$(policy_for "$@")" \
    | signed -X PUT -H 'content-type: application/json' --data-binary @- \
      -o "$dir/.credentials.json.new" -w '%{http_code}' "$endpoint/rustfs/admin/v3/add-service-accounts")
  if [ "$status" != 200 ] || ! grep -q '"secretKey"' "$dir/.credentials.json.new"; then
    rm -f "$dir/.credentials.json.new"
    echo "RustFS account for $service could not be created (HTTP $status)" >&2
    exit 1
  fi
  mv "$dir/.credentials.json.new" "$file"
  echo "RustFS account for $service is ready, with new keys."
}

ensure_account identity portraits/ faces/
ensure_account camera cameras/
ensure_account guard guard/incidents/
