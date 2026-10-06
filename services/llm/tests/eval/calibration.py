import math

EPSILON = 1e-6
RIDGE = 1e-6
MIN_SCALE = 1e-3
MAX_SHIFT = 50.0
MAX_ITERATIONS = 100


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


def log_loss(features, scale, shift):
    total = 0.0
    for x, y in features:
        z = scale * x + shift
        total += max(z, 0.0) + math.log1p(math.exp(-abs(z))) - y * z
    return total / len(features)


def newton_step(features, scale, shift, temperature_only):
    g_scale = g_shift = h_ss = h_sb = h_bb = 0.0
    for x, y in features:
        p = sigmoid(scale * x + shift)
        error = p - y
        weight = p * (1.0 - p)
        g_scale += error * x
        g_shift += error
        h_ss += weight * x * x
        h_sb += weight * x
        h_bb += weight
    n = len(features)
    g_scale, g_shift = g_scale / n, g_shift / n
    h_ss, h_sb, h_bb = h_ss / n + RIDGE, h_sb / n, h_bb / n + RIDGE
    if temperature_only:
        return g_scale / h_ss, 0.0, g_scale * g_scale / h_ss
    determinant = h_ss * h_bb - h_sb * h_sb
    step_scale = (h_bb * g_scale - h_sb * g_shift) / determinant
    step_shift = (h_ss * g_shift - h_sb * g_scale) / determinant
    return step_scale, step_shift, g_scale * step_scale + g_shift * step_shift


def fit_platt(pairs, temperature_only=False):
    scale, shift = 1.0, 0.0
    features = [(logit(confidence), 1.0 if correct else 0.0) for confidence, correct in pairs]
    loss = log_loss(features, scale, shift)
    for _ in range(MAX_ITERATIONS):
        step_scale, step_shift, slope = newton_step(features, scale, shift, temperature_only)
        fraction = 1.0
        while fraction > 1e-10:
            trial_scale = max(MIN_SCALE, scale - fraction * step_scale)
            trial_shift = max(-MAX_SHIFT, min(MAX_SHIFT, shift - fraction * step_shift))
            trial = log_loss(features, trial_scale, trial_shift)
            if trial <= loss - 1e-4 * fraction * slope:
                break
            fraction /= 2.0
        else:
            break
        improvement = loss - trial
        scale, shift, loss = trial_scale, trial_shift, trial
        if improvement < 1e-12:
            break
    return {"type": "platt", "scale": scale, "shift": shift, "temperature": 1.0 / scale}


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
