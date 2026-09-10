from __future__ import annotations

import unittest


HELPER_SOURCE = """
import agentic_circuit as ac

def add_one(value: ac.u8) -> ac.u8:
    return value + 1

@ac.inline
def twice_after_increment(value: ac.u8) -> ac.u8:
    return add_one(value=value) * 2

@ac.system
def pipeline() -> None:
    incoming = ac.source(ac.u8)
    outgoing = incoming.apply(lambda item: twice_after_increment(item))
    ac.sink(outgoing)
"""


STRUCTURED_HELPER_SOURCE = """
import agentic_circuit as ac

def choose_adjustment(value: ac.u8, flag: bool) -> ac.u8:
    result = value
    if flag:
        result = value + 1
    else:
        result = value + 2
    return result

@ac.system
def pipeline() -> None:
    incoming = ac.source(ac.u8)
    outgoing = incoming.apply(lambda item: choose_adjustment(item, True))
    ac.sink(outgoing)
"""


MULTI_RESULT_HELPER_SOURCE = """
import agentic_circuit as ac

def classify(value: ac.u8, flag: bool) -> tuple[ac.u8, bool]:
    adjusted = value
    seen = flag
    if flag:
        adjusted = value + 1
        seen = True
    else:
        adjusted = value + 2
    return adjusted, seen

@ac.rule
def split(item) -> tuple[ac.u8, bool]:
    adjusted, seen = classify(item, item != 0)
    return adjusted, seen

@ac.system
def pipeline() -> tuple[ac.u8, bool]:
    incoming = ac.source(ac.u8)
    left, right = split(incoming)
    ac.sink(left)
    ac.sink(right)
    return left, right
"""


class HelperFunctionTest(unittest.TestCase):
    def test_public_inline_decorator_marks_and_preserves_callable(self) -> None:
        import agentic_circuit as ac

        def helper(value: int) -> int:
            return value + 1

        decorated = ac.inline(helper)

        self.assertIs(helper, decorated)
        self.assertTrue(decorated.__agentic_circuit_inline__)
        self.assertEqual(4, decorated(3))

    def test_frontend_emits_typed_helper_calls_and_inline_intent(self) -> None:
        from agentic_circuit._queue_frontend import (
            lower_queue_program,
            parse_queue_program,
        )

        lowered = lower_queue_program(parse_queue_program(HELPER_SOURCE, "pipeline"))

        self.assertIn("func.func private @add_one", lowered)
        self.assertIn("ac.helper = true, ac.inline = false", lowered)
        self.assertIn("func.func private @twice_after_increment", lowered)
        self.assertIn("ac.helper = true, ac.inline = true", lowered)
        self.assertIn("func.call @add_one", lowered)
        self.assertIn("func.call @twice_after_increment", lowered)

    def test_recursive_helpers_are_rejected(self) -> None:
        from agentic_circuit._queue_frontend import QueueFrontendError, parse_queue_program

        source = """
import agentic_circuit as ac

def left(value: ac.u8) -> ac.u8:
    return right(value)

def right(value: ac.u8) -> ac.u8:
    return left(value)

@ac.system
def pipeline() -> None:
    ac.sink(ac.source(ac.u8))
"""
        with self.assertRaisesRegex(QueueFrontendError, "ACPY-HELPER-003"):
            parse_queue_program(source, "pipeline")

    def test_structured_helper_lowers_locals_and_branch_to_ssa(self) -> None:
        from agentic_circuit._queue_frontend import (
            lower_queue_program,
            parse_queue_program,
        )

        lowered = lower_queue_program(
            parse_queue_program(STRUCTURED_HELPER_SOURCE, "pipeline")
        )

        self.assertIn("func.func private @choose_adjustment", lowered)
        self.assertIn("ac.var.select", lowered)
        self.assertIn("func.call @choose_adjustment", lowered)

    def test_return_annotation_types_a_new_literal_local(self) -> None:
        from agentic_circuit._queue_frontend import (
            lower_queue_program,
            parse_queue_program,
        )

        source = STRUCTURED_HELPER_SOURCE.replace(
            "result = value",
            "result = 1",
        )
        lowered = lower_queue_program(parse_queue_program(source, "pipeline"))

        self.assertIn("ac.var.constant 1 : i8 as !ac.var<i8>", lowered)

    def test_helper_field_assignment_is_an_immutable_local_update(self) -> None:
        from agentic_circuit._queue_frontend import (
            lower_queue_program,
            parse_queue_program,
        )

        source = """
import agentic_circuit as ac

@ac.struct
class Entry:
    value: ac.u8
    valid: bool

def mark(entry: Entry) -> Entry:
    entry.valid = True
    return entry

@ac.system
def pipeline() -> None:
    incoming = ac.source(Entry)
    outgoing = incoming.apply(lambda item: mark(item))
    ac.sink(outgoing)
"""
        lowered = lower_queue_program(parse_queue_program(source, "pipeline"))

        self.assertIn("func.func private @mark", lowered)
        self.assertIn('ac.var.with %arg0, %v0 field "valid"', lowered)

    def test_multi_result_helper_lowers_to_one_call_and_direct_unpacking(self) -> None:
        from agentic_circuit._queue_frontend import (
            lower_queue_program,
            parse_queue_program,
        )

        lowered = lower_queue_program(
            parse_queue_program(MULTI_RESULT_HELPER_SOURCE, "pipeline")
        )

        self.assertIn(
            "func.func private @classify(%arg0: !ac.var<i8>, %arg1: !ac.var<i1>) "
            "-> (!ac.var<i8>, !ac.var<i1>)",
            lowered,
        )
        self.assertEqual(1, lowered.count("func.call @classify"))
        self.assertRegex(lowered, r"%v[0-9]+, %v[0-9]+ = func.call @classify")

    def test_helper_rejects_local_not_defined_on_every_path(self) -> None:
        from agentic_circuit._queue_frontend import (
            QueueFrontendError,
            lower_queue_program,
            parse_queue_program,
        )

        source = """
import agentic_circuit as ac

@ac.inline
def invalid(value: ac.u8, flag: bool) -> ac.u8:
    if flag:
        temporary = value + 1
    return temporary

@ac.system
def pipeline() -> None:
    incoming = ac.source(ac.u8)
    outgoing = incoming.apply(lambda item: invalid(item, True))
    ac.sink(outgoing)
"""
        with self.assertRaisesRegex(QueueFrontendError, "ACPY-HELPER-006"):
            lower_queue_program(parse_queue_program(source, "pipeline"))

    def test_multi_result_helper_requires_direct_unpacking(self) -> None:
        from agentic_circuit._queue_frontend import (
            QueueFrontendError,
            lower_queue_program,
            parse_queue_program,
        )

        source = MULTI_RESULT_HELPER_SOURCE.replace(
            "adjusted, seen = classify(item, item != 0)\n    return adjusted, seen",
            "seen = False\n    adjusted = classify(item, item != 0)\n    return adjusted, seen",
        )
        with self.assertRaisesRegex(QueueFrontendError, "direct unpacking"):
            lower_queue_program(parse_queue_program(source, "pipeline"))

    def test_helper_may_unpack_another_helper(self) -> None:
        from agentic_circuit._queue_frontend import (
            lower_queue_program,
            parse_queue_program,
        )

        source = MULTI_RESULT_HELPER_SOURCE.replace(
            "@ac.rule\ndef split",
            "def collapse(value: ac.u8) -> ac.u8:\n"
            "    adjusted, seen = classify(value, value != 0)\n"
            "    return adjusted if seen else value\n\n"
            "@ac.rule\ndef split",
        ).replace(
            "adjusted, seen = classify(item, item != 0)",
            "adjusted = collapse(item)\n    seen = item != 0",
        )
        lowered = lower_queue_program(parse_queue_program(source, "pipeline"))

        collapse = lowered[lowered.index("func.func private @collapse") :]
        self.assertRegex(collapse, r"%v[0-9]+, %v[0-9]+ = func.call @classify")

    def test_helper_rejects_early_return(self) -> None:
        from agentic_circuit._queue_frontend import QueueFrontendError, parse_queue_program

        source = """
import agentic_circuit as ac

@ac.inline
def invalid(value: ac.u8, flag: bool) -> ac.u8:
    if flag:
        return value
    return value + 1

@ac.system
def pipeline() -> None:
    incoming = ac.source(ac.u8)
    outgoing = incoming.apply(lambda item: invalid(item, True))
    ac.sink(outgoing)
"""
        with self.assertRaisesRegex(QueueFrontendError, "ACPY-HELPER-002"):
            parse_queue_program(source, "pipeline")


if __name__ == "__main__":
    unittest.main()
