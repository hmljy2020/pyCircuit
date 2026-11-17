"""Write-heavy finite loop with scalar RHS and full-domain dynamic selectors."""

import pycircuit as ac


def update(
    values: ac.table[1024, ac.u13], selector: ac.u10, value: ac.u8
) -> ac.table[1024, ac.u13]:
    result = values
    for index in range(8):
        result[selector] = value
        result[index] = index
    return result


@ac.module
def Updated(selector: ac.u10, value: ac.u8) -> tuple[ac.u13, ac.u13, ac.u13]:
    values = ac.table[1024, ac.u13](init=0)
    result = update(values, selector, value)
    return result[selector], result[7], result[8]


@ac.system
def Top():
    far, last, untouched = Updated(900, 255)
    overlap, overlap_last, overlap_untouched = Updated(0, 173)

    @ac.rule
    def check():
        assert far == 255, "dynamic scalar extension"
        assert last == 7, "ordered constant writes"
        assert untouched == 0, "untouched lane"
        assert overlap == 173, "later dynamic write replaces earlier constant write"
        assert overlap_last == 7, "last constant write"
        assert overlap_untouched == 0, "overlap leaves untouched lane"
