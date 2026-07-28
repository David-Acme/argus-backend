#!/usr/bin/env bash
set -e
echo "=============================================="
echo "  FACE AUTH INTEGRATION TEST"
echo "=============================================="

BASE64=$(cat ~/Downloads/person-image-b64.txt | tr -d '\n')
PROFILE="${1:-dev}"
BIN="./build/$PROFILE/argus-backend"

# 1. Setup
echo "[1/5] Setting up DB..."
  pkill -f "argus-backend" 2>/dev/null || true
  sleep 3
sqlite3 database/argus.db "DELETE FROM face_embedding; DELETE FROM person; DELETE FROM user;"
sqlite3 database/argus.db "INSERT INTO user (name,last_name,role) VALUES ('Admin','User','owner');"
USERID=$(sqlite3 database/argus.db "SELECT id FROM user LIMIT 1;")
sqlite3 database/argus.db "INSERT INTO person (user_id,name,alias) VALUES ($USERID,'Admin','admin');"
PID=$(sqlite3 database/argus.db "SELECT id FROM person WHERE user_id=$USERID LIMIT 1;")
echo "  User=$USERID Person=$PID"

# 2. Start server
echo "[2/5] Starting server ($PROFILE)..."
cd $(dirname "$BIN") && nohup ./argus-backend > /tmp/argus-test.log 2>&1 &
for i in $(seq 1 20); do
  if curl -s -o /dev/null -w "%{http_code}" http://localhost:7024/ 2>/dev/null | grep -q .; then
    echo "  Server ready after ${i}s"
    break
  fi
  sleep 2
done

# 3. Register
echo "[3/5] Registering face..."
REG=$(curl -s --max-time 30 http://localhost:7024/auth/register-face \
  -H "Content-Type: application/json" \
  -d "{\"image\": \"$BASE64\", \"personId\": $PID, \"label\": \"frontal\"}")
if echo "$REG" | grep -q "faceCount"; then
  echo "  ✓ Registered"
else
  echo "  ✗ FAIL: $REG"; exit 1
fi

# 4. Login
echo "[4/5] Login..."
LOGIN=$(curl -s --max-time 30 http://localhost:7024/auth/login \
  -H "Content-Type: application/json" \
  -d "{\"image\": \"$BASE64\"}")
if echo "$LOGIN" | grep -q "accessToken"; then
  echo "  ✓ Login OK - $(echo "$LOGIN" | python3 -c "import sys,json;d=json.load(sys.stdin);print(d['name'],d['role'])" 2>/dev/null || echo 'ok')"
else
  echo "  ✗ Login failed: $LOGIN"; exit 1
fi

# 5. Restart + re-login (test persistence)
echo "[5/5] Restart + persistence test..."
pkill -f argus-backend 2>/dev/null || true
sleep 2
cd $(dirname "$BIN") && nohup ./argus-backend > /tmp/argus2.log 2>&1 &
for i in $(seq 1 20); do
  if curl -s -o /dev/null -w "%{http_code}" http://localhost:7024/ 2>/dev/null | grep -q .; then
    echo "  Server ready after ${i}s"
    break
  fi
  sleep 2
done

LOGIN2=$(curl -s --max-time 30 http://localhost:7024/auth/login \
  -H "Content-Type: application/json" \
  -d "{\"image\": \"$BASE64\"}")
if echo "$LOGIN2" | grep -q "accessToken"; then
  echo "  ✓ Persistence OK - login works after restart"
else
  echo "  ✗ FAIL: $LOGIN2"; exit 1
fi

echo ""
echo "=============================================="
echo "  ALL TESTS PASSED ✓"
echo "=============================================="
