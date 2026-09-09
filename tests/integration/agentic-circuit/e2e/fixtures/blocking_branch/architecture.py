import agentic_circuit as ac


@ac.struct
class Command:
    bad: bool
    value: ac.u8


@ac.rule
def accept(busy, retained, diagnostics, entries, command):
    if not busy or command.bad:
        if command.bad:
            diagnostics = diagnostics + 1
        else:
            busy = True
            retained = command.value
            entries[0] = command.value
            if command.value != 0:
                entries[1] = command.value + 1
            else:
                entries[1] = 0


@ac.system
def blocking_branch(command: Command):
    busy: bool = False
    retained: ac.u8 = 0
    diagnostics: ac.u8 = 0
    entries: list[ac.u8] = [0] * 2
    accept(busy, retained, diagnostics, entries, command)
