from typing import Annotated

from pycircuit import module


def calculate(a: Annotated[int, range(256)]) -> Annotated[int, range(65536)]:
    base = a + 3
    product = base * 3
    result = product - 3
    return result


@module
def Transport(a: Annotated[int, range(256)]) -> {
    "calculated": Annotated[int, range(65536)],
    "shifted": Annotated[int, range(256)],
    "oversized": Annotated[int, range(256)],
}:
    calculated = calculate(a)
    shifted = a >> 3
    oversized = a >> 80
    return {"calculated": calculated, "shifted": shifted, "oversized": oversized}
