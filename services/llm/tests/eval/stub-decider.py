import json
import sys

RULES = (("agenda", "calendar.create_event", 0.95, None, 0.0),
         ("tarea", "task.create", 0.95, None, 0.0),
         ("dudoso", "calendar.create_event", 0.55, None, 0.0),
         ("recuerda", "memory.remember", 0.9, None, 0.0),
         ("certeza", "calendar.create_event", 1.0, None, 0.0),
         ("quizas", "calendar.create_event", 0.8, "memory.remind", 0.7))

for line in sys.stdin:
    request = json.loads(line)
    text = request["text"].lower()
    answer = {"seq": request["seq"], "tool": None, "confidence": 0.0}
    for needle, tool, confidence, runner, runner_confidence in RULES:
        if needle in text and tool in request["tools"]:
            answer = {"seq": request["seq"], "tool": tool, "confidence": confidence}
            if runner:
                answer["runnerUp"] = {"tool": runner, "confidence": runner_confidence}
            break
    print(json.dumps(answer), flush=True)
