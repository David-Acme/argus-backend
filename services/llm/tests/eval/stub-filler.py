import json
import sys

answers = json.loads(open(sys.argv[1]).read())
for line in sys.stdin:
    request = json.loads(line)
    answer = answers.get(request["seq"], {"args": {}, "missing": request["required"]})
    print(json.dumps({"seq": request["seq"], **answer}, ensure_ascii=False), flush=True)
