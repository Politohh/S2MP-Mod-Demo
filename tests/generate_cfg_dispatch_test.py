"""Exercise the actual CFG dispatcher with native-engine calls mocked."""
from pathlib import Path
import sys

source = Path(sys.argv[1] if len(sys.argv) > 1 else "src/Exec.cpp").read_text(encoding="utf-8-sig")
helpers = source[source.index("namespace {"):source.index("void Exec::init()")]
dispatcher = source[source.index("void Exec::execCmd() {"):]
legacy = "#define LEGACY_CFG_DISPATCH\n" if "bool hasLocalCfgFile(" not in source else ""
prelude = r'''
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>
struct CmdArgs { int nesting{}; int argc[8]{}; const char** argv[8]{}; } args;
struct RawFile { int len{}, compressedLen{}; const char* buffer{}; } raw;
struct Header { RawFile* rawfile{}; };
enum { ASSET_TYPE_RAWFILE };
using Bytef = unsigned char;
using uInt = unsigned int;
struct z_stream { Bytef* next_in{}; uInt avail_in{}; Bytef* next_out{}; uInt avail_out{}; uInt total_out{}; };
constexpr int Z_OK = 0, Z_STREAM_END = 1, Z_FINISH = 4;
int inflateInit(z_stream*) { return -1; }
int inflate(z_stream*, int) { return -1; }
int inflateEnd(z_stream*) { return 0; }
std::string workingDir;
std::vector<std::string> submitted, nativeFiles;
int rawReads = 0;
struct Functions {
    static const char* cwd() { return workingDir.c_str(); }
    static inline const char* (*_Sys_Cwd)() = cwd;
    static void _Cmd_Exec_f() { nativeFiles.emplace_back(args.argv[0][1]); }
    static Header rawfile(int, const char*, int) { ++rawReads; return {&raw}; }
    static inline Header (*_DB_FindXAssetHeader)(int, const char*, int) = rawfile;
};
struct GameUtil {
    static CmdArgs* getCmdArgs() { return &args; }
    static std::string toLower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
        return s;
    }
};
struct Console {
    static void print(const char*) {}
    static void printf(const char*, ...) {}
    static std::vector<std::string> parseCmdToVec(const std::string& s) {
        std::istringstream input(s);
        std::vector<std::string> result;
        std::string token;
        while (input >> std::quoted(token)) result.push_back(token);
        return result;
    }
    static void execCmd(const std::string& s, bool) { submitted.push_back(s); }
};
struct Exec { static void execCmd(); };
'''
tests = r'''
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void writeCfg(const std::string& name, const std::string& text) {
    const auto path = std::filesystem::path(workingDir) / "players2" / name;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}
void run(const char* name) {
    const char* argv[] = {"exec", name};
    args.argc[0] = 2; args.argv[0] = argv;
    submitted.clear(); nativeFiles.clear(); rawReads = 0;
    Exec::execCmd();
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    workingDir = argv[1];
    std::filesystem::create_directories(std::filesystem::path(workingDir) / "players2");
    std::string gameRules;
    for (int i = 0; i < 900; ++i) gameRules += "set scr_rule_" + std::to_string(i) + " 1\n";
    raw = {static_cast<int>(gameRules.size()), 0, gameRules.c_str()};
    try {
        run("default_match.cfg");
        require(nativeFiles.size() == 1, "Built-in CFG must execute through the engine exactly once");
        require(submitted.empty(), "Built-in CFG was replayed into the command buffer");
        require(rawReads == 0, "Mod must not read the built-in rawfile again");

        writeCfg("custom.cfg", "\xEF\xBB\xBF// custom\r\nset cg_fov 90\r\n\r\n");
        run("custom");
        require(nativeFiles.empty(), "Local CFG must not also execute through the native handler");
        require(submitted == std::vector<std::string>{"set cg_fov 90"}, "Local CFG/BOM/CRLF parsing changed");

        writeCfg("child.cfg", "set child 1\n");
        writeCfg("parent.cfg", "set before 1\nexec child\nexec default_match.cfg\nset after 1\n");
        run("parent");
        require(submitted == std::vector<std::string>{"set before 1", "set child 1", "exec default_match.cfg", "set after 1"}, "Nested local/native order changed");
        require(nativeFiles.empty() && rawReads == 0, "Nested built-in command must stay on the engine queue");

        writeCfg("loop.cfg", "exec loop\nset completed 1\n");
        run("loop");
        require(submitted == std::vector<std::string>{"set completed 1"}, "Local recursion guard failed");
        require(g_activeExecFiles.empty(), "Local recursion guard leaked state");

        writeCfg("system_config_mp.cfg", "encrypted profile\n");
        writeCfg("user_config_mp.cfg", "encrypted profile\n");
        writeCfg("controls/preset.cfg", "native preset\n");
        for (const char* name : {"system_config_mp.cfg", "user_config_mp.cfg", "controls/preset.cfg"}) {
            run(name);
            require(nativeFiles.size() == 1 && submitted.empty(), "Native profile/preset was intercepted");
        }

        run("missing.cfg");
        require(nativeFiles.size() == 1 && submitted.empty() && rawReads == 0, "Missing local CFG must go to the engine");
#ifndef LEGACY_CFG_DISPATCH
        require(!hasLocalCfgFile("../outside.cfg"), "Parent path accepted");
        require(!hasLocalCfgFile(workingDir + "/outside.cfg"), "Absolute path accepted");
#endif
        writeCfg("empty.cfg", "");
        run("empty.cfg");
        require(nativeFiles.empty() && submitted.empty(), "Empty local CFG must not fall back to built-in assets");
        std::cout << "CFG dispatch: all regressions passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
'''
Path("obj/test_cfg_dispatch.cpp").write_text(legacy + prelude + helpers + dispatcher + tests, encoding="utf-8")
