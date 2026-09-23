# Build 20: read-only loaded-code export

Serv supplied s2_mp64_ship.rar. Extracted original:
SHA256 6e785ba25e0133bd3ade87b74f4269116bea93b21d861efe9cdd6b37fa941734
PE timestamp 0x6963A4D9; image size 0x12073200, matching probe 19.
The disk bytes at RVA 0x917ED0 differ from runtime instructions:
disk e524b0e30cc354d3..., runtime push rbp/rbx/rsi/rdi....
A source-built Steamless static unpack attempt did not recognize/unpack it.
No game executable was launched. Original preserved in
F:/Dev/S2MP-engine-analysis-serv. No S2 database opened on encrypted bytes.

New explicit command: demo_engine_image. Copies the on-disk PE into a separate
main/s2mp-build-20-engine.analysis.bin and replaces ONLY eligible non-writable
.text/.rdata/.pdata/_RDATA sections with loaded image bytes. Initialized writable
sections stay from disk; no live .data or heap copied. Bounds checked and SEH
copy protected. PE ImageBase becomes current runtime base for cross references.
This mixed image is ONLY for analysis; do not run/install it as a game executable.
No upload, game-memory mutation, game-file replacement, or automatic export.

This removes the repeated need to request individual code ranges. It can
still contain hook branches and runtime protections; analysis must account
for them. A readable export is not itself a rewind or brightness fix.

Tests compile actual write_image extracted from production into a fixture
harness. Verify copied code/rdata, unchanged writable data, preserved source,
and ASLR base. Policy tests cover supported/unsupported sections, writable
exclusion and overflow-safe range checks. Release x64 build checked before
packaging. Existing working rewind and exposure behaviour unchanged.

Tester: install both build-20 files, load a demo, enter demo_engine_image once.
Send main/s2mp-build-20-engine.analysis.bin (ZIP/RAR is fine) and fresh
main/s2mp_console.log. No repeat A/B test is requested.
