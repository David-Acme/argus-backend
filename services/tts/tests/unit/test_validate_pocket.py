import importlib.util
from pathlib import Path


SPEC = importlib.util.spec_from_file_location(
    "pocket_validation_tests", Path(__file__).with_name("validate-pocket-test.py")
)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)
PocketValidationTest = MODULE.PocketValidationTest
