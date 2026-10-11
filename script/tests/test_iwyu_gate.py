from __future__ import annotations

import hashlib
import io
import json
import subprocess
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import patch

from script.ci import iwyu_gate


class IwyuGateTests(unittest.TestCase):
    def create_repository(self, root: Path) -> tuple[Path, Path]:
        source = root / "src/sample.cpp"
        source.parent.mkdir(parents=True)
        source.write_text(
            '#include <glm/vec3.hpp>\n#include <glm/gtc/matrix_transform.hpp>\n'
            "int main() { return 0; }\n",
            encoding="utf-8",
        )
        header = root / "src/include/sample/Public.hpp"
        header.parent.mkdir(parents=True)
        header.write_text("struct Public {};\n", encoding="utf-8")
        return source, header

    def clean_report(self, source: Path) -> bytes:
        return f"({source} has correct #includes/fwd-decls)\n".encode("utf-8")

    def report_with(self, source: Path, additions: str, removals: str = "") -> bytes:
        return (
            f"{source} should add these lines:\n{additions}\n"
            f"{source} should remove these lines:\n{removals}\n"
            f"The full include-list for {source}:\n#include <glm/vec3.hpp>\n---\n"
        ).encode("utf-8")

    def invoke(
        self,
        root: Path,
        source: Path,
        output: bytes,
        returncode: int = 0,
        compiler_arguments: list[str] | None = None,
    ):
        stdout = io.StringIO()
        compiler_arguments = compiler_arguments or ["-std=c++20", "-c", str(source)]
        wrapper_arguments = [
            "--",
            "include-what-you-use",
            "-Xiwyu",
            "--no_fwd_decls",
            "-Xiwyu",
            "--error",
            "--",
            *compiler_arguments,
        ]
        completed = subprocess.CompletedProcess(
            ["include-what-you-use", *wrapper_arguments[2:]], returncode, output
        )
        with (
            patch.object(iwyu_gate, "REPOSITORY_ROOT", root),
            patch.object(iwyu_gate, "_project_input_sha256", return_value="a" * 64),
            patch.object(iwyu_gate.subprocess, "run", return_value=completed) as run,
            redirect_stdout(stdout),
        ):
            exit_code = iwyu_gate.main(wrapper_arguments)
        marker_lines = stdout.getvalue().splitlines()
        result_line = next(line for line in marker_lines if line.startswith(iwyu_gate.RESULT_MARKER))
        record = json.loads(result_line[len(iwyu_gate.RESULT_MARKER) :])
        return exit_code, record, marker_lines, run

    def test_clean_report_passes_and_writes_source_bound_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, _ = self.create_repository(root)
            output = self.clean_report(source)

            exit_code, record, markers, run = self.invoke(root, source, output)

            self.assertEqual(exit_code, 0)
            self.assertEqual(record["status"], "passed")
            self.assertEqual(record["source"], "src/sample.cpp")
            self.assertEqual(record["source_sha256"], hashlib.sha256(source.read_bytes()).hexdigest())
            self.assertEqual(record["project_input_sha256"], "a" * 64)
            self.assertEqual(record["report_sha256"], hashlib.sha256(output).hexdigest())
            self.assertEqual(record["source_argv"], ["-std=c++20", "-c", str(source)])
            self.assertEqual(
                record["adapter_argv"],
                [
                    "include-what-you-use",
                    "-Xiwyu",
                    "--no_fwd_decls",
                    "-Xiwyu",
                    "--error",
                    "--",
                    *record["source_argv"],
                ],
            )
            self.assertEqual(record["invoked_tool"], "include-what-you-use")
            expected_tool_argv = [
                "include-what-you-use",
                "-Xiwyu",
                "--no_fwd_decls",
                *record["source_argv"],
            ]
            self.assertEqual(record["tool_argv"], expected_tool_argv)
            self.assertEqual(run.call_args.args[0], expected_tool_argv)
            self.assertNotIn("--", record["tool_argv"])
            self.assertEqual((root / record["log"]).read_bytes(), output)
            self.assertEqual(json.loads((root / record["record"]).read_text()), record)
            self.assertEqual(len(markers), 2)
            self.assertTrue(markers[0].startswith(iwyu_gate.COMMAND_MARKER))

    def test_empty_full_include_list_is_valid_when_report_sections_are_complete(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, _ = self.create_repository(root)
            report = (
                f"{source} should add these lines:\n"
                f"{source} should remove these lines:\n"
                f"The full include-list for {source}:\n---\n"
            ).encode("utf-8")

            exit_code, record, _, _ = self.invoke(root, source, report)

            self.assertEqual(exit_code, 0)
            self.assertEqual(record["status"], "passed")

    def test_receipt_paths_do_not_collide_for_different_compile_arguments(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            first = iwyu_gate._receipt_paths("tests/sample.cpp", ["-std=c++20", "-c", "sample.cpp"], root)
            second = iwyu_gate._receipt_paths("tests/sample.cpp", ["-std=c++23", "-c", "sample.cpp"], root)

            self.assertNotEqual(first[:2], second[:2])
            self.assertEqual(
                first[0].stem,
                hashlib.sha256(
                    b'tests/sample.cpp\0["-std=c++20","-c","sample.cpp"]'
                ).hexdigest(),
            )

    def test_public_include_addition_blocks_even_when_tool_exits_zero(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, _ = self.create_repository(root)
            output = self.report_with(source, "#include <vector>  // for vector")

            exit_code, record, _, _ = self.invoke(root, source, output)

            self.assertEqual(exit_code, 1)
            self.assertEqual(record["status"], "blocked")
            self.assertEqual(record["public_additions"], ["src/sample.cpp: #include <vector>  // for vector"])

    def test_unknown_public_include_blocks_by_default(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, _ = self.create_repository(root)
            output = self.report_with(source, '#include "Core.hpp"  // for Core')

            exit_code, record, _, _ = self.invoke(root, source, output)

            self.assertEqual(exit_code, 1)
            self.assertEqual(record["status"], "blocked")
            self.assertEqual(
                record["public_additions"],
                ['src/sample.cpp: unmapped quoted addition #include "Core.hpp"  // for Core'],
            )

    def test_unmapped_project_detail_include_is_not_silently_ignored(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, _ = self.create_repository(root)
            output = self.report_with(source, "#include <shared/detail/Missing.hpp>")

            exit_code, record, _, _ = self.invoke(root, source, output)

            self.assertEqual(exit_code, 1)
            self.assertEqual(record["status"], "blocked")
            self.assertEqual(
                record["public_additions"],
                ["src/sample.cpp: #include <shared/detail/Missing.hpp>"],
            )

    def test_fmt_private_header_maps_only_with_proven_api_and_facade(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, _ = self.create_repository(root)
            with source.open("a", encoding="utf-8") as stream:
                stream.write('#include <fmt/format.h>\nvoid use_fmt() { fmt::format("ok"); }\n')
            output = self.report_with(source, "#include <fmt/base.h>  // for format")

            exit_code, record, _, _ = self.invoke(root, source, output)

            self.assertEqual(exit_code, 0)
            self.assertEqual(record["status"], "passed")
            self.assertEqual(
                record["mapped_facades"],
                ["src/sample.cpp: #include <fmt/base.h>  // for format -> <fmt/format.h> (already included)"],
            )

    def test_quoted_project_header_addition_reports_owning_public_path(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, header = self.create_repository(root)
            output = self.report_with(source, f'#include "{header.relative_to(root).as_posix()}"')

            exit_code, record, _, _ = self.invoke(root, source, output)

            self.assertEqual(exit_code, 1)
            self.assertEqual(
                record["public_additions"],
                ["src/sample.cpp: use public include <sample/Public.hpp> instead of #include \"src/include/sample/Public.hpp\""],
            )

    def test_quoted_nested_project_paths_map_to_existing_module_facades(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, _ = self.create_repository(root)
            facades = {
                "world/World.hpp": "shared/world/World.hpp",
                "world/Chunk.hpp": "shared/world/Chunk.hpp",
                "world/ScriptedWorld.hpp": "shared/world/ScriptedWorld.hpp",
                "net/Message.hpp": "shared/net/Message.hpp",
                "Camera.hpp": "client/Camera.hpp",
            }
            for facade in facades.values():
                header = root / "src" / facade.split("/", 1)[0] / "include" / facade
                header.parent.mkdir(parents=True, exist_ok=True)
                header.write_text("// public test header\n", encoding="utf-8")
            with source.open("a", encoding="utf-8") as stream:
                stream.writelines(f"#include <{facade}>\n" for facade in facades.values())
            additions = "\n".join(f'#include "{name}"' for name in facades)

            exit_code, record, _, _ = self.invoke(root, source, self.report_with(source, additions))

            self.assertEqual(exit_code, 0)
            self.assertEqual(record["status"], "passed")
            self.assertEqual(record["public_additions"], [])
            self.assertEqual(
                record["mapped_facades"],
                sorted(
                    f"src/sample.cpp: #include \"{name}\" -> <{facade}> (already included)"
                    for name, facade in facades.items()
                ),
            )

    def test_quoted_nested_project_path_requires_the_public_facade(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, _ = self.create_repository(root)
            header = root / "src/shared/include/shared/world/World.hpp"
            header.parent.mkdir(parents=True)
            header.write_text("// public test header\n", encoding="utf-8")

            exit_code, record, _, _ = self.invoke(
                root, source, self.report_with(source, '#include "world/World.hpp"')
            )

            self.assertEqual(exit_code, 1)
            self.assertEqual(
                record["public_additions"],
                [
                    'src/sample.cpp: use public include <shared/world/World.hpp> instead of #include "world/World.hpp"'
                ],
            )

    def test_ambiguous_quoted_project_suffix_remains_blocking(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, _ = self.create_repository(root)
            headers = (
                root / "src/shared/include/world/World.hpp",
                root / "src/client/include/client/world/World.hpp",
            )
            for header in headers:
                header.parent.mkdir(parents=True)
                header.write_text("// public test header\n", encoding="utf-8")

            exit_code, record, _, _ = self.invoke(
                root, source, self.report_with(source, '#include "world/World.hpp"')
            )

            self.assertEqual(exit_code, 1)
            self.assertEqual(
                record["public_additions"],
                ['src/sample.cpp: unmapped quoted addition #include "world/World.hpp"'],
            )

    def test_known_sdk_private_include_is_visible_but_not_applied(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, _ = self.create_repository(root)
            output = self.report_with(source, "#include <_stdlib.h>  // for abs")

            exit_code, record, _, _ = self.invoke(root, source, output)

            self.assertEqual(exit_code, 0)
            self.assertEqual(record["status"], "passed")
            self.assertEqual(record["public_additions"], [])
            self.assertEqual(
                record["unsupported_suggestions"],
                ["src/sample.cpp: #include <_stdlib.h>  // for abs (platform or standard-library private header; do not add directly)"],
            )

    def test_private_facade_and_removal_suggestions_are_reported_without_blocking(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, _ = self.create_repository(root)
            with source.open("a", encoding="utf-8") as stream:
                stream.write("#include <glm/ext/matrix_clip_space.hpp>\n")
            output = self.report_with(
                source,
                "#include <glm/detail/type_vec3.inl>  // for vec\n"
                "#include <glm/ext/matrix_transform.inl>  // for lookAtRH\n"
                "#include <glm/ext/matrix_clip_space.inl>  // for perspectiveRH_ZO",
                "- #include <vector>  // lines 2-2\nnamespace sample { class Item; }",
            )

            exit_code, record, _, _ = self.invoke(root, source, output)

            self.assertEqual(exit_code, 0)
            self.assertEqual(record["status"], "passed")
            self.assertEqual(record["mapped_facades"], [
                "src/sample.cpp: #include <glm/detail/type_vec3.inl>  // for vec -> <glm/vec3.hpp> (already included)",
                "src/sample.cpp: #include <glm/ext/matrix_clip_space.inl>  // for perspectiveRH_ZO -> <glm/ext/matrix_clip_space.hpp> (already included)",
                "src/sample.cpp: #include <glm/ext/matrix_transform.inl>  // for lookAtRH -> <glm/gtc/matrix_transform.hpp> (already included)",
            ])
            self.assertEqual(record["removals"], ["src/sample.cpp: #include <vector>  // lines 2-2"])
            self.assertEqual(record["forward_declarations"], ["src/sample.cpp: namespace sample { class Item; }"])
            self.assertEqual(record["public_additions"], [])

    def test_enet_private_headers_map_only_when_public_umbrella_is_resolvable(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, _ = self.create_repository(root)
            include_root = root / "vendor/include"
            enet_root = include_root / "enet"
            enet_root.mkdir(parents=True)
            for name in ("enet.h", "protocol.h", "unix.h"):
                (enet_root / name).write_text("// test header\n", encoding="utf-8")
            with source.open("a", encoding="utf-8") as stream:
                stream.write("#include <enet/enet.h>\n")
            compiler_arguments = ["-isystem", str(include_root), "-c", str(source)]
            additions = "#include <enet/protocol.h>\n#include <enet/unix.h>"

            exit_code, record, _, _ = self.invoke(
                root,
                source,
                self.report_with(source, additions),
                compiler_arguments=compiler_arguments,
            )

            self.assertEqual(exit_code, 0)
            self.assertEqual(record["public_additions"], [])
            self.assertEqual(
                record["mapped_facades"],
                [
                    "src/sample.cpp: #include <enet/protocol.h> -> <enet/enet.h> (already included)",
                    "src/sample.cpp: #include <enet/unix.h> -> <enet/enet.h> (already included)",
                ],
            )

    def test_enet_private_header_does_not_map_without_resolvable_umbrella(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, _ = self.create_repository(root)
            include_root = root / "vendor/include"
            (include_root / "enet").mkdir(parents=True)
            (include_root / "enet/protocol.h").write_text("// private test header\n", encoding="utf-8")

            exit_code, record, _, _ = self.invoke(
                root,
                source,
                self.report_with(source, "#include <enet/protocol.h>"),
                compiler_arguments=["-isystem", str(include_root), "-c", str(source)],
            )

            self.assertEqual(exit_code, 1)
            self.assertEqual(
                record["public_additions"],
                ["src/sample.cpp: #include <enet/protocol.h>"],
            )

    def test_missing_public_include_in_owned_header_blocks_translation_unit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, header = self.create_repository(root)
            report = self.clean_report(source) + self.report_with(header, "#include <string>")

            exit_code, record, _, _ = self.invoke(root, source, report)

            self.assertEqual(exit_code, 1)
            self.assertEqual(record["status"], "blocked")
            self.assertEqual(record["public_additions"], ["src/include/sample/Public.hpp: #include <string>"])

    def test_empty_and_malformed_reports_fail_closed(self):
        cases = (
            (b"", "empty or contains no recognized"),
            (b"fatal error: compiler crashed\n", "unrecognized IWYU output"),
            (
                b"/tmp/source.cpp should add these lines:\n#include <vector\"\n",
                "malformed IWYU suggestion",
            ),
        )
        for output, message in cases:
            with self.subTest(output=output), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                source, _ = self.create_repository(root)

                exit_code, record, _, _ = self.invoke(root, source, output, 1)

                self.assertEqual(exit_code, 2)
                self.assertEqual(record["status"], "error")
                self.assertIn(message, record["diagnostic"])

    def test_any_nonzero_tool_status_fails_even_with_a_valid_report(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, _ = self.create_repository(root)

            exit_code, record, _, _ = self.invoke(root, source, self.clean_report(source), 1)

            self.assertEqual(exit_code, 2)
            self.assertEqual(record["status"], "error")
            self.assertIn("exited with nonzero status 1", record["diagnostic"])

    def test_tool_launch_failure_fails_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, _ = self.create_repository(root)
            stdout = io.StringIO()
            arguments = [
                "--",
                "missing-include-what-you-use",
                "-Xiwyu",
                "--no_fwd_decls",
                "-Xiwyu",
                "--error",
                "--",
                "-c",
                str(source),
            ]
            with (
                patch.object(iwyu_gate, "REPOSITORY_ROOT", root),
                patch.object(iwyu_gate, "_project_input_sha256", return_value="a" * 64),
                patch.object(iwyu_gate.subprocess, "run", side_effect=OSError("not found")),
                redirect_stdout(stdout),
            ):
                exit_code = iwyu_gate.main(arguments)

            result_line = next(
                line for line in stdout.getvalue().splitlines() if line.startswith(iwyu_gate.RESULT_MARKER)
            )
            record = json.loads(result_line[len(iwyu_gate.RESULT_MARKER) :])
            self.assertEqual(exit_code, 2)
            self.assertEqual(record["status"], "error")
            self.assertIn("cannot execute IWYU", record["diagnostic"])

    def test_missing_iwyu_separator_is_a_configuration_error(self):
        stdout = io.StringIO()
        arguments = [
            "--",
            "include-what-you-use",
            "-Xiwyu",
            "--no_fwd_decls",
            "-Xiwyu",
            "--error",
            "-c",
            "file.cpp",
        ]
        with redirect_stdout(stdout):
            exit_code = iwyu_gate.main(arguments)

        self.assertEqual(exit_code, 2)
        result_line = next(
            line for line in stdout.getvalue().splitlines() if line.startswith(iwyu_gate.RESULT_MARKER)
        )
        record = json.loads(result_line[len(iwyu_gate.RESULT_MARKER) :])
        self.assertEqual(record["status"], "error")
        self.assertIn("missing IWYU/compiler argument separator", record["diagnostic"])

    def test_cmake_error_exit_pair_is_required_before_compiler_separator(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, _ = self.create_repository(root)
            stdout = io.StringIO()
            arguments = [
                "--",
                "include-what-you-use",
                "-Xiwyu",
                "--no_fwd_decls",
                "--",
                "-c",
                str(source),
            ]
            with (
                patch.object(iwyu_gate, "REPOSITORY_ROOT", root),
                patch.object(iwyu_gate, "_project_input_sha256", return_value="a" * 64),
                redirect_stdout(stdout),
            ):
                exit_code = iwyu_gate.main(arguments)

            result_line = next(
                line for line in stdout.getvalue().splitlines() if line.startswith(iwyu_gate.RESULT_MARKER)
            )
            record = json.loads(result_line[len(iwyu_gate.RESULT_MARKER) :])
            self.assertEqual(exit_code, 2)
            self.assertEqual(record["status"], "error")
            self.assertIn("must end with '-Xiwyu --error'", record["diagnostic"])


if __name__ == "__main__":
    unittest.main()
