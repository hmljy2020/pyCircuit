"""FW-0002: a blocking candidate qualifies every selected state effect."""

from pathlib import Path
import re
import unittest

from agentic_circuit._queue_frontend import QueueFrontendError, lower_queue_source

ROOT = Path(__file__).resolve().parents[4]
FIXTURE = (
    ROOT
    / "tests/integration/agentic-circuit/e2e/fixtures/blocking_branch/architecture.py"
)


class BlockingBranchTest(unittest.TestCase):
    def test_blocking_candidate_is_preserved_and_qualifies_writes(self):
        text = lower_queue_source(FIXTURE.read_text(), "blocking_branch")
        candidate = re.search(r"ac.rule.condition (%\w+)", text)[1]
        self.assertNotIn(f"{candidate} = ac.var.constant true", text)
        presences = re.findall(r"ac.var.assign[^\n]* when (%\w+)", text)
        self.assertEqual(5, len(presences))
        for presence in presences:
            self.assertIn(f"{presence} = ac.var.and {candidate},", text)
        self.assertEqual(1, text.count("ac.rule.condition"))

    def test_branch_local_cannot_escape_into_other_arm(self):
        source = (
            FIXTURE.read_text()
            .replace(
                "diagnostics = diagnostics + 1",
                "local = command.value\n            diagnostics = local",
            )
            .replace("retained = command.value", "retained = local")
        )
        with self.assertRaisesRegex(QueueFrontendError, "escapes its defining path"):
            lower_queue_source(source, "blocking_branch")

    def test_constant_bad_index_is_still_rejected(self):
        source = FIXTURE.read_text().replace("entries[0] =", "entries[2] =")
        with self.assertRaisesRegex(QueueFrontendError, "out of range"):
            lower_queue_source(source, "blocking_branch")

    def test_non_boolean_guard_is_still_rejected(self):
        source = FIXTURE.read_text().replace("if command.bad:", "if command.value:")
        with self.assertRaisesRegex(QueueFrontendError, "bool"):
            lower_queue_source(source, "blocking_branch")

    def test_multiple_inputs_remain_outside_this_slice(self):
        source = (
            FIXTURE.read_text()
            .replace("entries, command):", "entries, command, other):")
            .replace("command: Command):", "command: Command, other: Command):")
            .replace("entries, command)", "entries, command, other)")
        )
        with self.assertRaisesRegex(QueueFrontendError, "exactly one Queue input"):
            lower_queue_source(source, "blocking_branch")

    def test_selected_output_remains_outside_this_slice(self):
        source = FIXTURE.read_text().replace(
            "\n\n@ac.system", "\n        return command\n\n@ac.system"
        )
        with self.assertRaisesRegex(QueueFrontendError, "outputless rule"):
            lower_queue_source(source, "blocking_branch")

    def test_static_arguments_do_not_count_as_queue_inputs(self):
        source = (
            FIXTURE.read_text()
            .replace("entries, command):", "entries, command, limit):")
            .replace(
                "if not busy or command.bad:", "if not busy or command.value == limit:"
            )
        )
        for argument, prefix in (("7", ""), ("LIMIT", "LIMIT = 7\n")):
            with self.subTest(argument=argument):
                specialized = prefix + source.replace(
                    "entries, command)", f"entries, command, {argument})"
                )
                lowered = lower_queue_source(specialized, "blocking_branch")
                self.assertEqual(1, lowered.count("ac.source "))
                self.assertIn("ac.rule.condition", lowered)

    def test_static_arguments_do_not_hide_multiple_queue_inputs(self):
        source = (
            FIXTURE.read_text()
            .replace("entries, command):", "entries, command, other, limit):")
            .replace("command: Command):", "command: Command, other: Command):")
            .replace("entries, command)", "entries, command, other, 7)")
        )
        with self.assertRaisesRegex(QueueFrontendError, "exactly one Queue input"):
            lower_queue_source(source, "blocking_branch")
