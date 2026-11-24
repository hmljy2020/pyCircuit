# ruff: noqa: F823, F841 -- Method state proposals are captured by the compiler.
"""Exercise instance-relative guards through nested Method calls."""

import pycircuit as ac


@ac.module
def Target():
    ceiling: ac.u8 = 20

    @ac.method(guard=lambda proposed, replacement: proposed < ceiling)
    def accept(proposed: ac.u8, replacement: ac.u8) -> None:
        ceiling = replacement

    @ac.method
    def read() -> ac.u8:
        return ceiling


@ac.module
def Relay():
    left = Target()
    right = Target()

    @ac.method(guard=lambda value, select: value != 0)
    def route(value: ac.u8, select: ac.u1) -> None:
        if select:
            left.accept(value + 1, value)
        else:
            right.accept(value + 2, value)

    @ac.method
    def after_read() -> None:
        left.accept(right.read(), 0)


@ac.system
def GuardSystem():
    relay = Relay()
    value: ac.u8 = 4
    select: ac.u1 = 1

    @ac.rule(guard=lambda: value < 20)
    def drive() -> None:
        relay.route(value, select)


@ac.system
def PriorResultSystem():
    relay = Relay()

    @ac.rule
    def try_prior_result() -> None:
        relay.after_read()
