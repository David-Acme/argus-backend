import json
import sys

replies = json.loads(open(sys.argv[1]).read())
for line in sys.stdin:
    request = json.loads(line)
    print(json.dumps({"seq": request["seq"], "reply": replies[request["user"]]}, ensure_ascii=False), flush=True)
