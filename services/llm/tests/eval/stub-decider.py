import json
import sys

RULES = (("agenda", "calendar.create_event", 0.95), ("tarea", "task.create", 0.95),
         ("dudoso", "calendar.create_event", 0.55), ("recuerda", "memory.remember", 0.9), ("certeza", "calendar.create_event", 1.0))

for line in sys.stdin:
    request = json.loads(line)
    text = request["text"].lower()
    answer = {"seq": request["seq"], "tool": None, "confidence": 0.0}
    for needle, tool, confidence in RULES:
        if needle in text and tool in request["tools"]:
            answer = {"seq": request["seq"], "tool": tool, "confidence": confidence}
            break
    print(json.dumps(answer), flush=True)
