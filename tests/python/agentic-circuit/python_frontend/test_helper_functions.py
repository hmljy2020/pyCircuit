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

    def test_inline_helper_requires_one_return_expression(self) -> None:
        from agentic_circuit._queue_frontend import QueueFrontendError, parse_queue_program

        source = """
import agentic_circuit as ac

@ac.inline
def invalid(value: ac.u8) -> ac.u8:
    temporary = value + 1
    return temporary

@ac.system
def pipeline() -> None:
    ac.sink(ac.source(ac.u8))
"""
        with self.assertRaisesRegex(QueueFrontendError, "ACPY-HELPER-002"):
            parse_queue_program(source, "pipeline")


if __name__ == "__main__":
    unittest.main()
