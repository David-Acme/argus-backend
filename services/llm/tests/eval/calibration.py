import math

EPSILON = 1e-6


def clip(value):
    return min(1.0 - EPSILON, max(EPSILON, value))


def logit(value):
    value = clip(value)
    return math.log(value / (1.0 - value))


def sigmoid(value):
    if value >= 0:
        return 1.0 / (1.0 + math.exp(-value))
    exp = math.exp(value)
    return exp / (1.0 + exp)


def fit_platt(pairs, temperature_only=False):
    scale, shift = 1.0, 0.0
    features = [(logit(confidence), 1.0 if correct else 0.0) for confidence, correct in pairs]
    for _ in range(60):
        g_scale = g_shift = h_ss = h_sb = h_bb = 0.0
        for x, y in features:
            p = sigmoid(scale * x + shift)
            error = p - y
            weight = max(p * (1.0 - p), 1e-9)
            g_scale += error * x
            g_shift += error
            h_ss += weight * x * x
            h_sb += weight * x
            h_bb += weight
        n = len(features)
        g_scale, g_shift, h_ss, h_sb, h_bb = g_scale / n, g_shift / n, h_ss / n, h_sb / n, h_bb / n
        if temperature_only:
            step_scale, step_shift = g_scale / (h_ss + 1e-9), 0.0
        else:
            determinant = h_ss * h_bb - h_sb * h_sb
            if abs(determinant) < 1e-12:
                step_scale, step_shift = g_scale / (h_ss + 1e-9), g_shift / (h_bb + 1e-9)
            else:
                step_scale = (h_bb * g_scale - h_sb * g_shift) / determinant
                step_shift = (h_ss * g_shift - h_sb * g_scale) / determinant
        scale -= step_scale
        shift -= step_shift
        if abs(step_scale) < 1e-9 and abs(step_shift) < 1e-9:
            break
    return {"type": "platt", "scale": scale, "shift": shift, "temperature": 1.0 / scale if scale else math.inf}


def fit_isotonic(pairs):
    ordered = sorted(pairs)
    blocks = []
    for confidence, correct in ordered:
        blocks.append([confidence, confidence, float(correct), 1.0])
        while len(blocks) > 1 and blocks[-2][2] / blocks[-2][3] > blocks[-1][2] / blocks[-1][3]:
            last = blocks.pop()
            blocks[-1][1] = last[1]
            blocks[-1][2] += last[2]
            blocks[-1][3] += last[3]
    return {"type": "isotonic", "lower": [b[0] for b in blocks], "upper": [b[1] for b in blocks],
            "value": [b[2] / b[3] for b in blocks]}


def fit(pairs, kind):
    if not pairs:
        return {"type": "identity"}
    if kind == "temperature":
        return fit_platt(pairs, temperature_only=True)
    if kind == "platt":
        return fit_platt(pairs)
    if kind == "isotonic":
        return fit_isotonic(pairs)
    raise ValueError(kind)


def apply(model, confidence):
    kind = model["type"]
    if kind == "identity":
        return confidence
    if kind == "platt":
        return sigmoid(model["scale"] * logit(confidence) + model["shift"])
    lower, upper, value = model["lower"], model["upper"], model["value"]
    if confidence <= lower[0]:
        return value[0]
    if confidence >= upper[-1]:
        return value[-1]
    low, high = 0, len(lower) - 1
    while low < high:
        middle = (low + high + 1) // 2
        if lower[middle] <= confidence:
            low = middle
        else:
            high = middle - 1
    if confidence <= upper[low] or low == len(lower) - 1:
        return value[low]
    span = lower[low + 1] - upper[low]
    weight = (confidence - upper[low]) / span if span > 0 else 0.0
    return value[low] + weight * (value[low + 1] - value[low])


def reliability(pairs, bins=10):
    table = [[0, 0.0, 0.0] for _ in range(bins)]
    for confidence, correct in pairs:
        index = min(bins - 1, int(confidence * bins))
        table[index][0] += 1
        table[index][1] += confidence
        table[index][2] += 1.0 if correct else 0.0
    rows = []
    for index, (count, confidence_sum, correct_sum) in enumerate(table):
        if count:
            rows.append({"low": index / bins, "high": (index + 1) / bins, "count": count,
                         "confidence": confidence_sum / count, "accuracy": correct_sum / count})
    return rows


def expected_calibration_error(pairs, bins=10):
    if not pairs:
        return 0.0
    return sum(row["count"] * abs(row["confidence"] - row["accuracy"]) for row in reliability(pairs, bins)) / len(pairs)


def evaluate(model, pairs, bins=10):
    calibrated = [(apply(model, confidence), correct) for confidence, correct in pairs]
    return {"ece": expected_calibration_error(calibrated, bins), "pairs": len(pairs)}
