import importlib.util
import math
import os
import pathlib
import random
import unittest

HERE = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("calibration", pathlib.Path(os.environ.get("CALIBRATION", HERE / "calibration.py")))
calibration = importlib.util.module_from_spec(spec)
spec.loader.exec_module(calibration)


def synthetic(count, power, seed):
    rng = random.Random(seed)
    pairs = []
    for _ in range(count):
        confidence = 0.5 + 0.5 * rng.random()
        pairs.append((confidence, rng.random() < confidence ** power))
    return pairs


def clustered(count, seed):
    rng = random.Random(seed)
    pairs = []
    for _ in range(count):
        x = 3.5 + 10.0 * rng.random()
        pairs.append((1.0 / (1.0 + math.exp(-x)), rng.random() < 0.75 + 0.1 * (x - 8.5) / 5.0))
    return pairs


class CalibrationTest(unittest.TestCase):
    def test_the_expected_error_of_a_calibrated_source_is_small_and_of_an_overconfident_one_is_large(self):
        honest = synthetic(6000, 1.0, 1)
        overconfident = synthetic(6000, 4.0, 2)
        self.assertLess(calibration.expected_calibration_error(honest), 0.03)
        self.assertGreater(calibration.expected_calibration_error(overconfident), 0.15)

    def test_every_fit_reduces_the_error_on_held_out_data(self):
        train, held = synthetic(6000, 4.0, 3), synthetic(6000, 4.0, 4)
        before = calibration.evaluate({"type": "identity"}, held)["ece"]
        for kind in ("platt", "isotonic"):
            after = calibration.evaluate(calibration.fit(train, kind), held)["ece"]
            self.assertLess(after, before * 0.5, kind)
        self.assertLess(calibration.evaluate(calibration.fit(train, "temperature"), held)["ece"], before)

    def test_temperature_alone_flattens_an_overconfident_source(self):
        model = calibration.fit(synthetic(6000, 4.0, 5), "temperature")
        self.assertGreater(model["temperature"], 1.5)
        self.assertEqual(model["shift"], 0.0)
        self.assertLess(calibration.apply(model, 0.95), 0.95)

    def test_a_compressed_source_is_stretched(self):
        rng = random.Random(6)
        pairs = []
        for _ in range(6000):
            truth = rng.random()
            pairs.append((0.5 + 0.2 * truth, rng.random() < truth))
        model = calibration.fit(pairs, "platt")
        self.assertGreater(calibration.apply(model, 0.69), 0.8)
        self.assertLess(calibration.apply(model, 0.51), 0.2)

    def test_a_source_packed_against_one_converges_to_a_finite_rank_preserving_map(self):
        train, held = clustered(4000, 9), clustered(4000, 10)
        before = calibration.evaluate({"type": "identity"}, held)["ece"]
        for kind in ("temperature", "platt"):
            model = calibration.fit(train, kind)
            self.assertTrue(0.0 < model["scale"] < 5.0, (kind, model))
            self.assertLess(calibration.evaluate(model, held)["ece"], before, kind)
            self.assertLess(calibration.apply(model, 0.98), calibration.apply(model, 0.99999), kind)

    def test_a_source_with_no_signal_keeps_a_positive_scale(self):
        rng = random.Random(11)
        pairs = [(0.5 + 0.5 * rng.random(), rng.random() < 0.5) for _ in range(2000)]
        for kind in ("temperature", "platt"):
            self.assertGreater(calibration.fit(pairs, kind)["scale"], 0.0)

    def test_isotonic_is_monotone_and_stays_inside_zero_and_one(self):
        model = calibration.fit(synthetic(3000, 3.0, 7), "isotonic")
        previous = -1.0
        for step in range(0, 101):
            value = calibration.apply(model, 0.45 + 0.6 * step / 100)
            self.assertGreaterEqual(value, previous - 1e-9)
            self.assertTrue(0.0 <= value <= 1.0)
            previous = value

    def test_a_pure_source_maps_to_one_and_a_pure_noise_source_to_the_base_rate(self):
        sure = [(0.9, True)] * 50 + [(0.6, False)] * 50
        model = calibration.fit(sure, "isotonic")
        self.assertAlmostEqual(calibration.apply(model, 0.9), 1.0)
        self.assertAlmostEqual(calibration.apply(model, 0.6), 0.0)

    def test_an_empty_set_is_the_identity(self):
        self.assertEqual(calibration.apply(calibration.fit([], "platt"), 0.7), 0.7)

    def test_the_reliability_table_counts_every_decision(self):
        pairs = synthetic(1000, 2.0, 8)
        self.assertEqual(sum(row["count"] for row in calibration.reliability(pairs)), 1000)


if __name__ == "__main__":
    unittest.main()
