# ruff: noqa: F823, F841 -- Compiler-owned state candidates.
"""Hardware for port aliasing, transaction timing and support boundaries."""

import pycircuit as ac


@ac.module
def Stage(value: ac.u8, update: ac.u1) -> tuple[ac.u8, ac.u8]:
    saved: ac.u8 = 9

    @ac.rule(guard=lambda: update)
    def capture() -> None:
        saved = value

    return saved, saved ^ 255


@ac.module
def Increment(value: ac.u8) -> ac.u8:
    return value + 1


@ac.module
def Invert(value: ac.u8, unused: ac.u8) -> ac.u8:
    return value ^ 255


@ac.module
def Compute(value: ac.u8) -> ac.u8:
    @ac.method
    def increment(value: ac.u8) -> ac.u8:
        return value + 1

    @ac.rule
    def add_one() -> ac.u8:
        return increment(value)

    return add_one


@ac.module
def Fanout(value: ac.u8) -> tuple[ac.u8, ac.u8, ac.u8]:
    shared = Compute(value)
    extra = value + 3
    return shared, Increment(shared), Invert(shared, extra)


@ac.module
def Cell(limit: ac.u8) -> ac.u8:
    saved: ac.u8 = 4

    @ac.method(guard=lambda value: (value < limit) & (saved < limit))
    def store(value: ac.u8) -> None:
        saved = value

    @ac.method
    def bump() -> None:
        saved = saved + 1

    return saved


@ac.module
def Router(limit: ac.u8) -> tuple[ac.u8, ac.u8]:
    left = Cell(limit)
    right = Cell(12)

    @ac.method
    def route(value: ac.u8, select: ac.u1) -> None:
        if select:
            left.store(value)
            left.bump()
        else:
            right.store(value)
            right.bump()

    return left, right


@ac.module
def ThroughMethods(limit: ac.u8, value: ac.u8, select: ac.u1) -> tuple[ac.u8, ac.u8]:
    bound = Increment(limit)
    cells = Router(bound)

    @ac.rule
    def drive() -> None:
        cells.route(value, select)

    left, right = cells
    return left, right


@ac.module
def TimedQueue(
    value: ac.u8, issue: ac.u1, inspect: ac.u1, take: ac.u1
) -> tuple[ac.u8, ac.u8]:
    pending = ac.queue[ac.u8](depth=1, latency=3)

    @ac.rule(guard=lambda: issue)
    def enqueue() -> None:
        pending.push(value)

    @ac.rule(guard=lambda: inspect)
    def inspect_head() -> ac.u8:
        return pending.peek()

    @ac.rule(guard=lambda: take)
    def dequeue() -> ac.u8:
        return pending.pop()

    return inspect_head, dequeue


@ac.module
def ConflictingPops(
    issue: ac.u1,
    take_first: ac.u1,
    take_second: ac.u1,
    overwrite: ac.u1,
    healthy: ac.u1,
) -> tuple[ac.u8, ac.u8, ac.u8, ac.u8]:
    pending = ac.queue[ac.u8](depth=1, latency=1)
    count: ac.u8 = 0
    snapshot: ac.u8 = 0

    @ac.rule(guard=lambda: issue)
    def enqueue() -> None:
        pending.push(37)

    @ac.rule(guard=lambda: take_first)
    def first() -> ac.u8:
        return pending.pop()

    @ac.rule(guard=lambda: take_second)
    def second() -> ac.u8:
        return pending.pop()

    @ac.rule
    def tick() -> None:
        count = count + 1

    @ac.rule(guard=lambda: overwrite)
    def replace_count() -> None:
        count = 93

    @ac.rule
    def save_snapshot() -> None:
        snapshot = count

    @ac.rule
    def check() -> None:
        assert healthy == 1, "injected check failure"

    return first, second, count, snapshot


@ac.module
def RawBound() -> ac.u1:
    # A zero-valued explicit binding remains a real binding.
    _wired = ac.queue[ac.u8](in_valid=False, in_data=0, depth=1)
    return False


@ac.module
def RawRead(value: ac.u8) -> ac.u1:
    inspected = ac.queue[ac.u8](depth=1)

    @ac.rule
    def enqueue() -> None:
        inspected.push(value)

    return inspected.valid


@ac.module
def Interleaved(value: ac.u8) -> ac.u8:
    prepared = value + 1
    produced = Increment(prepared)
    return produced ^ 255


@ac.module
def UnknownValue() -> ac.u8:
    return ac.four_state("10xz0011")
