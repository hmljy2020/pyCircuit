"""Per-flow ROB with durable, ordered CMT handoff (Decision 0222)."""

# ruff: noqa: F841

from __future__ import annotations

import agentic_circuit as ac

from designs.davincioo.contracts.spe import (
    FlowKey,
    RobCompletion,
    RobEvent,
    RobEventKind,
    RobHandoff,
    RobKey,
    TerminalStatus,
)


def accepts_flush(
    request: RobEvent,
    flow: FlowKey,
    epoch: ac.u16,
    recovering: bool,
) -> bool:
    return (
        request.valid
        and request.kind == RobEventKind.FLUSH
        and request.epoch.flow == flow
        and request.epoch.recovery_epoch == epoch
        and not recovering
    )


def accepts_handoff(
    request: RobHandoff,
    old: RobEvent,
    pending: bool,
) -> bool:
    return (
        request.valid
        and request.durable
        and pending
        and request.epoch == old.epoch
        and request.inst == old.inst
        and request.block == old.block
        and request.rob == old.rob
        and request.required_owner_mask == old.handoff_required_mask
        and (request.received_owner_mask & old.handoff_required_mask)
        == old.handoff_required_mask
        and request.mpq_histories_required == old.mpq_history_record_count
        and request.mpq_histories_acked == old.mpq_history_record_count
    )


def accepts_completion(
    response: RobCompletion,
    old: RobEvent,
    epoch: ac.u16,
    recovering: bool,
) -> bool:
    identity = response.attempt.identity
    return (
        response.valid
        and not recovering
        and old.valid
        and not old.done
        and not old.handoff_pending
        and old.epoch.recovery_epoch == epoch
        and identity.epoch == old.epoch
        and identity.inst == old.inst
        and identity.block == old.block
        and identity.rob == old.rob
    )


def accepts_allocation(
    request: RobEvent,
    flow: FlowKey,
    epoch: ac.u16,
    count: ac.u5,
    recovering: bool,
) -> bool:
    return (
        count < 16
        and not recovering
        and request.valid
        and request.kind == RobEventKind.ALLOCATE
        and request.epoch.flow == flow
        and request.inst.flow == flow
        and request.block.flow == flow
        and request.rob.flow == flow
        and request.epoch.recovery_epoch == epoch
    )


@ac.module
def rob(
    flush_request: RobEvent,
    allocate_request: RobEvent,
    completion: RobCompletion,
    handoff_ack: RobHandoff,
    *,
    core_id: ac.const[int] = 0,
    pe_id: ac.const[int] = 0,
    stid: ac.const[int] = 0,
    launch_generation: ac.const[int] = 0,
) -> tuple[RobEvent, RobEvent]:
    head: ac.u4 = 0
    tail: ac.u4 = 0
    count: ac.u5 = 0
    epoch: ac.u16 = 0
    pending: bool = False
    recovering: bool = False
    entries: list[RobEvent] = [0] * 16

    @ac.rule
    def recover(request, core_id, pe_id, stid, launch_generation):
        nonlocal head, tail, count, epoch, recovering, pending
        flow = FlowKey(
            core_id=core_id, pe_id=pe_id, stid=stid, launch_generation=launch_generation
        )
        if not accepts_flush(request, flow, epoch, recovering):
            return
        epoch = epoch + 1
        recovering = pending
        count = 1 if pending else 0
        head = head if pending else tail

    @ac.rule
    def acknowledge(request):
        nonlocal head, tail, count, recovering, pending, entries
        old = entries[head]
        if not accepts_handoff(request, old, pending):
            return
        entries[head] = old.with_fields(valid=False, handoff_pending=False)
        head = tail if recovering else head + 1
        count = count - 1
        pending = False
        recovering = False

    @ac.rule
    def complete(response):
        nonlocal epoch, recovering, entries
        identity = response.attempt.identity
        old = entries[identity.rob.slot]
        if not accepts_completion(response, old, epoch, recovering):
            return
        entries[identity.rob.slot] = old.with_fields(
            result=response.result,
            result_valid=response.result_valid,
            status=response.status,
            fault_code=response.fault_code,
            fault_arg0=response.fault_arg0,
            fault_bi=response.fault_bi,
            fault_valid=response.fault_valid,
            done=True,
        )

    @ac.rule
    def handoff():
        nonlocal head, count, epoch, recovering, pending, entries
        old = entries[head]
        if (
            count != 0
            and not recovering
            and not pending
            and old.valid
            and old.done
            and old.epoch.recovery_epoch == epoch
        ):
            result = old.with_fields(
                kind=RobEventKind.MICROCOMMIT, handoff_pending=True
            )
            entries[head] = result
            pending = True
            return result

    # NDF: DOC-DAV-SPE-OOO-ROB-ALLOCATE
    @ac.rule
    def allocate(request, core_id, pe_id, stid, launch_generation):
        nonlocal tail, count, epoch, recovering, entries
        flow = FlowKey(
            core_id=core_id, pe_id=pe_id, stid=stid, launch_generation=launch_generation
        )
        old = entries[tail]
        if accepts_allocation(request, flow, epoch, count, recovering):
            result = request.with_fields(
                kind=RobEventKind.ALLOCATED,
                rob=RobKey(flow=flow, slot=tail, generation=old.rob.generation + 1),
                result=0,
                result_valid=False,
                status=TerminalStatus.VALUE,
                fault_code=0,
                fault_arg0=0,
                fault_bi=False,
                fault_valid=False,
                done=False,
                handoff_pending=False,
            )
            entries[tail] = result
            tail = tail + 1
            count = count + 1
            return result

    recover(flush_request, core_id, pe_id, stid, launch_generation)
    acknowledge(handoff_ack)
    complete(completion)
    committed = handoff()
    allocated = allocate(allocate_request, core_id, pe_id, stid, launch_generation)
    return allocated, committed


@ac.system
def rob_system(
    flush_request: RobEvent,
    allocate_request: RobEvent,
    completion: RobCompletion,
    handoff_ack: RobHandoff,
) -> tuple[RobEvent, RobEvent]:
    allocated, committed = rob(flush_request, allocate_request, completion, handoff_ack)
    return allocated, committed


@ac.system
def dual_rob_system(
    left_flush: RobEvent,
    left_allocate: RobEvent,
    left_completion: RobCompletion,
    left_ack: RobHandoff,
    right_flush: RobEvent,
    right_allocate: RobEvent,
    right_completion: RobCompletion,
    right_ack: RobHandoff,
) -> tuple[RobEvent, RobEvent, RobEvent, RobEvent]:
    left_allocated, left_committed = rob(
        left_flush, left_allocate, left_completion, left_ack, stid=0
    )
    right_allocated, right_committed = rob(
        right_flush, right_allocate, right_completion, right_ack, stid=1
    )
    return left_allocated, left_committed, right_allocated, right_committed
