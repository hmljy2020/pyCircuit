"""FW-0005: field stores preserve value and transaction semantics."""

import unittest

from agentic_circuit._queue_frontend import QueueFrontendError, lower_queue_source


PREFIX = """import agentic_circuit as ac

@ac.struct
class Entry:
    x: ac.u8
    valid: bool

@ac.struct
class Command:
    index: ac.u1
    value: ac.u8
    enabled: bool

@ac.module
def field_module(command: Command) -> Entry:
    retained: Entry = 0
    entries: list[Entry] = [0] * 2

    @ac.rule
    def update(command):
        nonlocal retained, entries
"""
SUFFIX = """
    result = update(command)
    return result

@ac.system
def fields(command: Command) -> Entry:
    result = field_module(command)
    return result
"""


def source(body):
    return PREFIX + "".join("    " + line + "\n" for line in body.splitlines()) + SUFFIX


def lower(text, name):
    return lower_queue_source(text, name, host_results=True)


class FieldAssignmentTest(unittest.TestCase):
    def test_local_record_matches_value_replacement(self):
        body = """    local = retained
    saved = local
    local.x = command.value
    local.valid = True
    return local
"""
        explicit = body.replace("local.x = command.value", "local = local.with_fields(x=command.value)").replace(
            "local.valid = True", "local = local.with_fields(valid=True)"
        )
        raw = lower(source(body), "fields")
        self.assertEqual(raw, lower(source(explicit), "fields"))
        self.assertNotIn("ac.var.assign ", raw)

    def test_state_and_list_match_explicit_replacement(self):
        body = """    retained.x = command.value
    retained.valid = True
    entries[command.index].x = command.value
    entries[command.index].valid = True
    result = entries[command.index]
    return result
"""
        # Complex indices are captured once by the normalizer.
        explicit = """    retained = retained.with_fields(x=command.value)
    retained = retained.with_fields(valid=True)
    __ac_field_index = command.index
    entries[__ac_field_index] = entries[__ac_field_index].with_fields(x=command.value)
    __ac_field_index_ = command.index
    entries[__ac_field_index_] = entries[__ac_field_index_].with_fields(valid=True)
    result = entries[command.index]
    return result
"""
        raw = lower(source(body), "fields")
        self.assertEqual(raw, lower(source(explicit), "fields"))

    def test_static_loop(self):
        body = """    for i in range(2):
        entries[i].valid = False
    return retained
"""
        explicit = body.replace(
            "entries[i].valid = False", "entries[i] = entries[i].with_fields(valid=False)"
        )
        self.assertEqual(lower(source(body), "fields"), lower(source(explicit), "fields"))

    def test_conditional_local_preserves_prior_value(self):
        body = """    local = retained
    if command.enabled:
        local.x = command.value
    else:
        local.valid = False
    return local
"""
        explicit = body.replace("local.x = command.value", "local = local.with_fields(x=command.value)").replace(
            "local.valid = False", "local = local.with_fields(valid=False)"
        )
        self.assertEqual(lower(source(body), "fields"), lower(source(explicit), "fields"))

    def test_missing_nonlocal_remains_an_error(self):
        text = source("    retained.valid = True\n    return retained\n").replace(
            "        nonlocal retained, entries\n", ""
        )
        with self.assertRaisesRegex(QueueFrontendError, "nonlocal declaration"):
            lower(text, "fields")

    def test_find_selected_index_keeps_valid_guard(self):
        body = """    found = ac.find(entries, where=lambda row: row.valid)
    if found.valid:
        entries[found.index].valid = False
        return found.value
"""
        self.assertIn("ac.var.choose", lower(source(body), "fields"))

    def test_optional_multi_output_fields(self):
        body = """    retained.x = command.value
    entries[0].valid = True
    a = retained
    a.valid = True
    b = retained
    return a, b
"""
        text = source(body).replace("-> Entry:", "-> tuple[Entry, Entry]:").replace(
            "def update(command):", "def update(command) -> tuple[Entry, Entry]:"
        ).replace("result = update(command)", "left, right = update(command)").replace(
            "result = field_module(command)", "left, right = field_module(command)"
        ).replace("return result", "return left, right")
        self.assertIn("ac.rule.output", lower(text, "fields"))

    def test_repeated_updates_keep_bounded_ssa_and_user_names(self):
        body = """    __ac_list_write_0 = command.value
    for i in range(32):
        entries[0].x = entries[0].x + 1
    retained.x = __ac_list_write_0
    return retained
"""
        raw = lower(source(body), "fields")
        self.assertLess(len(raw), 40000)
        self.assertEqual(raw.count("ac.var.with "), 33)

    def test_invalid_fields_and_targets(self):
        for assignment, diagnostic in (
            ("local.missing = True", "unknown field"),
            ("local.valid = command.value", "type mismatch"),
            ("local.x.y = 0", "nested field"),
            ("local.x += 1", "augmented field"),
            ("entries[:].valid = False", "persistent list element"),
            ("entries[2].valid = False", "out of range"),
            ("command.value = 1", "persistent"),
        ):
            with self.subTest(assignment=assignment):
                with self.assertRaisesRegex(QueueFrontendError, diagnostic):
                    lower(source(f"    local = retained\n    {assignment}\n    return local\n"), "fields")


if __name__ == "__main__":
    unittest.main()
