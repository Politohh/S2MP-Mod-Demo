"""Compatibility entry point for the current live-unload regression."""
import runpy
from pathlib import Path
runpy.run_path(str(Path(__file__).with_name("generate_native_unload_test.py")), run_name="__main__")
