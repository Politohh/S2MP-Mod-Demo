from pathlib import Path
source = Path("src/demo/engine_probe.hpp").read_text()
start = source.index("    inline void write_image()")
end = source.index("\n    }", start) + 6
harness = Path("tests/engine_image_export.template.cpp").read_text()
Path("obj").mkdir(exist_ok=True)
Path("obj/test_engine_image_export.cpp").write_text(harness.replace("// ACTUAL_EXPORT_FUNCTION", source[start:end]))
