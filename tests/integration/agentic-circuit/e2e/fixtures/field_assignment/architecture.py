import agentic_circuit as ac


@ac.struct
class Entry:
    x: ac.u8
    valid: bool


@ac.struct
class Command:
    index: ac.u1
    value: ac.u8
    enabled: bool


@ac.struct
class Report:
    local: Entry
    saved: Entry
    before: Entry
    after: Entry
    other: Entry


@ac.module
def field_module(command: Command) -> Report:
    retained: Entry = 0
    entries: list[Entry] = [0] * 2

    @ac.rule
    def update(command):
        nonlocal retained, entries
        local = retained
        saved = local
        local.x = command.value
        local.valid = True
        before = entries[command.index]
        retained.x = command.value
        if command.enabled:
            retained.valid = True
        else:
            retained.valid = False
        index = command.index
        entries[command.index].x = command.value
        entries[index].valid = command.enabled
        after = entries[command.index]
        index = index ^ 1
        other = entries[index]
        return Report(local=local, saved=saved, before=before, after=after, other=other)

    result = update(command)
    return result


@ac.system
def fields(command: Command) -> Report:
    result = field_module(command)
    return result
