from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path, PureWindowsPath
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[1] / "ci/install_msan_libcxx.py"


def load_module():
    spec = importlib.util.spec_from_file_location("install_msan_libcxx", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class InstallMsanLibcxxTests(unittest.TestCase):
    def setUp(self):
        self.toolchain = load_module()

    def compile_msan_ir(self, source, *extra_flags):
        clangxx = shutil.which("clang++")
        if clangxx is None:
            self.skipTest("Clang C++ is required for MSan IR controls")
        completed = subprocess.run(
            [
                clangxx,
                "--target=x86_64-pc-linux-gnu",
                "-std=c++17",
                "-O1",
                "-fsanitize=memory",
                "-fsanitize-memory-track-origins=2",
                *extra_flags,
                "-S",
                "-emit-llvm",
                "-x",
                "c++",
                "-o",
                "-",
                "-",
            ],
            input=source,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        return completed.stdout

    def function_ir(self, ir, name):
        marker = f"@{name}("
        self.assertIn(marker, ir)
        start = ir.rfind("define ", 0, ir.index(marker))
        end = ir.index("\n}", start) + 2
        return ir[start:end]

    def spill_import_source(self):
        patch = self.toolchain.LIBUNWIND_MSAN_PATCH.read_text(encoding="utf-8")
        dwarf = patch.split("diff --git a/libunwind/src/DwarfInstructions.hpp", 1)[1]
        additions = "\n".join(line[1:] for line in dwarf.splitlines()
                              if line.startswith("+") and not line.startswith("+++"))
        helper = re.search(r"#if defined\(_LIBUNWIND_MSAN_RA_DIAGNOSTIC\)\n"
                           r"template <typename A>.*?\n#endif\n#endif", additions, re.DOTALL).group(0)
        self.assertNotIn("UnwindLevel1.c", patch)
        self.assertIn(", &returnAddressSource, i,", additions)
        self.assertIn(", nullptr, i,", additions)
        eligibility_matches = re.findall(r"R::getArch\(\) == REGISTERS_X86_64 && !cieInfo.isSignalFrame\n"
                                         r"\s*&& cieInfo.returnAddressRegister == 16 && prolog.cfaExpression == 0",
                                         additions)
        self.assertEqual(len(eligibility_matches), 2)
        self.assertEqual(" ".join(eligibility_matches[0].split()), " ".join(eligibility_matches[1].split()))
        eligibility = eligibility_matches[0]
        _, _, _, cases = self.return_address_diagnostic_source()
        return helper, eligibility, cases

    def spill_fixture(self):
        helper, eligibility, cases = self.spill_import_source()
        source = (
            "using uint64_t = __UINT64_TYPE__; using int64_t = __INT64_TYPE__;\n"
            "using int32_t = __INT32_TYPE__; using uint32_t = __UINT32_TYPE__;\n"
            "using pint_t = uint64_t;\n"
            "extern \"C\" void __msan_unpoison(const volatile void *, __SIZE_TYPE__);\n"
            "struct LocalAddressSpace { using pint_t = uint64_t;\n"
            "  uint32_t reads = 0, evaluations = 0;\n"
            "  pint_t getRegister(pint_t source) { ++reads; return *reinterpret_cast<volatile pint_t*>(source); } };\n"
            "struct RemoteAddressSpace { using pint_t = uint64_t;\n"
            "  uint32_t reads = 0, evaluations = 0;\n"
            "  pint_t getRegister(pint_t source) { ++reads; return *reinterpret_cast<volatile pint_t*>(source); } };\n"
            "struct RegisterLocation { uint32_t location; int64_t value; };\n"
            "template <class> struct CFI_Parser { enum { kRegisterInCFA = 2, kRegisterAtExpression = 6 }; };\n"
            "static int32_t requested_arch = 1;\n"
            "struct R { static int32_t getArch() { return requested_arch; } };\n"
            "static constexpr int32_t REGISTERS_X86_64 = 1;\n"
            + helper + "\n"
            "template <class A> pint_t evaluateExpression(pint_t expression, A &a, int32_t, pint_t) {\n"
            "  ++a.evaluations; return expression; }\n"
            "template <class A> pint_t restore(A &addressSpace, int32_t destinationRegister,\n"
            "    pint_t cfa, RegisterLocation savedReg, bool signal, uint32_t ra, int64_t expr) {\n"
            "  int32_t registers = 0; pint_t *sourceAddress = nullptr;\n"
            "  struct { bool isSignalFrame; uint32_t returnAddressRegister; } cieInfo{signal, ra};\n"
            "  struct { int64_t cfaExpression; } prolog{expr};\n"
            "  const bool nativeSpillFrame = " + eligibility + ";\n"
            "  switch(savedReg.location) {\n" + cases + "\n"
            "    default: return *reinterpret_cast<volatile pint_t*>(cfa);\n"
            "  }\n}\n"
            "extern \"C\" uint64_t local_import(volatile uint64_t *source, int32_t destination,\n"
            "    uint32_t rule, int64_t offset, bool signal, uint32_t ra, int64_t expr) {\n"
            "  LocalAddressSpace a; return restore(a, destination, reinterpret_cast<pint_t>(source),\n"
            "                                     {rule, offset}, signal, ra, expr);\n}\n"
            "extern \"C\" uint64_t remote_import(volatile uint64_t *source) {\n"
            "  RemoteAddressSpace a; return restore(a, 6, reinterpret_cast<pint_t>(source), {2, 0}, false, 16, 0);\n}\n"
        )
        return source

    def test_spill_import_is_exact_local_copy_and_preserves_source_origin_and_canaries(self):
        compiler = shutil.which("clang++")
        if compiler is None:
            self.skipTest("Clang is required for finite spill import controls")
        source = "#include <string.h>\n#define _LIBUNWIND_MSAN_RA_DIAGNOSTIC\n" + self.spill_fixture() + (
            "static uint64_t slots[3] = {0xabc1, 0x123456789abcdef0, 0xabc2};\n"
            "static unsigned char source_shadow[24]; static uint32_t source_origin = 6;\n"
            "static int32_t imports = 0, bad_import = 0;\n"
            "static uint64_t owned[3] = {0xdef1, 0x123456789abcdef0, 0xdef2};\n"
            "static unsigned char owned_shadow[24];\n"
            "static const void *expected_local = nullptr;\n"
            "extern \"C\" void __msan_unpoison(const volatile void *p, __SIZE_TYPE__ n) {\n"
            "  ++imports; if (n != 8 || p == slots || p == slots+1 || p == slots+2) bad_import = 1;\n"
            "  if (*static_cast<const volatile uint64_t*>(p) != slots[1]) bad_import = 1;\n}\n"
        ).replace(
            "bad_import = 1;\n}\n",
            "bad_import = 1;\n"
            "  if (expected_local) { if (p != expected_local) bad_import = 1;\n"
            "    else memset(owned_shadow+8, 0, n); }\n}\n",
        ) + (
            "int main() {\n"
            "  memset(source_shadow, 0xa5, sizeof(source_shadow));\n"
            "  for (int32_t reg = 0; reg != 18; ++reg) {\n"
            "    imports = 0;\n"
            "    if (local_import(slots+1, reg, 2, 0, false, 16, 0) != slots[1]) return 1;\n"
            "    const bool eligible = reg == 3 || reg == 6 || (reg >= 12 && reg <= 16);\n"
            "    if (imports != (eligible ? 1 : 0)) return 2;\n"
            "  }\n"
            "  for (int32_t mode = 0; mode != 6; ++mode) {\n"
            "    imports = 0; requested_arch = mode == 0 ? 0 : 1;\n"
            "    const uint32_t rule = mode == 1 ? 6 : mode == 2 ? 3 : 2;\n"
            "    const int64_t offset = mode == 1 ? reinterpret_cast<pint_t>(slots+1) : 0;\n"
            "    if (local_import(slots+1, 6, rule, offset, mode == 3, mode == 4 ? 8 : 16,\n"
            "                     mode == 5 ? 1 : 0) != slots[1] || imports) return 3;\n"
            "  }\n"
            "  imports = 0; if (remote_import(slots+1) != slots[1] || imports) return 4;\n"
            "  LocalAddressSpace a; imports = 0;\n"
            "  if (restore(a, 6, reinterpret_cast<pint_t>(slots+1), {2, 0}, false, 16, 0) != slots[1]\n"
            "      || a.reads != 1 || a.evaluations || imports != 1) return 5;\n"
            "  imports = 0;\n"
            "  if (restore(a, 6, 0, {6, static_cast<int64_t>(reinterpret_cast<pint_t>(slots+1))}, false, 16, 0)\n"
            "      != slots[1] || a.reads != 2 || a.evaluations != 1 || imports) return 6;\n"
            "  for (uint32_t i = 0; i != sizeof(source_shadow); ++i) if (source_shadow[i] != 0xa5) return 7;\n"
            "  memset(owned_shadow, 0xa5, sizeof(owned_shadow)); expected_local = owned+1; imports = 0;\n"
            "  msanImportCFISpill(a, owned[1], 6, true); if (imports != 1) return 8;\n"
            "  for (uint32_t i = 0; i != sizeof(owned_shadow); ++i)\n"
            "    if (owned_shadow[i] != (i >= 8 && i < 16 ? 0 : 0xa5)) return 9;\n"
            "  if (owned[0] != 0xdef1 || owned[1] != slots[1] || owned[2] != 0xdef2) return 10;\n"
            "  return bad_import || source_origin != 6 || slots[0] != 0xabc1\n"
            "      || slots[1] != 0x123456789abcdef0 || slots[2] != 0xabc2;\n}\n"
        )
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "spill-import"
            result = subprocess.run([compiler, "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror",
                                     "-x", "c++", "-o", str(executable), "-"],
                                    input=source, text=True, capture_output=True, check=False)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(executable)], text=True, capture_output=True, check=False)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_spill_import_platform_and_word_width_guard(self):
        compiler = shutil.which("clang++")
        if compiler is None:
            self.skipTest("Clang is required for import guard controls")
        guard, _, _, _ = self.return_address_diagnostic_source()
        guard = re.sub(r'#include [<"][^>"]+[>"]', "INTERFACE_INCLUDED", guard)
        helper, _, _ = self.spill_import_source()
        for target, flags, active in (
            ("x86_64-pc-linux-gnu", ["-fsanitize=memory"], True),
            ("x86_64-pc-linux-gnu", [], False),
            ("aarch64-pc-linux-gnu", ["-fsanitize=memory"], False),
            ("x86_64-pc-linux-gnux32", ["-fsanitize=memory"], False),
            ("x86_64-apple-darwin", [], False),
        ):
            with self.subTest(target=target, flags=flags):
                result = subprocess.run([compiler, f"--target={target}", *flags, "-E", "-P", "-x", "c++", "-"],
                                        input=guard + "\n" + helper, text=True, capture_output=True, check=False)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual("__msan_unpoison(&value, 8)" in result.stdout, active)

    def test_spill_import_retains_address_metadata_and_unsupported_payload_checks(self):
        source = "#define _LIBUNWIND_MSAN_RA_DIAGNOSTIC\n" + self.spill_fixture() + (
            "extern \"C\" bool eligible_branch(volatile uint64_t *p) {\n"
            "  return local_import(p, 6, 2, 0, false, 16, 0) != 0; }\n"
            "extern \"C\" bool unsupported_branch(volatile uint64_t *p) {\n"
            "  return local_import(p, 0, 2, 0, false, 16, 0) != 0; }\n"
            "extern \"C\" bool remote_branch(volatile uint64_t *p) { return remote_import(p) != 0; }\n"
            "extern \"C\" bool expression_branch(volatile uint64_t *p) {\n"
            "  return local_import(p, 6, 6, reinterpret_cast<pint_t>(p), false, 16, 0) != 0; }\n"
            "extern \"C\" bool signal_branch(volatile uint64_t *p) {\n"
            "  return local_import(p, 6, 2, 0, true, 16, 0) != 0; }\n"
            "extern \"C\" bool float_branch(volatile double *p) { return *p != 0; }\n"
            "extern \"C\" bool vector_branch(volatile uint64_t *p) { return (p[0] | p[1]) != 0; }\n"
        )
        ir = self.compile_msan_ir(source, "-fno-sanitize-memory-param-retval")
        local = self.function_ir(ir, "local_import")
        self.assertIn("@__msan_unpoison", local)
        self.assertIn("@__msan_warning_with_origin_noreturn", local)
        self.assertLess(local.index("load volatile i64"), local.index("@__msan_unpoison"))
        self.assertIn("sanitize_memory", ir)
        for name in ("unsupported_branch", "remote_branch", "expression_branch", "signal_branch",
                     "float_branch", "vector_branch"):
            body = self.function_ir(ir, name)
            self.assertIn("@__msan_warning_with_origin_noreturn", body)
            self.assertNotIn("@__msan_unpoison", body)

    def cfa_address_diagnostic_source(self):
        patch = self.toolchain.LIBUNWIND_MSAN_PATCH.read_text(encoding="utf-8")
        dwarf = patch.split("diff --git a/libunwind/src/DwarfInstructions.hpp", 1)[1].split("diff --git ", 1)[0]
        additions = "\n".join(line[1:] for line in dwarf.splitlines() if line.startswith("+") and not line.startswith("+++"))
        diagnostic = re.search(
            r"#if defined\(_LIBUNWIND_MSAN_RA_DIAGNOSTIC\)\n"
            r"          if \(R::getArch\(\).*?\n#endif", additions, re.DOTALL,
        ).group(0)
        _, declarations, _, _ = self.return_address_diagnostic_source()
        declarations = declarations.replace(
            'extern "C" void* stderr;\nextern "C" int fprintf(void*, const char*, ...);\n', "#include <stdio.h>\n",
        ).replace(
            "struct Registers { pint_t ip; pint_t getIP() const { return ip; } };",
            "static int32_t arch = 1; static uint64_t base_value = 0xa001;\n"
            "struct Registers { static int32_t getArch() { return arch; }\n"
            "  pint_t getRegister(uint32_t) const { return base_value; } };\n"
            "static constexpr int32_t REGISTERS_X86_64 = 1;\n"
            "template <typename> struct CFI_Parser { static constexpr uint32_t kRegisterInCFA = 2; };",
        )
        observer = (
            "extern \"C\" int32_t address_poison, data_poison, restore_calls;\n"
            "static uint64_t restore() { ++restore_calls; return saved; }\n"
            "extern \"C\" uint64_t observe_cfa(int32_t i, uint32_t base_reg, uint32_t location, int64_t expr) {\n"
            "  using R = Registers; using A = int; Registers registers;\n"
            "  pint_t pc = saved, fdeStart = 0x2000;\n"
            "  struct { pint_t pcStart, pcEnd; } fdeInfo{0x1000, 0x1100};\n"
            "  struct { uint8_t returnAddressRegister; bool isSignalFrame; } cieInfo{16, false};\n"
            "  struct { uint32_t cfaRegister; int32_t cfaRegisterOffset; int64_t cfaExpression;\n"
            "    RegisterLocation savedRegisters[17]; } prolog{base_reg, 16, expr, {}};\n"
            "  prolog.savedRegisters[i] = {location, -16};\n"
            "  pint_t cfa = reinterpret_cast<pint_t>(&saved) + 16;\n"
            f"{diagnostic}\n"
            "  if (address_poison) return 86;\n"
            "  return restore();\n}\n"
        )
        return declarations, observer

    def test_cfa_diagnostic_keeps_original_load_address_checks(self):
        declarations, observer = self.cfa_address_diagnostic_source()
        declarations = declarations.replace("#include <stdio.h>",
            'extern "C" void* stderr;\nextern "C" int fprintf(void*, const char*, ...);')
        observer = observer.replace("if (address_poison) return 86;\n  return restore();",
                                    "return *reinterpret_cast<volatile uint64_t*>(cfa - 16);")
        observer = observer.replace("int64_t expr) {", "int64_t expr, pint_t *incoming_cfa) {")
        observer = observer.replace("pint_t cfa = reinterpret_cast<pint_t>(&saved) + 16;",
                                    "pint_t cfa = *incoming_cfa;")
        ir = self.compile_msan_ir("#define _LIBUNWIND_MSAN_RA_DIAGNOSTIC\n" + declarations + observer,
                                  "-fno-sanitize-memory-param-retval")
        observed = self.function_ir(ir, "observe_cfa")
        self.assertIn("@__msan_test_shadow", observed)
        self.assertIn("@pread", observed)
        self.assertIn("@__msan_warning_with_origin_noreturn", observed)
        self.assertIn("load volatile i64", observed)
        self.assertNotRegex(observed, r"@__msan_(?:unpoison|poison|set_origin|set_keep_going)")
        self.assertIn("sanitize_memory", ir)

    def test_cfa_diagnostic_matches_latest_eligible_restore_and_preserves_state(self):
        compiler = shutil.which("clang++")
        if compiler is None:
            self.skipTest("Clang is required for compiled CFA provenance controls")
        declarations, observer = self.cfa_address_diagnostic_source()
        source = "#define _LIBUNWIND_MSAN_RA_DIAGNOSTIC\n" + declarations + (
            "#include <string.h>\n"
            "extern \"C\" { int32_t address_poison = 0, data_poison = 0, restore_calls = 0; }\n"
            "static int32_t read_calls = 0, short_read = 0, open_fail = 0;\n"
            "static int32_t sequence = 0, sequence_fail = 0, recording = 0, data_reads = 0;\n"
            "extern \"C\" intptr_t __msan_test_shadow(const void *p, __SIZE_TYPE__ n) {\n"
            "  if (recording && p == &saved) {\n"
            "    if (sequence == 0) sequence = 1; else if (sequence == 3) sequence = 4;\n"
            "    else sequence_fail = 1; }\n"
            "  return n != 8 ? 99 : p == &saved ? (data_poison ? 0 : -1) : (address_poison ? 0 : -1);\n}\n"
            "extern \"C\" uint32_t __msan_get_origin(const void *p) {\n"
            "  if (recording && p == &saved) {\n"
            "    if (sequence == 1) sequence = 2; else if (sequence == 4) sequence = 5;\n"
            "    else sequence_fail = 1; } return p == &saved ? 6 : 1234; }\n"
            "extern \"C\" int32_t open(const char*, int32_t, ...) {\n"
            "  sequence = 0; data_reads = 0; recording = !open_fail; return open_fail ? -1 : 99; }\n"
            "extern \"C\" intptr_t pread(int32_t fd, void *out, __SIZE_TYPE__ n, off_t from) {\n"
            "  ++read_calls; if (fd != 99 || n != 8) return -1;\n"
            "  if (static_cast<uintptr_t>(from) == reinterpret_cast<uintptr_t>(&saved)) {\n"
            "    ++data_reads; if (sequence != 2) sequence_fail = 1; sequence = 3; }\n"
            "  memcpy(out, reinterpret_cast<const void*>(static_cast<uintptr_t>(from)), n);\n"
            "  return read_calls == short_read ? 4 : 8;\n}\n"
            "extern \"C\" int32_t close(int32_t) {\n"
            "  if (data_reads && sequence != 5) sequence_fail = 1; recording = 0; return 0; }\n"
        ) + observer + (
            "int main(int argc, char **argv) {\n"
            "  const char *mode = argc == 2 ? argv[1] : \"clean\";\n"
            "  saved = 0xa001;\n"
            "  if (!strcmp(mode, \"clean\")) return observe_cfa(6, 6, 2, 0) == saved && restore_calls == 1 ? 0 : 1;\n"
            "  data_poison = 1;\n"
            "  const int32_t reg = strncmp(mode, \"reg-\", 4) ? 6\n"
            "      : mode[5] ? (mode[4] - '0') * 10 + mode[5] - '0' : mode[4] - '0';\n"
            "  if (observe_cfa(reg, reg, 2, 0) != saved || restore_calls != 1) return 2;\n"
            "  if (!strcmp(mode, \"update\")) { saved = 0xa002; base_value = saved;\n"
            "    if (observe_cfa(6, 6, 2, 0) != saved || restore_calls != 2) return 3; }\n"
            "  if (!strcmp(mode, \"clear\")) { data_poison = 0; observe_cfa(6, 6, 2, 0); }\n"
            "  if (!strcmp(mode, \"unsupported-rule\")) observe_cfa(6, 6, 3, 0);\n"
            "  if (!strcmp(mode, \"wrong-base\")) base_value = 0xbaad;\n"
            "  if (!strcmp(mode, \"non-native\")) arch = 0;\n"
            "  if (!strcmp(mode, \"open-fail\")) open_fail = 1;\n"
            "  if (!strncmp(mode, \"short-\", 6)) short_read = mode[6] - '0';\n"
            "  read_calls = 0; address_poison = 1; const int32_t calls_before = restore_calls;\n"
            "  uint64_t before = saved;\n"
            "  const int32_t data_before = data_poison;\n"
            "  const uint32_t base_reg = !strcmp(mode, \"ineligible\") ? 7 : reg;\n"
            "  const int64_t expr = !strcmp(mode, \"expression\") ? 1 : 0;\n"
            "  if (observe_cfa(6, base_reg, 2, expr) != 86) return 4;\n"
            "  if (saved != before || address_poison != 1 || data_poison != data_before\n"
            "      || restore_calls != calls_before || sequence_fail) return 5;\n"
            "  if (observe_cfa(6, base_reg, 2, expr) != 86) return 6;\n"
            "  return saved != before || address_poison != 1 || data_poison != data_before\n"
            "      || restore_calls != calls_before || sequence_fail;\n}\n"
        )
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "cfa-provenance"
            compiled = subprocess.run(
                [compiler, "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror", "-x", "c++",
                 "-o", str(executable), "-"], input=source, text=True, capture_output=True, check=False,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            for mode in ("clean", "match", "update", "clear", "unsupported-rule", "wrong-base",
                         "ineligible", "expression", "non-native", "open-fail",
                         "short-1", "short-2", "short-3", "short-4", "short-5",
                         "reg-3", "reg-12", "reg-13", "reg-14", "reg-15", "reg-16"):
                with self.subTest(mode=mode):
                    result = subprocess.run([str(executable), mode], text=True, capture_output=True, check=False)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    if mode in ("clean", "non-native"):
                        self.assertEqual(result.stderr, "")
                        continue
                    self.assertEqual(len(result.stderr.splitlines()), 2)
                    self.assertLess(len(result.stderr), 2048)
                    self.assertIn("address_shadow=0 address_origin=1234", result.stderr)
                    self.assertIn("address_shadow_after=0 address_origin_after=1234", result.stderr)
                    self.assertIn("valid=0" if mode in ("clear", "unsupported-rule", "ineligible", "expression")
                                  else "valid=1", result.stderr)
                    self.assertIn("base_matches=1" if mode.startswith("reg-") or mode in ("match", "update", "short-1", "short-3",
                                                              "short-4", "short-5") else "base_matches=0",
                                  result.stderr)
                    if mode == "update":
                        self.assertIn("pc=0xa002", result.stderr.splitlines()[0])
                        self.assertIn("value_bits=0xa002", result.stderr.splitlines()[0])
                    if mode.startswith("short-"):
                        field = {"1": "cfa", "2": "base", "3": "source", "4": "rule", "5": "value"}[mode[6]]
                        self.assertIn(f"{field}_read=4", result.stderr.splitlines()[1])
                        self.assertIn(f"{field}_bits=0x0", result.stderr.splitlines()[1])
                    if mode == "open-fail":
                        self.assertIn("source_read=-1 source_bits=0x0", result.stderr)
                        self.assertIn("value_read=-1 value_bits=0x0", result.stderr)

    def return_address_diagnostic_source(self):
        patch = self.toolchain.LIBUNWIND_MSAN_PATCH.read_text(encoding="utf-8")
        dwarf_patch = patch.split("diff --git a/libunwind/src/DwarfInstructions.hpp", 1)[1].split("diff --git ", 1)[0]
        additions = "\n".join(
            line[1:] for line in dwarf_patch.splitlines()
            if line.startswith("+") and not line.startswith("+++")
        )
        guard = re.search(
            r"#if defined\(__linux__\).*?\n#endif\n#endif", additions, re.DOTALL
        ).group(0)
        diagnostic = re.search(
            r"#if defined\(_LIBUNWIND_MSAN_RA_DIAGNOSTIC\)\n"
            r"      // Observe.*?\n#endif", additions, re.DOTALL
        ).group(0)
        memory_cases = "\n".join(
            re.search(rf"  case CFI_Parser<A>::{rule}: \{{.*?\n  \}}", additions, re.DOTALL).group(0)
            for rule in ("kRegisterInCFA", "kRegisterAtExpression")
        )
        declarations = (
            "using uint64_t = __UINT64_TYPE__; using int64_t = __INT64_TYPE__;\n"
            "using uint32_t = __UINT32_TYPE__; using int32_t = __INT32_TYPE__;\n"
            "using uint8_t = __UINT8_TYPE__; using intptr_t = __INTPTR_TYPE__;\n"
            "using uintptr_t = __UINTPTR_TYPE__; using off_t = int64_t;\n"
            "#define PRIx64 __UINT64_FMTx__\n#define PRIu64 __UINT64_FMTu__\n"
            "#define PRId64 __INT64_FMTd__\n#define PRIu32 __UINT32_FMTu__\n"
            "#define PRId32 __INT32_FMTd__\n#define PRIu8 __UINT8_FMTu__\n"
            "#define INT64_C(value) static_cast<int64_t>(value)\n"
            "#define UINT32_C(value) static_cast<uint32_t>(value)\n"
            "#define UINT64_C(value) static_cast<uint64_t>(value)\n"
            "#define O_RDONLY 0\n#define O_CLOEXEC 1\n"
            "extern \"C\" void* stderr;\n"
            "extern \"C\" int fprintf(void*, const char*, ...);\n"
            "extern \"C\" int32_t open(const char*, int32_t, ...);\n"
            "extern \"C\" intptr_t pread(int32_t, void*, __SIZE_TYPE__, off_t);\n"
            "extern \"C\" int32_t close(int32_t);\n"
            "extern \"C\" intptr_t __msan_test_shadow(const void*, __SIZE_TYPE__);\n"
            "extern \"C\" uint32_t __msan_get_origin(const void*);\n"
            "using pint_t = uint64_t;\n"
            "struct RegisterLocation { uint32_t location; int64_t value; };\n"
            "struct Registers { pint_t ip; pint_t getIP() const { return ip; } };\n"
            "static uint64_t saved = 0xabcdef;\n"
        )
        observer = (
            "extern \"C\" uint64_t observe(uint64_t incoming, bool has_source) {\n"
            "  pint_t pc = 0x1000, fdeStart = 0x2000, cfa = 0x3000;\n"
            "  struct { pint_t pcStart, pcEnd; } fdeInfo{0x1000, 0x1100};\n"
            "  struct { uint8_t returnAddressRegister; } cieInfo{16};\n"
            "  struct { uint32_t cfaRegister; int32_t cfaRegisterOffset;\n"
            "    int64_t cfaExpression; RegisterLocation savedRegisters[17];\n"
            "  } prolog{7, 16, 0, {}};\n"
            "  prolog.savedRegisters[16] = {2, -8};\n"
            "  pint_t returnAddress = incoming;\n"
            "  pint_t returnAddressSource = has_source ? reinterpret_cast<pint_t>(&saved) : 0;\n"
            "  Registers registers{incoming};\n"
            f"{diagnostic}\n"
            "  if (returnAddress == 0) return 7;\n"
            "  return registers.getIP();\n"
            "}\n"
        )
        return guard, declarations, observer, memory_cases

    def test_return_address_diagnostic_requires_linux_x86_64_msan(self):
        compiler = shutil.which("clang++")
        if compiler is None:
            self.skipTest("Clang is required for diagnostic activation controls")
        guard, _, _, _ = self.return_address_diagnostic_source()
        guard = re.sub(r'#include [<"][^>"]+[>"]', "MSAN_INTERFACE_INCLUDED", guard)
        source = guard + (
            "\n#if defined(_LIBUNWIND_MSAN_RA_DIAGNOSTIC)\nRA_DIAGNOSTIC_ACTIVE\n#endif\n"
        )
        for target, flags, active in (
            ("x86_64-pc-linux-gnu", ["-fsanitize=memory"], True),
            ("x86_64-pc-linux-gnu", [], False),
            ("aarch64-pc-linux-gnu", ["-fsanitize=memory"], False),
            ("x86_64-apple-darwin", [], False),
        ):
            with self.subTest(target=target, flags=flags):
                completed = subprocess.run(
                    [compiler, "-E", "-P", f"--target={target}", *flags, "-x", "c++", "-"],
                    input=source, text=True, capture_output=True, check=False,
                )
                self.assertEqual(completed.returncode, 0, completed.stderr)
                self.assertEqual("RA_DIAGNOSTIC_ACTIVE" in completed.stdout, active)
                self.assertEqual("MSAN_INTERFACE_INCLUDED" in completed.stdout, active)

    def test_return_address_observation_preserves_state_and_captures_each_memory_source_once(self):
        compiler = shutil.which("clang++")
        if compiler is None:
            self.skipTest("Clang is required for compiled diagnostic controls")
        _, declarations, observer, memory_cases = self.return_address_diagnostic_source()
        declarations = declarations.replace(
            'extern "C" void* stderr;\nextern "C" int fprintf(void*, const char*, ...);\n',
            "#include <stdio.h>\n",
        )
        source = "#define _LIBUNWIND_MSAN_RA_DIAGNOSTIC\n" + declarations + (
            "static int32_t poisoned = 1, shadow_reads = 0, origin_reads = 0;\n"
            "static int32_t read_mode = 0, kernel_reads = 0;\n"
            "extern \"C\" intptr_t __msan_test_shadow(const void*, __SIZE_TYPE__ size) {\n"
            "  ++shadow_reads;\n"
            "  if (read_mode == 3 && shadow_reads == 1) return -1;\n"
            "  return size == 8 ? (poisoned ? 3 : -1) : 99;\n"
            "}\n"
            "extern \"C\" uint32_t __msan_get_origin(const void*) {\n"
            "  ++origin_reads; return 1234;\n"
            "}\n"
            "extern \"C\" int32_t open(const char*, int32_t, ...) { return read_mode == 2 ? -1 : 99; }\n"
            "extern \"C\" intptr_t pread(int32_t fd, void* output, __SIZE_TYPE__ size, off_t) {\n"
            "  ++kernel_reads;\n"
            "  if (fd != 99 || size != 8) return -1;\n"
            "  *static_cast<uint64_t*>(output) = 0x5dcafe;\n"
            "  return read_mode == 1 ? 4 : 8;\n"
            "}\n"
            "extern \"C\" int32_t close(int32_t) { return 0; }\n"
            "template<class A> struct CFI_Parser {\n"
            "  enum { kRegisterInCFA = 2, kRegisterAtExpression = 6 };\n"
            "};\n"
            "struct AddressSpace { uint32_t reads = 0, evaluations = 0; pint_t last = 0;\n"
            "  pint_t getRegister(pint_t source) { ++reads; last = source; return saved; }\n"
            "};\n"
            "pint_t evaluateExpression(pint_t expr, AddressSpace& addressSpace, int32_t, pint_t) {\n"
            "  ++addressSpace.evaluations; return expr + 0x4000;\n"
            "}\n"
            "template<class A> pint_t restore(A& addressSpace, int32_t registers, pint_t cfa,\n"
            "    RegisterLocation savedReg, pint_t* sourceAddress) {\n"
            "  int32_t destinationRegister = -1; bool nativeSpillFrame = false;\n"
            "  auto msanImportCFISpill = [](A&, pint_t&, int32_t, bool) {};\n"
            "  switch (savedReg.location) {\n"
            f"{memory_cases}\n"
            "    default: return 0;\n"
            "  }\n"
            "}\n"
        ) + observer + (
            "int main(int argc, char**) {\n"
            "  poisoned = argc == 2 ? 0 : 1;\n"
            "  read_mode = argc == 3 ? 1 : (argc == 5 ? 2 : (argc == 6 ? 3 : 0));\n"
            "  const bool has_source = argc != 4;\n"
            "  if (observe(saved, has_source) != saved || saved != 0xabcdef) return 1;\n"
            "  if (shadow_reads != (poisoned ? (has_source ? 4 : 2) : 2)) return 2;\n"
            "  if (origin_reads != (poisoned ? (has_source ? 4 : 2) : 0)) return 10;\n"
            "  if (kernel_reads != (poisoned && read_mode != 2 ? 1 : 0)) return 11;\n"
            "  AddressSpace addressSpace; pint_t sourceAddress = 0;\n"
            "  if (restore(addressSpace, 0, 0x3000, {2, -8}, &sourceAddress) != saved) return 3;\n"
            "  if (sourceAddress != 0x2ff8 || addressSpace.last != sourceAddress) return 4;\n"
            "  if (restore(addressSpace, 0, 0x3000, {6, 32}, &sourceAddress) != saved) return 5;\n"
            "  if (sourceAddress != 0x4020 || addressSpace.last != sourceAddress) return 6;\n"
            "  if (addressSpace.reads != 2 || addressSpace.evaluations != 1) return 8;\n"
            "  if (saved != 0xabcdef || poisoned != (argc == 2 ? 0 : 1)) return 9;\n"
            "  return 0;\n"
            "}\n"
        )
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "diagnostic-observation"
            compiled = subprocess.run(
                [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O1",
                 "-x", "c++", "-", "-o", str(executable)],
                input=source, text=True, capture_output=True, check=False,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            poisoned = subprocess.run([str(executable)], text=True, capture_output=True, check=False)
            clean = subprocess.run([str(executable), "clean"], text=True, capture_output=True, check=False)
            short = subprocess.run([str(executable), "short", "read"], text=True, capture_output=True, check=False)
            no_source = subprocess.run(
                [str(executable), "no", "source", "slot"], text=True, capture_output=True, check=False,
            )
            no_memory = subprocess.run(
                [str(executable), "cannot", "open", "process", "memory"],
                text=True, capture_output=True, check=False,
            )
            poisoned_copy = subprocess.run(
                [str(executable), "clean", "local", "poisoned", "copied", "ip"],
                text=True, capture_output=True, check=False,
            )
        self.assertEqual(poisoned.returncode, 0, poisoned.stderr)
        self.assertEqual(clean.returncode, 0, clean.stderr)
        self.assertEqual(clean.stdout + clean.stderr, "")
        self.assertEqual(poisoned.stdout, "")
        self.assertEqual(len(poisoned.stderr.splitlines()), 1)
        self.assertLess(len(poisoned.stderr), 1024)
        for field in ("pc=0x1000", "fde=0x2000", "range=0x1000-0x1100", "cfa=0x3000",
                      "cfa_reg=7", "cfa_offset=16", "ra_reg=16", "rule=2", "rule_value=-8",
                      "width=8", "source_shadow=3", "source_origin=1234", "return_shadow=3",
                      "source_shadow_after=3", "source_origin_after=1234",
                      "return_origin=1234", "ip_shadow=3", "ip_origin=1234",
                      "bits_read=8", "diagnostic_bits=0x5dcafe"):
            self.assertIn(field, poisoned.stderr)
        self.assertNotIn("abcdef", poisoned.stderr)
        for failed_read, count in ((short, 4), (no_memory, -1)):
            self.assertEqual(failed_read.returncode, 0, failed_read.stderr)
            self.assertIn(f"bits_read={count} diagnostic_bits=0x0", failed_read.stderr)
            self.assertIn("source_shadow_after=3 source_origin_after=1234", failed_read.stderr)
            self.assertEqual(len(failed_read.stderr.splitlines()), 1)
        self.assertEqual(no_source.returncode, 0, no_source.stderr)
        self.assertIn("source=0x0", no_source.stderr)
        self.assertIn("source_shadow=-2 source_origin=0", no_source.stderr)
        self.assertIn("source_shadow_after=-2 source_origin_after=0", no_source.stderr)
        self.assertIn("bits_read=8 diagnostic_bits=0x5dcafe", no_source.stderr)
        with self.subTest("clean local return address and poisoned copied IP"):
            self.assertEqual(poisoned_copy.returncode, 0, poisoned_copy.stderr)
            self.assertEqual(poisoned_copy.stdout, "")
            self.assertEqual(len(poisoned_copy.stderr.splitlines()), 1)
            self.assertLess(len(poisoned_copy.stderr), 1024)
            self.assertIn("return_shadow=-1", poisoned_copy.stderr)
            self.assertIn("ip_shadow=3", poisoned_copy.stderr)
            self.assertIn("source_shadow_after=3 source_origin_after=1234", poisoned_copy.stderr)

    def test_return_address_diagnostic_keeps_msan_shadow_and_branch_checks(self):
        _, declarations, observer, _ = self.return_address_diagnostic_source()
        ir = self.compile_msan_ir(
            "#define _LIBUNWIND_MSAN_RA_DIAGNOSTIC\n" + declarations + observer,
            "-fno-sanitize-memory-param-retval",
        )
        observed = self.function_ir(ir, "observe")
        self.assertIn("@__msan_test_shadow", observed)
        self.assertIn("@__msan_get_origin", observed)
        self.assertIn("@fprintf", observed)
        self.assertIn("@__msan_warning_with_origin_noreturn", observed)
        self.assertRegex(observed, r"store i64 %[^,]+, ptr @__msan_retval_tls")
        self.assertNotRegex(observed, r"@__msan_(?:unpoison|poison|set_origin|set_keep_going)")
        self.assertIn("sanitize_memory", ir)

    def test_return_address_provenance_does_not_accept_positive_probe_failure(self):
        diagnostic = (
            "[libunwind-msan-ra] ra_reg=16 rule=2 source_shadow=3 bits_read=8\n"
            "==6318==WARNING: MemorySanitizer: use-of-uninitialized-value\n"
        )
        message = self.assert_positive_probe_rejected(
            subprocess.CompletedProcess([], 86, "", diagnostic)
        )
        self.assertIn("return code: 86", message)
        self.assertIn("[libunwind-msan-ra]", message)
        self.assertIn("emitted a sanitizer diagnostic", message)

    def prepare_verification_tree(self, root):
        prefix = root / "prefix"
        include_dir = prefix / "include/c++/v1"
        library_dir = prefix / "lib"
        build = root / "build"
        include_dir.mkdir(parents=True)
        library_dir.mkdir(parents=True)
        build.mkdir()
        (library_dir / "libc++.so").write_bytes(b"test libc++ runtime")
        negative_probe = build / "msan-uninitialized-read"
        negative_probe.write_bytes(b"test executable")
        symbolizer = root / "llvm-symbolizer-18"
        symbolizer.write_bytes(b"test symbolizer")
        symbolizer.chmod(0o755)
        return prefix, library_dir, build, negative_probe, symbolizer

    def diagnostic_processes(self, negative_run, *, symbolized=None):
        processes = [
            subprocess.CompletedProcess([], 0, "", ""),
            subprocess.CompletedProcess([], 0, "", ""),
            subprocess.CompletedProcess([], 0, "", ""),
            subprocess.CompletedProcess([], 0, "clang version 18.1.3\n", ""),
            subprocess.CompletedProcess(
                [], 0, "libc++.so => /tmp/prefix/lib/libc++.so (0x1234)\n", ""
            ),
            subprocess.CompletedProcess([], 0, "llvm-symbolizer\n", ""),
            negative_run,
        ]
        if symbolized is not None:
            processes.extend(symbolized if isinstance(symbolized, list) else [symbolized])
        return processes

    def negative_report(self, *, pid="", read_site="msanNegativeHeapRead", allocation_site="msanNegativeHeapRead"):
        return (
            f"{pid}WARNING: MemorySanitizer: use-of-uninitialized-value\n"
            f"    #0 0x1234 in {read_site} /tmp/probe.cpp:8:9\n"
            "    #1 0x2345 in main /tmp/probe.cpp:17:21\n\n"
            "  Uninitialized value was created by a heap allocation\n"
            "    #0 0x3456 in malloc /tmp/msan_interceptors.cpp:123:3\n"
            f"    #1 0x4567 in {allocation_site} /tmp/probe.cpp:3:37\n\n"
            "SUMMARY: MemorySanitizer: use-of-uninitialized-value /tmp/probe.cpp:8:9 in msanNegativeHeapRead\n"
        )

    def assert_negative_probe_rejected(self, negative_run):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix, _, build, _, symbolizer = self.prepare_verification_tree(root)
            with (
                mock.patch.object(
                    self.toolchain.subprocess,
                    "run",
                    side_effect=self.diagnostic_processes(negative_run),
                ),
                mock.patch.object(self.toolchain, "resolve_msan_symbolizer", return_value=str(symbolizer)),
            ):
                with self.assertRaisesRegex(self.toolchain.ToolchainError, "did not diagnose") as raised:
                    self.toolchain.verify_install(prefix, "clang++", build)
        return str(raised.exception)

    def assert_positive_probe_rejected(self, positive_run):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix, _, build, _, _ = self.prepare_verification_tree(root)
            with mock.patch.object(
                self.toolchain.subprocess,
                "run",
                side_effect=[subprocess.CompletedProcess([], 0, "", ""), positive_run],
            ) as run:
                with self.assertRaises(self.toolchain.ToolchainError) as raised:
                    self.toolchain.verify_install(prefix, "clang++", build)

        self.assertEqual(run.call_count, 2)
        return str(raised.exception)

    def test_resolve_tool_preserves_clang_driver_aliases(self):
        with (
            mock.patch.object(
                self.toolchain.shutil,
                "which",
                side_effect=["/usr/bin/clang-18", "/usr/bin/clang++-18"],
            ),
            mock.patch.object(
                self.toolchain.Path,
                "resolve",
                return_value=Path("/usr/lib/llvm-18/bin/clang"),
            ) as resolve,
        ):
            clang = self.toolchain.resolve_tool("clang-18", "Clang C")
            clangxx = self.toolchain.resolve_tool("clang++-18", "Clang C++")

        self.assertEqual(clang, self.toolchain.os.path.abspath("/usr/bin/clang-18"))
        self.assertEqual(clangxx, self.toolchain.os.path.abspath("/usr/bin/clang++-18"))
        resolve.assert_not_called()

    def test_runtime_build_is_pinned_and_instrumented(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            command = self.toolchain.configure_command(
                root / "llvm-project",
                root / "build",
                root / "prefix",
                "/usr/bin/clang",
                "/usr/bin/clang++",
            )
        self.assertEqual(self.toolchain.LLVM_TAG, "llvmorg-18.1.3")
        self.assertEqual(self.toolchain.LLVM_COMMIT, "c13b7485b87909fcf739f62cfa382b55407433c0")
        self.assertIn("-DLLVM_USE_SANITIZER=MemoryWithOrigins", command)
        self.assertIn("-DLLVM_ENABLE_RUNTIMES=libcxx;libcxxabi;libunwind", command)
        self.assertIn("-DLIBCXX_USE_COMPILER_RT=ON", command)
        self.assertFalse(any("LIBUNWIND_ADDITIONAL_COMPILE_FLAGS" in arg for arg in command))
        self.assertFalse(any(argument.startswith("-DCMAKE_CXX_FLAGS=") for argument in command))

    def test_libunwind_compile_database_scopes_private_policy_to_objects_and_consumers(self):
        cmake = shutil.which("cmake")
        ninja = shutil.which("ninja")
        clang = shutil.which("clang")
        clangxx = shutil.which("clang++")
        if not all((cmake, ninja, clang, clangxx)):
            self.skipTest("CMake, Ninja, and Clang are required for compile-command scope coverage")

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source"
            build = root / "build"
            source.mkdir()
            sources = {
                "unwind_shared.cpp": "int unwind_shared_cpp() { return 0; }\n",
                "unwind_shared.c": "int unwind_shared_c(void) { return 0; }\n",
                "unwind_shared.S": ".text\n.globl unwind_shared_asm\nunwind_shared_asm:\n ret\n",
                "unwind_static.cpp": "int unwind_static_cpp() { return 0; }\n",
                "unwind_static.c": "int unwind_static_c(void) { return 0; }\n",
                "unwind_static.S": ".text\n.globl unwind_static_asm\nunwind_static_asm:\n ret\n",
                "libcxx.cpp": "int libcxx_runtime() { return 0; }\n",
                "libcxxabi.cpp": "int libcxxabi_runtime() { return 0; }\n",
                "application.cpp": "int main() { return 0; }\n",
                "positive_probe.cpp": "int main() { return 0; }\n",
                "negative_probe.cpp": "int main() { return 0; }\n",
            }
            for name, contents in sources.items():
                (source / name).write_text(contents, encoding="utf-8")
            (source / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.25)\n"
                "project(msan_flag_scope LANGUAGES C CXX ASM)\n"
                "set(CMAKE_SYSTEM_NAME \"${TEST_SYSTEM_NAME}\")\n"
                "set(CMAKE_SYSTEM_PROCESSOR \"${TEST_PROCESSOR}\")\n"
                "if(LLVM_USE_SANITIZER MATCHES \"^Memory(WithOrigins)?$\")\n"
                "  add_compile_options(-fsanitize=memory)\n"
                "endif()\n"
                "if(LLVM_USE_SANITIZER STREQUAL \"MemoryWithOrigins\")\n"
                "  add_compile_options(-fsanitize-memory-track-origins=2)\n"
                "endif()\n"
                "add_library(unwind_shared_objects OBJECT unwind_shared.cpp unwind_shared.c unwind_shared.S)\n"
                "target_compile_options(unwind_shared_objects PUBLIC \"${LIBUNWIND_ADDITIONAL_COMPILE_FLAGS}\")\n"
                "add_library(unwind_static_objects OBJECT unwind_static.cpp unwind_static.c unwind_static.S)\n"
                "target_compile_options(unwind_static_objects PUBLIC \"${LIBUNWIND_ADDITIONAL_COMPILE_FLAGS}\")\n"
                "if (CMAKE_SYSTEM_NAME STREQUAL \"Linux\"\n"
                "    AND CMAKE_SYSTEM_PROCESSOR MATCHES \"^(x86_64|AMD64)$\"\n"
                "    AND LLVM_USE_SANITIZER MATCHES \"^Memory(WithOrigins)?$\")\n"
                "  target_compile_options(unwind_shared_objects PRIVATE\n"
                "    \"$<$<COMPILE_LANGUAGE:CXX>:-fno-sanitize-memory-param-retval>\")\n"
                "  target_compile_options(unwind_static_objects PRIVATE\n"
                "    \"$<$<COMPILE_LANGUAGE:CXX>:-fno-sanitize-memory-param-retval>\")\n"
                "endif()\n"
                "add_library(unwind_shared SHARED)\n"
                "target_link_libraries(unwind_shared PUBLIC unwind_shared_objects)\n"
                "add_library(unwind_static STATIC)\n"
                "target_link_libraries(unwind_static PUBLIC unwind_static_objects)\n"
                "add_library(libcxx_runtime OBJECT libcxx.cpp)\n"
                "target_link_libraries(libcxx_runtime PRIVATE unwind_shared)\n"
                "add_library(libcxxabi_runtime OBJECT libcxxabi.cpp)\n"
                "target_link_libraries(libcxxabi_runtime PRIVATE unwind_static)\n"
                "add_executable(application application.cpp)\n"
                "target_link_libraries(application PRIVATE libcxx_runtime libcxxabi_runtime)\n"
                "add_executable(positive_probe positive_probe.cpp)\n"
                "add_executable(negative_probe negative_probe.cpp)\n",
                encoding="utf-8",
            )
            def configure(build_name, system_name, processor, sanitizer):
                build = root / build_name
                completed = subprocess.run(
                    [
                        cmake,
                        "-G",
                        "Ninja",
                        "-S",
                        str(source),
                        "-B",
                        str(build),
                        f"-DCMAKE_C_COMPILER={clang}",
                        f"-DCMAKE_CXX_COMPILER={clangxx}",
                        f"-DCMAKE_ASM_COMPILER={clang}",
                        "-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY",
                        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
                        f"-DTEST_SYSTEM_NAME={system_name}",
                        f"-DTEST_PROCESSOR={processor}",
                        f"-DLLVM_USE_SANITIZER={sanitizer}",
                    ],
                    text=True,
                    capture_output=True,
                    check=False,
                )
                self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
                return json.loads((build / "compile_commands.json").read_text(encoding="utf-8"))

            compile_commands = configure("build-msan", "Linux", "x86_64", "MemoryWithOrigins")
            normal_commands = configure("build-no-msan", "Linux", "x86_64", "none")
            platform_commands = configure("build-non-linux", "Darwin", "x86_64", "MemoryWithOrigins")
            architecture_commands = configure("build-non-x86", "Linux", "aarch64", "MemoryWithOrigins")

        commands_by_source = {
            Path(entry["file"]).name: entry.get("command", " ".join(entry.get("arguments", [])))
            for entry in compile_commands
        }
        self.assertEqual(set(sources), set(commands_by_source))
        for target_name in ("unwind_shared", "unwind_static"):
            self.assertIn("-fno-sanitize-memory-param-retval", commands_by_source[f"{target_name}.cpp"])
            for suffix in (".c", ".S"):
                self.assertNotIn(
                    "-fno-sanitize-memory-param-retval",
                    commands_by_source[f"{target_name}{suffix}"],
                )
        for source_name, command in commands_by_source.items():
            self.assertIn("-fsanitize=memory", command, source_name)
            self.assertIn("-fsanitize-memory-track-origins=2", command, source_name)
            if not source_name.startswith("unwind_") or not source_name.endswith(".cpp"):
                self.assertNotIn("-fno-sanitize-memory-param-retval", command, source_name)
        for commands in (normal_commands, platform_commands, architecture_commands):
            self.assertFalse(
                any("-fno-sanitize-memory-param-retval" in entry.get("command", "") for entry in commands)
            )

    def test_msan_param_retval_opt_out_preserves_shadow_and_use_checks(self):
        source = (
            "using word = unsigned long;\n"
            "extern \"C\" word transport() { volatile word saved; return saved; }\n"
            "extern \"C\" void side_effect();\n"
            "extern \"C\" void branch_use() {\n"
            "  volatile int value;\n"
            "  if (value) side_effect();\n"
            "}\n"
            "extern \"C\" int address_use() {\n"
            "  volatile word address;\n"
            "  return *reinterpret_cast<volatile int*>(address);\n"
            "}\n"
        )
        ir = self.compile_msan_ir(source, "-fno-sanitize-memory-param-retval")
        transport = self.function_ir(ir, "transport")
        branch_use = self.function_ir(ir, "branch_use")
        address_use = self.function_ir(ir, "address_use")
        self.assertIn("@__msan_track_origins = weak_odr constant i32 2", ir)
        self.assertIn("sanitize_memory", ir)
        self.assertRegex(transport, r"store i64 %[^,]+, ptr @__msan_retval_tls")
        self.assertRegex(transport, r"store i32 %[^,]+, ptr @__msan_retval_origin_tls")
        self.assertNotIn("call void @__msan_warning_with_origin_noreturn", transport)
        self.assertIn("call void @__msan_warning_with_origin_noreturn", branch_use)
        self.assertIn("call void @side_effect()", branch_use)
        self.assertIn("call void @__msan_warning_with_origin_noreturn", address_use)
        self.assertLess(
            address_use.index("call void @__msan_warning_with_origin_noreturn"),
            address_use.index("load volatile i32, ptr"),
        )

    def test_default_msan_application_keeps_branch_and_return_checks(self):
        source = (
            "extern \"C\" void side_effect();\n"
            "extern \"C\" void application_call() {\n"
            "  volatile int value;\n"
            "  if (value) side_effect();\n"
            "}\n"
            "extern \"C\" int application_return() {\n"
            "  volatile int value;\n"
            "  return value;\n"
            "}\n"
        )
        ir = self.compile_msan_ir(source)
        application_call = self.function_ir(ir, "application_call")
        application_return = self.function_ir(ir, "application_return")
        self.assertIn("sanitize_memory", ir)
        self.assertIn("call void @__msan_warning_with_origin_noreturn", application_call)
        self.assertIn("call void @side_effect()", application_call)
        self.assertIn("load volatile i32, ptr", application_return)
        self.assertIn("call void @__msan_warning_with_origin_noreturn", application_return)

    def test_libunwind_patch_is_pinned_and_limited_to_linux_x86_64_msan(self):
        patch = self.toolchain.LIBUNWIND_MSAN_PATCH.read_text(encoding="utf-8")
        self.assertEqual(
            self.toolchain.sha256_file(self.toolchain.LIBUNWIND_MSAN_PATCH),
            self.toolchain.LIBUNWIND_MSAN_PATCH_SHA256,
        )
        self.assertIn('CMAKE_SYSTEM_NAME STREQUAL "Linux"', patch)
        self.assertIn('CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$"', patch)
        self.assertIn('LLVM_USE_SANITIZER MATCHES "^Memory(WithOrigins)?$"', patch)
        self.assertIn(
            "set_property(SOURCE UnwindRegistersSave.S APPEND PROPERTY COMPILE_DEFINITIONS",
            patch,
        )
        self.assertIn("target_compile_options(unwind_shared_objects PRIVATE", patch)
        self.assertIn("target_compile_options(unwind_static_objects PRIVATE", patch)
        self.assertIn(
            '"$<$<COMPILE_LANGUAGE:CXX>:-fno-sanitize-memory-param-retval>"',
            patch,
        )
        self.assertIn("movq  $136, %rsi", patch)
        self.assertIn("call  __msan_unpoison", patch)

    def test_msan_feature_selects_intercepted_loader_fallback(self):
        patch = self.toolchain.LIBUNWIND_MSAN_PATCH.read_text(encoding="utf-8")
        feature_guard_lines = (
            "#if defined(__has_feature)",
            "#if __has_feature(memory_sanitizer)",
            "#define _LIBUNWIND_MSAN_USE_DL_ITERATE_PHDR",
            "#endif",
            "#endif",
        )
        feature_guard_patch = "\n".join(f"+{line}" for line in feature_guard_lines)
        feature_guard = "\n".join(feature_guard_lines)
        self.assertIn(feature_guard_patch, patch)
        fast_path_guard = "#if !defined(_LIBUNWIND_MSAN_USE_DL_ITERATE_PHDR)"
        self.assertIn(f"+{fast_path_guard}", patch)
        feature_guard_cleanup = "#undef _LIBUNWIND_MSAN_USE_DL_ITERATE_PHDR"
        self.assertIn(f"+{feature_guard_cleanup}", patch)
        self.assertIn("int found = dl_iterate_phdr(findUnwindSectionsByPhdr, &cb_data);", patch)

        compiler = self.toolchain.shutil.which("clang++")
        if compiler is None:
            self.skipTest("clang++ is required to preprocess the libunwind feature guard")

        source = (
            f"{feature_guard}\n"
            f"{fast_path_guard}\n"
            "DL_FIND_OBJECT_FAST_PATH\n"
            "#else\n"
            "DL_ITERATE_PHDR_INTERCEPTED_FALLBACK\n"
            "#endif\n"
            f"{feature_guard_cleanup}\n"
        )

        def preprocess(flags, input_source=source):
            result = subprocess.run(
                [
                    compiler,
                    "-E",
                    "-P",
                    "--target=x86_64-pc-linux-gnu",
                    *flags,
                    "-x",
                    "c++",
                    "-",
                ],
                input=input_source,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            return result.stdout

        msan_output = preprocess(["-fsanitize=memory"])
        ordinary_output = preprocess([])
        no_feature_output = preprocess(
            ["-Wno-builtin-macro-redefined"],
            f"#undef __has_feature\n{source}",
        )
        self.assertIn("DL_ITERATE_PHDR_INTERCEPTED_FALLBACK", msan_output)
        self.assertNotIn("DL_FIND_OBJECT_FAST_PATH", msan_output)
        for control_output in (ordinary_output, no_feature_output):
            self.assertIn("DL_FIND_OBJECT_FAST_PATH", control_output)
            self.assertNotIn("DL_ITERATE_PHDR_INTERCEPTED_FALLBACK", control_output)

    def test_libunwind_patch_missing_fails_before_git_apply(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            missing_patch = root / "missing.patch"
            with (
                mock.patch.object(self.toolchain, "LIBUNWIND_MSAN_PATCH", missing_patch),
                mock.patch.object(self.toolchain, "run") as run,
            ):
                with self.assertRaisesRegex(self.toolchain.ToolchainError, "missing or unreadable"):
                    self.toolchain.apply_libunwind_msan_patch(root / "source")
            run.assert_not_called()

    def test_libunwind_patch_modified_fails_before_git_apply(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            modified_patch = root / "modified.patch"
            modified_patch.write_text("not the pinned patch\n", encoding="utf-8")
            with (
                mock.patch.object(self.toolchain, "LIBUNWIND_MSAN_PATCH", modified_patch),
                mock.patch.object(self.toolchain, "run") as run,
            ):
                with self.assertRaisesRegex(self.toolchain.ToolchainError, "SHA-256 mismatch"):
                    self.toolchain.apply_libunwind_msan_patch(root / "source")
            run.assert_not_called()

    def test_libunwind_patch_apply_check_failure_does_not_apply(self):
        commands = []

        def reject_check(command):
            commands.append(command)
            raise self.toolchain.ToolchainError("patch does not apply")

        with mock.patch.object(self.toolchain, "run", side_effect=reject_check):
            with self.assertRaisesRegex(self.toolchain.ToolchainError, "patch does not apply"):
                self.toolchain.apply_libunwind_msan_patch(Path("source"))

        self.assertEqual(len(commands), 1)
        self.assertIn("--check", commands[0])

    def test_build_includes_every_runtime_registered_for_install(self):
        command = self.toolchain.build_command(Path("build"), jobs=4)
        target_index = command.index("--target")
        parallel_index = command.index("--parallel")
        self.assertEqual(
            command[target_index + 1 : parallel_index],
            ["cxx", "cxx_experimental", "cxxabi", "unwind"],
        )

    def test_compiler_major_must_match_pinned_llvm_sources(self):
        self.toolchain.require_compatible_clang("Ubuntu clang version 18.1.3", "clang-18")
        with self.assertRaisesRegex(self.toolchain.ToolchainError, "Clang 22, LLVM 18"):
            self.toolchain.require_compatible_clang("clang version 22.1.7", "clang-22")
        with self.assertRaisesRegex(self.toolchain.ToolchainError, "did not identify itself as Clang"):
            self.toolchain.require_compatible_clang("gcc version 13.2.0", "gcc")

    def test_setup_refuses_non_linux_hosts(self):
        with mock.patch.object(self.toolchain.platform, "system", return_value="Darwin"):
            with self.assertRaisesRegex(self.toolchain.ToolchainError, "only on Linux"):
                self.toolchain.require_linux()

    def test_install_requires_runtime_to_catch_an_uninitialized_read(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix, library_dir, build, _, symbolizer = self.prepare_verification_tree(root)
            negative_run = subprocess.CompletedProcess(
                [], 86, "", self.negative_report()
            )
            output = io.StringIO()
            with (
                mock.patch.object(self.toolchain.subprocess, "run", side_effect=self.diagnostic_processes(negative_run)) as run,
                mock.patch.object(self.toolchain, "resolve_msan_symbolizer", return_value=str(symbolizer)),
                contextlib.redirect_stdout(output),
            ):
                self.toolchain.verify_install(prefix, "clang++", build)

            self.assertIn("Accepted negative-probe report:", output.getvalue())
            self.assertIn(self.negative_report(), output.getvalue())

            commands = [call.args[0] for call in run.call_args_list]
            include_dir = prefix / "include/c++/v1"
            for command in (commands[0], commands[2]):
                self.assertIn("-fsanitize=memory", command)
                self.assertIn("-fsanitize-memory-track-origins=2", command)
                self.assertNotIn("-fno-sanitize-memory-param-retval", command)
                self.assertIn("-stdlib=libc++", command)
                self.assertIn(str(include_dir), " ".join(command))
                self.assertIn(str(library_dir), " ".join(command))
            positive_source = run.call_args_list[0].kwargs["input"]
            self.assertIn("[[gnu::noinline]] void throw_value(int32_t value)", positive_source)
            self.assertIn("volatile int32_t runtime_seed = 0;", positive_source)
            self.assertIn("#include <string>", positive_source)
            self.assertIn("std::to_string(expected)", positive_source)
            self.assertIn("!marker.empty()", positive_source)
            self.assertIn("Cleanup cleanup{count};", positive_source)
            self.assertIn("throw;", positive_source)
            self.assertIn("cleanup_count != 1", positive_source)
            negative_source = run.call_args_list[2].kwargs["input"]
            self.assertIn("std::malloc(sizeof(int))", negative_source)
            self.assertIn("volatile int", negative_source)
            self.assertIn(
                "if (*static_cast<volatile int*>(value) == 0)",
                negative_source,
            )
            self.assertNotIn("return result;", negative_source)
            self.assertIn('extern "C" [[gnu::noinline]] int msanNegativeHeapRead()', negative_source)
            self.assertNotIn("__msan_", negative_source)
            negative_environment = run.call_args_list[6].kwargs["env"]
            self.assertEqual(negative_environment["MSAN_SYMBOLIZER_PATH"], str(symbolizer))
            self.assertIn("symbolize=1", negative_environment["MSAN_OPTIONS"])
            self.assertIn("fast_unwind_on_fatal=1", negative_environment["MSAN_OPTIONS"])
            self.assertEqual(commands[3], ["clang++", "--version"])
            self.assertEqual(commands[4][0], "ldd")
            self.assertEqual(commands[5], [str(symbolizer), "--version"])

    def test_positive_probe_compile_failure_stops_before_execution(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix, _, build, _, _ = self.prepare_verification_tree(root)
            compile_failure = subprocess.CompletedProcess([], 1, "compiler stdout", "compiler stderr")
            with mock.patch.object(
                self.toolchain.subprocess,
                "run",
                side_effect=[compile_failure],
            ) as run:
                with self.assertRaisesRegex(
                    self.toolchain.ToolchainError, "positive-probe compilation"
                ) as raised:
                    self.toolchain.verify_install(prefix, "clang++", build)

        self.assertEqual(run.call_count, 1)
        self.assertIn("compiler stdout", str(raised.exception))
        self.assertIn("compiler stderr", str(raised.exception))

    def test_positive_probe_nonzero_exit_fails_before_negative_control(self):
        message = self.assert_positive_probe_rejected(
            subprocess.CompletedProcess([], -11, "positive stdout", "positive stderr")
        )
        self.assertIn("return code: -11", message)
        self.assertIn("positive stdout", message)
        self.assertIn("positive stderr", message)

    def test_positive_probe_timeout_preserves_partial_output(self):
        message = self.assert_positive_probe_rejected(
            subprocess.TimeoutExpired(
                ["msan-libcxx-positive"],
                30,
                output="partial stdout",
                stderr="partial stderr",
            )
        )
        self.assertIn("return code: 124", message)
        self.assertIn("partial stdout", message)
        self.assertIn("partial stderr", message)
        self.assertIn("timed out after 30 seconds", message)

    def test_positive_probe_sanitizer_diagnostic_fails_even_on_zero_exit(self):
        message = self.assert_positive_probe_rejected(
            subprocess.CompletedProcess(
                [], 0, "", "WARNING: MemorySanitizer: use-of-uninitialized-value\n"
            )
        )
        self.assertIn("emitted a sanitizer diagnostic", message)
        self.assertIn("use-of-uninitialized-value", message)

    def test_verified_toolchain_exports_fast_fatal_unwind_for_analysis_processes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            environment_file = root / "github-env"
            commands = []

            def fake_run(command, **_kwargs):
                commands.append(command)
                if command[-1] == "--version":
                    return "Ubuntu clang version 18.1.3\n"
                if command[-2:] == ["rev-parse", "HEAD"]:
                    return self.toolchain.LLVM_COMMIT
                return ""

            with (
                mock.patch.object(self.toolchain, "require_linux"),
                mock.patch.object(
                    self.toolchain,
                    "resolve_tool",
                    side_effect=["/usr/bin/clang-18", "/usr/bin/clang++-18"],
                ),
                mock.patch.object(self.toolchain, "run", side_effect=fake_run),
                mock.patch.object(self.toolchain, "verify_install"),
                mock.patch.dict(self.toolchain.os.environ, {"GITHUB_ENV": str(environment_file)}),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                result = self.toolchain.main(
                    [
                        "--source-root",
                        str(root / "llvm" / "source"),
                        "--build-root",
                        str(root / "llvm" / "build"),
                        "--prefix",
                        str(root / "msan-libcxx"),
                        "--clang",
                        "clang-18",
                        "--clangxx",
                        "clang++-18",
                        "--jobs",
                        "1",
                    ]
                )

            self.assertEqual(result, 0)
            self.assertIn(
                f"MSAN_OPTIONS={self.toolchain.MSAN_ANALYSIS_OPTIONS}\n",
                environment_file.read_text(encoding="utf-8"),
            )
            identity_index = next(
                index for index, command in enumerate(commands) if command[-2:] == ["rev-parse", "HEAD"]
            )
            patch_check_index = next(
                index for index, command in enumerate(commands) if "--check" in command and "apply" in command
            )
            patch_apply_index = next(
                index
                for index, command in enumerate(commands)
                if "apply" in command and "--check" not in command
            )
            configure_index = next(
                index for index, command in enumerate(commands) if command[:2] == ["cmake", "-G"]
            )
            self.assertLess(identity_index, patch_check_index)
            self.assertLess(patch_check_index, patch_apply_index)
            self.assertLess(patch_apply_index, configure_index)

    def test_source_pin_mismatch_stops_before_patch_or_configure(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            environment_file = root / "github-env"
            commands = []

            def fake_run(command, **_kwargs):
                commands.append(command)
                if command[-1] == "--version":
                    return "Ubuntu clang version 18.1.3\n"
                if command[-2:] == ["rev-parse", "HEAD"]:
                    return "unexpected-source-commit"
                return ""

            with (
                mock.patch.object(self.toolchain, "require_linux"),
                mock.patch.object(
                    self.toolchain,
                    "resolve_tool",
                    side_effect=["/usr/bin/clang-18", "/usr/bin/clang++-18"],
                ),
                mock.patch.object(self.toolchain, "run", side_effect=fake_run),
                mock.patch.dict(self.toolchain.os.environ, {"GITHUB_ENV": str(environment_file)}),
                contextlib.redirect_stdout(io.StringIO()),
                contextlib.redirect_stderr(io.StringIO()),
            ):
                result = self.toolchain.main(
                    [
                        "--source-root",
                        str(root / "llvm" / "source"),
                        "--build-root",
                        str(root / "llvm" / "build"),
                        "--prefix",
                        str(root / "msan-libcxx"),
                        "--clang",
                        "clang-18",
                        "--clangxx",
                        "clang++-18",
                        "--jobs",
                        "1",
                    ]
                )

            self.assertEqual(result, 1)
            self.assertFalse(environment_file.exists())
            self.assertFalse(any("apply" in command for command in commands))
            self.assertFalse(any(command[:2] == ["cmake", "-G"] for command in commands))

    def test_install_accepts_standard_pid_prefix_on_msan_warning(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix, _, build, _, symbolizer = self.prepare_verification_tree(root)
            negative_run = subprocess.CompletedProcess(
                [], 86, "", self.negative_report(pid="==12345==")
            )
            with (
                mock.patch.object(
                    self.toolchain.subprocess,
                    "run",
                    side_effect=self.diagnostic_processes(negative_run),
                ),
                mock.patch.object(
                    self.toolchain, "resolve_msan_symbolizer", return_value=str(symbolizer)
                ),
            ):
                self.toolchain.verify_install(prefix, "clang++", build)

    def test_negative_probe_has_an_ordinary_instrumented_noinline_heap_read(self):
        source = self.toolchain.MSAN_NEGATIVE_PROBE_SOURCE.replace(
            "#include <cstdlib>",
            'extern "C" void* malloc(__SIZE_TYPE__); extern "C" void free(void*);\n'
            "namespace std { using ::malloc; using ::free; }",
        )
        self.assertNotIn("__msan_", source)
        ir = self.compile_msan_ir(source)
        body = self.function_ir(ir, "msanNegativeHeapRead")
        self.assertIn("@malloc(", body)
        self.assertIn("load volatile i32", body)
        self.assertIn("@__msan_warning_with_origin_noreturn(", body)
        attributes = re.search(r"define[^\n]+@msanNegativeHeapRead\([^\n]+#([0-9]+)", ir)
        self.assertIsNotNone(attributes)
        self.assertRegex(ir, rf"attributes #{attributes.group(1)} = \{{[^\n]*\bnoinline\b")

    def test_negative_report_accepts_symbolized_format_variants_and_chained_origin(self):
        report = self.negative_report().replace(
            "  Uninitialized value was created",
            "  Uninitialized value was stored to memory at\n"
            "    #0 0x5678 in otherFunction /tmp/probe.cpp:5:3\n\n"
            "  Uninitialized value was created",
        )
        for variant in (report, report.replace("\n", "\r\n"), "\x1b[1m" + report + "\x1b[0m"):
            with self.subTest(variant=repr(variant[:30])):
                self.assertIsNotNone(self.toolchain.negative_report_evidence(variant))

    def test_negative_report_rejects_unrelated_primary_even_with_named_caller(self):
        report = self.negative_report(read_site="libcStartup").replace("in main", "in msanNegativeHeapRead")
        self.assertIsNone(self.toolchain.negative_report_evidence(report))
        self.assert_negative_probe_rejected(subprocess.CompletedProcess([], 86, "", report))

    def test_negative_report_requires_matching_heap_allocation_provenance(self):
        report = self.negative_report(allocation_site="otherAllocation")
        self.assertIsNone(self.toolchain.negative_report_evidence(report))
        self.assert_negative_probe_rejected(subprocess.CompletedProcess([], 86, "", report))
        for origin in ("a stack frame", "a heap deallocation", "destroyed member fields"):
            with self.subTest(origin=origin):
                self.assertIsNone(self.toolchain.negative_report_evidence(
                    self.negative_report().replace("a heap allocation", origin)))

    def test_negative_report_rejects_missing_origin_and_warning_only(self):
        report = self.negative_report().split("  Uninitialized value", 1)[0]
        self.assertIsNone(self.toolchain.negative_report_evidence(report))
        self.assert_negative_probe_rejected(subprocess.CompletedProcess([], 86, "", report))
        self.assertIsNone(self.toolchain.negative_report_evidence(
            "WARNING: MemorySanitizer: use-of-uninitialized-value\n"))

    def test_negative_report_rejects_named_allocation_caller_at_frame_two(self):
        report = self.negative_report(allocation_site="helperAllocation").replace(
            "    #1 0x4567 in helperAllocation /tmp/probe.cpp:3:37",
            "    #1 0x4567 in helperAllocation /tmp/probe.cpp:3:37\n"
            "    #2 0x5678 in msanNegativeHeapRead /tmp/probe.cpp:4:2",
        )
        self.assertIsNone(self.toolchain.negative_report_evidence(report))
        self.assert_negative_probe_rejected(subprocess.CompletedProcess([], 86, "", report))

    def test_negative_report_cannot_combine_separate_reports_or_text_only_names(self):
        report = self.negative_report(allocation_site="otherAllocation") + self.negative_report(read_site="libcStartup")
        self.assertIsNone(self.toolchain.negative_report_evidence(report))
        report = self.negative_report(allocation_site="otherAllocation") + "msanNegativeHeapRead\n"
        self.assertIsNone(self.toolchain.negative_report_evidence(report))
        report = self.negative_report(allocation_site="otherAllocation").replace(
            "SUMMARY:", "    #0 0x6789 in msanNegativeHeapRead /tmp/unrelated.cpp:1:1\nSUMMARY:")
        self.assertIsNone(self.toolchain.negative_report_evidence(report))

    def test_negative_report_rejects_an_additional_fatal_diagnostic(self):
        self.assertIsNone(self.toolchain.negative_report_evidence(
            self.negative_report() + "MemorySanitizer:DEADLYSIGNAL\n"))
        self.assertIsNone(self.toolchain.negative_report_evidence(
            self.negative_report() + "ERROR: MemorySanitizer: unexpected failure\n"))
        self.assertIsNone(self.toolchain.negative_report_evidence(
            self.negative_report() + "WARNING: MemorySanitizer: unexpected failure\n"))

    def test_negative_report_retention_is_bounded_and_keeps_accepted_provenance(self):
        report = self.negative_report().replace(
            "  Uninitialized value was created", "x" * 20000 + "\n  Uninitialized value was created")
        evidence = self.toolchain.negative_report_evidence(report)
        self.assertIsNotNone(evidence)
        self.assertLess(len(evidence), self.toolchain.MSAN_NEGATIVE_REPORT_LIMIT + 4200)
        self.assertIn("[report truncated; accepted provenance follows]", evidence)
        self.assertIn("#0 0x1234 in msanNegativeHeapRead /tmp/probe.cpp:8:9", evidence)
        self.assertIn("Uninitialized value was created by a heap allocation", evidence)
        self.assertIn("#1 0x4567 in msanNegativeHeapRead /tmp/probe.cpp:3:37", evidence)
        self.assertIsNone(self.toolchain.negative_report_evidence(
            self.negative_report().replace("/tmp/probe.cpp:8:9", "/" + "a" * 2000)))

    def test_install_rejects_a_toolchain_that_misses_the_probe(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix, _, build, _, symbolizer = self.prepare_verification_tree(root)
            missed_probe = subprocess.CompletedProcess([], 0, "", "")
            with (
                mock.patch.object(
                    self.toolchain.subprocess,
                    "run",
                    side_effect=self.diagnostic_processes(missed_probe),
                ),
                mock.patch.object(self.toolchain, "resolve_msan_symbolizer", return_value=str(symbolizer)),
            ):
                with self.assertRaisesRegex(self.toolchain.ToolchainError, "did not diagnose"):
                    self.toolchain.verify_install(prefix, "clang++", build)

    def test_exit_code_86_without_uninitialized_read_diagnostic_is_failure(self):
        message = self.assert_negative_probe_rejected(
            subprocess.CompletedProcess(
                [], 86, "", "MemorySanitizer: stack-overflow\nMemorySanitizer:DEADLYSIGNAL"
            )
        )
        self.assertIn("return code: 86", message)
        self.assertIn("MemorySanitizer: stack-overflow", message)

    def test_exit_code_86_with_unrelated_text_containing_error_kind_is_failure(self):
        message = self.assert_negative_probe_rejected(
            subprocess.CompletedProcess(
                [], 86, "", "not an MSan warning: use-of-uninitialized-value"
            )
        )
        self.assertIn("return code: 86", message)
        self.assertIn("not an MSan warning: use-of-uninitialized-value", message)

    def test_exit_code_86_with_unicode_pid_prefix_is_failure(self):
        message = self.assert_negative_probe_rejected(
            subprocess.CompletedProcess(
                [], 86, "", "==١٢٣==WARNING: MemorySanitizer: use-of-uninitialized-value\n"
            )
        )
        self.assertIn("return code: 86", message)
        self.assertIn("==١٢٣==WARNING: MemorySanitizer: use-of-uninitialized-value", message)

    def test_uninitialized_read_diagnostic_with_wrong_exit_code_is_failure(self):
        message = self.assert_negative_probe_rejected(
            subprocess.CompletedProcess(
                [], 1, "", self.negative_report()
            )
        )
        self.assertIn("return code: 1", message)
        self.assertIn("use-of-uninitialized-value", message)

    def test_negative_probe_timeout_preserves_diagnostics_and_context(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix, _, build, negative_probe, symbolizer = self.prepare_verification_tree(root)
            timeout = subprocess.TimeoutExpired(
                [str(negative_probe)], 30, output="partial stdout", stderr="partial stderr"
            )
            process_results = [
                *self.diagnostic_processes(subprocess.CompletedProcess([], 0, "", ""))[:6],
                timeout,
            ]
            with (
                mock.patch.object(self.toolchain.subprocess, "run", side_effect=process_results),
                mock.patch.object(self.toolchain, "resolve_msan_symbolizer", return_value=str(symbolizer)),
            ):
                with self.assertRaisesRegex(self.toolchain.ToolchainError, "did not diagnose") as raised:
                    self.toolchain.verify_install(prefix, "clang++", build)

        message = str(raised.exception)
        self.assertIn("return code: 124", message)
        self.assertIn("partial stdout", message)
        self.assertIn("partial stderr", message)
        self.assertIn("timed out after 30 seconds", message)
        self.assertIn(f"probe: {negative_probe}", message)

    def test_negative_probe_failure_reports_child_exit_and_diagnostics(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix, _, build, negative_probe, symbolizer = self.prepare_verification_tree(root)
            completed = [
                *self.diagnostic_processes(
                subprocess.CompletedProcess(
                    [],
                    -11,
                    "probe stdout",
                    "MemorySanitizer: stack-overflow\n"
                    "    #0 0x5615fe3a250b in main (/tmp/build/msan-uninitialized-read+0x1250b)\n"
                    "    #1 0x7f0000001234 in operator new (libc++.so+0x1234)\n"
                    "MemorySanitizer:DEADLYSIGNAL",
                ),
                symbolized=[
                    subprocess.CompletedProcess([], 0, "main\n/tmp/probe.cpp:5:7\n", ""),
                    subprocess.CompletedProcess([], 0, "operator new\nlibc++/new.cpp:9:1\n", ""),
                ],
            ),
            ]
            with (
                mock.patch.object(self.toolchain.subprocess, "run", side_effect=completed) as run,
                mock.patch.object(self.toolchain, "resolve_msan_symbolizer", return_value=str(symbolizer)),
            ):
                with self.assertRaisesRegex(self.toolchain.ToolchainError, "did not diagnose") as raised:
                    self.toolchain.verify_install(prefix, "clang++", build)

        message = str(raised.exception)
        self.assertIn("return code: -11", message)
        self.assertIn("stdout:\nprobe stdout", message)
        self.assertIn("stderr:\nMemorySanitizer: stack-overflow", message)
        self.assertIn("kernel:", message)
        self.assertIn("vm.mmap_rnd_bits=", message)
        self.assertIn("stack limit:", message)
        self.assertIn("compiler: clang++ sha256=", message)
        self.assertIn("clang version 18.1.3", message)
        self.assertIn(f"probe: {negative_probe}", message)
        self.assertRegex(message, r"probe: .* sha256=[0-9a-f]{64}")
        self.assertIn("libc++.so", message)
        self.assertIn(f"MSAN_SYMBOLIZER_PATH={symbolizer}", message)
        self.assertIn("symbolized module-relative PCs:\n$ ", message)
        self.assertIn("/tmp/probe.cpp:5:7", message)
        self.assertIn("libc++/new.cpp:9:1", message)
        self.assertIn(f"--obj={negative_probe}", " ".join(run.call_args_list[7].args[0]))
        self.assertIn(
            "--obj=/tmp/prefix/lib/libc++.so",
            " ".join(run.call_args_list[8].args[0]),
        )

    def test_symbolization_keeps_linux_module_paths_posix_on_windows_hosts(self):
        diagnostics = (
            "    #0 0x1 in main (/tmp/build/msan-uninitialized-read+0x1250b)\n"
            "    #1 0x2 in operator new (/tmp/prefix/lib/libc++.so+0x1234)\n"
        )
        symbolized_frames = [
            subprocess.CompletedProcess([], 0, "main\nprobe.cpp:5:7\n", ""),
            subprocess.CompletedProcess([], 0, "operator new\nlibc++/new.cpp:9:1\n", ""),
        ]
        with (
            mock.patch.object(self.toolchain, "Path", PureWindowsPath),
            mock.patch.object(
                self.toolchain.subprocess, "run", side_effect=symbolized_frames
            ) as run,
        ):
            symbolized = self.toolchain.symbolize_reported_pcs(
                diagnostics,
                PureWindowsPath("C:/build/msan-uninitialized-read"),
                "llvm-symbolizer",
                {},
            )

        self.assertEqual(run.call_count, 2)
        self.assertIn(
            "--obj=C:\\build\\msan-uninitialized-read",
            " ".join(run.call_args_list[0].args[0]),
        )
        self.assertIn(
            "--obj=/tmp/prefix/lib/libc++.so",
            " ".join(run.call_args_list[1].args[0]),
        )
        self.assertIn("probe.cpp:5:7", symbolized)
        self.assertIn("libc++/new.cpp:9:1", symbolized)


if __name__ == "__main__":
    unittest.main()
