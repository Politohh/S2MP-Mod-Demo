"""Compile the actual GUI value-selection block with controlled dvar inputs."""
from pathlib import Path
root=Path(__file__).resolve().parents[1]
s=(root/'src/demo/demo_gui.cpp').read_text(encoding='utf-8-sig')
block=s.split('const bool draw2d_supported =',1)[1].split('ImGui::BeginDisabled',1)[0]
block='const bool draw2d_supported ='+block
prefix='#include <cassert>\n#include <iostream>\nenum { DVAR_TYPE_BOOL=0, DVAR_TYPE_BOOL_SECURE=10 };\nstruct Dvar { int type; struct { bool enabled; } current; bool decoded; };\nnamespace GameUtil { bool decodeDvarSecureBool(const Dvar* d) { return d->decoded; } }\nbool checked(const Dvar* draw2d) {\n'
suffix='return no_hud;\n}\nint main() {\n Dvar d{DVAR_TYPE_BOOL_SECURE,{true},false};\n assert(checked(&d)); // encoded storage nonzero, real value false: HUD hidden\n d.decoded=true; assert(!checked(&d)); // second click can restore visibility\n d.type=DVAR_TYPE_BOOL; d.current.enabled=false; assert(checked(&d));\n d.current.enabled=true; assert(!checked(&d));\n d.type=2; assert(!checked(&d));\n assert(!checked(nullptr));\n std::cout << "HUD value-selection regression passed\\n";\n}\n'
(root/'obj/test_hud_state.cpp').write_text(prefix+block+suffix,encoding='utf-8')
