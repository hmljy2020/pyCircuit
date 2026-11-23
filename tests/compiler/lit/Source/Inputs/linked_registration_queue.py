"""Add two queued inputs atomically and enqueue their sum."""

import pycircuit as ac


@ac.module
def QueueAdd(left: ac.u16, right: ac.u16) -> ac.u16:  # noqa: N802
    left_input = ac.queue[ac.u16](depth=1, latency=1)
    right_input = ac.queue[ac.u16](depth=1, latency=1)
    output = ac.queue[ac.u16](depth=1, latency=1)

    @ac.rule
    def send_left() -> None:
        left_input.push(left)

    @ac.rule
    def send_right() -> None:
        right_input.push(right)

    @ac.rule
    def add() -> None:
        a = left_input.pop()
        b = right_input.pop()
        output.push(a + b)

    @ac.rule
    def receive() -> ac.u16:
        return output.pop()

    return receive
