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


@ac.struct
class SavedWords:
    words: ac.table[2, ac.u8]
    tag: ac.u8


@ac.module
def SwapState(
    word: ac.u8, divisor: ac.u8, healthy: ac.u1
) -> tuple[SavedWords, ac.table[2, ac.u8], ac.u8]:
    left: SavedWords = SavedWords(words=(9, 12), tag=3)
    right: ac.table[2, ac.u8] = (23, 31)
    next_left = SavedWords(words=(right[0] // divisor, right[1]), tag=3)
    next_right = left.words
    preview = word ^ right[0]

    @ac.rule
    def update_left() -> None:
        left = next_left

    @ac.rule
    def update_right() -> None:
        right = next_right

    @ac.rule
    def check() -> None:
        assert healthy == 1, "swap rejected"

    return left, right, preview


@ac.struct
class WordPair:
    first: ac.u32
    second: ac.u32


def mix_word(word: ac.u32) -> ac.u32:
    left = word
    right = ~word
    for _step in range(24):
        left = (left << 1) ^ (left >> 3) ^ 17
        right = (right << 2) ^ (right >> 1) ^ 41
    return left ^ right


@ac.module
def CachedWork(
    next_word: ac.u32, next_other: ac.u32, noise: ac.u32
) -> tuple[WordPair, WordPair, ac.u8]:
    current: WordPair = WordPair(first=3, second=5)
    preview = WordPair(
        first=mix_word(current.first), second=mix_word(current.second) ^ noise
    )
    following = WordPair(first=next_word, second=next_other)

    @ac.rule
    def advance() -> WordPair:
        current = following
        return preview

    return advance, current, 51


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


@ac.struct
class TableRow:
    left: ac.u8
    right: ac.u8


@ac.module
def TableState(
    index: ac.u1, value: ac.u8, other: ac.u8, healthy: ac.u1
) -> tuple[ac.table[3, TableRow], ac.u8, ac.u8]:
    rows: ac.table[3, TableRow] = (
        TableRow(left=1, right=2),
        TableRow(left=3, right=4),
        TableRow(left=5, right=6),
    )

    @ac.rule
    def write_left() -> ac.u8:
        rows[index].left = value
        return rows[index].left

    @ac.rule
    def write_right() -> None:
        rows[index].right = other

    @ac.rule
    def replace_last() -> None:
        rows[2] = TableRow(left=other, right=value)

    @ac.rule
    def snapshot() -> ac.u8:
        return rows[index].left

    @ac.rule
    def check() -> None:
        assert healthy == 1, "table update failed"

    return rows, write_left, snapshot


@ac.struct
class TablePacket:
    rows: ac.table[3, TableRow]
    marker: ac.u8


@ac.module
def MakeTablePacket(value: ac.u8) -> TablePacket:
    return TablePacket(
        rows=(
            TableRow(left=value, right=2),
            TableRow(left=3, right=4),
            TableRow(left=5, right=value + 1),
        ),
        marker=value + 2,
    )


@ac.module
def BufferTablePacket() -> ac.u8:
    pending = ac.queue[TablePacket](depth=1, latency=2)

    @ac.method
    def put(packet: TablePacket) -> None:
        pending.push(packet)

    @ac.method
    def get() -> TablePacket:
        return pending.pop()

    return 0


@ac.module
def TableQueue(value: ac.u8, issue: ac.u1, take: ac.u1) -> TablePacket:
    packet = MakeTablePacket(value)
    buffer = BufferTablePacket()

    @ac.rule(guard=lambda: issue)
    def enqueue() -> None:
        buffer.put(packet)

    @ac.rule(guard=lambda: take)
    def dequeue() -> TablePacket:
        return buffer.get()

    return dequeue


@ac.module
def Accumulator() -> ac.u8:
    saved: ac.u8 = 3

    @ac.method
    def add(amount: ac.u8) -> None:
        saved = saved + amount

    return saved


@ac.module
def FeedbackCell(row: TableRow, enable: ac.u1) -> TableRow:
    accumulator = Accumulator()

    @ac.rule(guard=lambda: enable)
    def accumulate() -> None:
        accumulator.add(row.left)

    return TableRow(left=accumulator, right=row.right + 1)
