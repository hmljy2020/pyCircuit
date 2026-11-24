# ruff: noqa: F823, F841 -- Compiler-owned state candidates.
"""Use the first held QueueAdd result from a downstream System Rule."""

import pycircuit as ac
from linked_gfsim.linked_registration_queue import QueueAdd


@ac.module
def IncrementTotal(value: ac.u16) -> ac.u16:
    return value + 1


@ac.system
def CheckedQueueAdd():
    total = QueueAdd(7, 35)
    adjusted = IncrementTotal(total)
    sample_count: ac.u8 = 0

    @ac.rule
    def advance() -> None:
        sample_count = sample_count + 1

    @ac.rule
    def check() -> None:
        if sample_count < 3:
            assert adjusted == 1, "QueueAdd available before its first pop committed"
        else:
            assert adjusted == 43, "QueueAdd result unavailable after commit"
