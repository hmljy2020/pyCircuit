"""Per-flow durable ROB handoff coordinator (Decision 0222)."""

# ruff: noqa: F841
import agentic_circuit as ac

from designs.davincioo.contracts.spe import (
    HANDOFF_OWNER_BROB,
    HANDOFF_OWNER_MPQ,
    BlockKey,
    BrobHandoffAck,
    EpochKey,
    FlowKey,
    HandoffDiagnostic,
    InstKey,
    MpqHandoffAck,
    RobEvent,
    RobEventKind,
    RobHandoff,
    RobKey,
    ScalarRenameHistory,
)


@ac.inline
def increment_saturated_u16(value: ac.u16) -> ac.u16:
    return value + 1 if value != 65535 else value


def next_diagnostic_state(
    valid: bool,
    count: ac.u16,
    overflow: bool,
    dropped: ac.u16,
) -> tuple[ac.u16, bool, ac.u16]:
    if valid:
        count = increment_saturated_u16(count)
        overflow = True
        dropped = increment_saturated_u16(dropped)
    else:
        count = 1
    return count, overflow, dropped


def same_handoff_identity(
    epoch: EpochKey,
    inst: InstKey,
    block: BlockKey,
    rob: RobKey,
    retained: RobEvent,
) -> bool:
    return (
        epoch == retained.epoch
        and inst == retained.inst
        and block == retained.block
        and rob == retained.rob
    )


@ac.module
def cmt(
    microcommit: RobEvent,
    mpq_ack: MpqHandoffAck,
    brob_ack: BrobHandoffAck,
    *,
    core_id: ac.const[int] = 0,
    pe_id: ac.const[int] = 0,
    stid: ac.const[int] = 0,
    launch_generation: ac.const[int] = 0,
) -> tuple[RobEvent, RobEvent, RobHandoff, HandoffDiagnostic]:
    busy: bool = False
    retained: RobEvent = 0
    mpq_sent: bool = False
    brob_sent: bool = False
    brob_done: bool = False
    histories_acked: ac.u4 = 0
    histories: list[ScalarRenameHistory] = [0] * 16
    diagnostics: list[HandoffDiagnostic] = [0] * 3
    diagnostic_overflow: list[bool] = [False] * 3
    diagnostic_dropped: list[ac.u16] = [0] * 3

    # NDF: DOC-DAV-SPE-OOO-CMT-EMIT-DIAGNOSTIC
    @ac.rule
    def emit_diagnostic():
        nonlocal diagnostics
        found = ac.find(diagnostics, where=lambda row: row.valid)
        if found.valid:
            diagnostics[found.index] = found.value.with_fields(valid=False)
            return found.value

    # NDF: DOC-DAV-SPE-OOO-CMT-RELEASE
    @ac.rule
    def release():
        nonlocal busy, retained, mpq_sent, brob_sent, brob_done, histories_acked
        need_mpq = (retained.handoff_required_mask & HANDOFF_OWNER_MPQ) != 0
        need_brob = (retained.handoff_required_mask & HANDOFF_OWNER_BROB) != 0
        if (
            busy
            and (not need_mpq or mpq_sent)
            and histories_acked == retained.mpq_history_record_count
            and (not need_brob or (brob_sent and brob_done))
        ):
            result = RobHandoff(
                epoch=retained.epoch,
                inst=retained.inst,
                block=retained.block,
                rob=retained.rob,
                required_owner_mask=retained.handoff_required_mask,
                received_owner_mask=retained.handoff_required_mask,
                mpq_histories_required=retained.mpq_history_record_count,
                mpq_histories_acked=histories_acked,
                valid=True,
                durable=True,
            )
            busy = False
            return result

    # NDF: DOC-DAV-SPE-OOO-CMT-SEND-MPQ
    @ac.rule
    def send_mpq():
        nonlocal busy, retained, mpq_sent
        if (
            busy
            and not mpq_sent
            and (retained.handoff_required_mask & HANDOFF_OWNER_MPQ) != 0
        ):
            mpq_sent = True
            return retained

    # NDF: DOC-DAV-SPE-OOO-CMT-SEND-BROB
    @ac.rule
    def send_brob():
        nonlocal busy, retained, brob_sent
        if (
            busy
            and not brob_sent
            and (retained.handoff_required_mask & HANDOFF_OWNER_BROB) != 0
        ):
            brob_sent = True
            return retained

    # NDF: DOC-DAV-SPE-OOO-CMT-ACKNOWLEDGE-MPQ
    @ac.rule
    def acknowledge_mpq(response):
        nonlocal busy, retained, mpq_sent, histories_acked, histories
        nonlocal diagnostics, diagnostic_overflow, diagnostic_dropped
        request = response.request
        history = response.history
        seen = ac.find(
            histories,
            where=lambda row: (
                row.valid
                and same_handoff_identity(
                    row.epoch, row.inst, row.block, row.rob, retained
                )
                and row.history_sequence == history.history_sequence
            ),
        )
        unsolicited = (
            not busy
            or not mpq_sent
            or (retained.handoff_required_mask & HANDOFF_OWNER_MPQ) == 0
        )
        identity_mismatch = not (
            same_handoff_identity(
                request.epoch, request.inst, request.block, request.rob, retained
            )
            and same_handoff_identity(
                history.epoch, history.inst, history.block, history.rob, retained
            )
        )
        kind_mismatch = request.kind != RobEventKind.MICROCOMMIT
        invalid_response = (
            not response.valid
            or not response.accepted
            or response.stale
            or not request.valid
            or not history.valid
            or request != retained
        )
        durability_missing = not history.durable
        duplicate = busy and not identity_mismatch and seen.valid
        capacity_error = (
            busy
            and histories_acked >= retained.mpq_history_record_count
            and not duplicate
        )
        bad = (
            unsolicited
            or identity_mismatch
            or kind_mismatch
            or invalid_response
            or durability_missing
            or duplicate
            or capacity_error
        )
        if bad:
            old = diagnostics[1]
            amount, overflow, dropped = next_diagnostic_state(
                old.valid,
                old.coalesced_count,
                diagnostic_overflow[1],
                diagnostic_dropped[1],
            )
            diagnostic_overflow[1] = overflow
            diagnostic_dropped[1] = dropped
            diagnostics[1] = HandoffDiagnostic(
                owner_mask=HANDOFF_OWNER_MPQ,
                epoch=request.epoch,
                inst=request.inst,
                block=request.block,
                rob=request.rob,
                history_sequence=history.history_sequence,
                coalesced_count=amount,
                valid=True,
                duplicate=duplicate,
                unsolicited=unsolicited,
                identity_mismatch=identity_mismatch,
                kind_mismatch=kind_mismatch,
                invalid_response=invalid_response,
                durability_missing=durability_missing,
                capacity_error=capacity_error,
            )
        else:
            histories[histories_acked] = history
            histories_acked = histories_acked + 1

    # NDF: DOC-DAV-SPE-OOO-CMT-ACKNOWLEDGE-BROB
    @ac.rule
    def acknowledge_brob(response):
        nonlocal busy, retained, brob_sent, brob_done
        nonlocal diagnostics, diagnostic_overflow, diagnostic_dropped
        unsolicited = (
            not busy
            or not brob_sent
            or (retained.handoff_required_mask & HANDOFF_OWNER_BROB) == 0
        )
        identity_mismatch = not same_handoff_identity(
            response.epoch, response.inst, response.block, response.rob, retained
        )
        duplicate = busy and not identity_mismatch and brob_done
        invalid_response = not response.valid
        durability_missing = not response.durable
        if (
            unsolicited
            or identity_mismatch
            or duplicate
            or invalid_response
            or durability_missing
        ):
            old = diagnostics[2]
            amount, overflow, dropped = next_diagnostic_state(
                old.valid,
                old.coalesced_count,
                diagnostic_overflow[2],
                diagnostic_dropped[2],
            )
            diagnostic_overflow[2] = overflow
            diagnostic_dropped[2] = dropped
            diagnostics[2] = HandoffDiagnostic(
                owner_mask=HANDOFF_OWNER_BROB,
                epoch=response.epoch,
                inst=response.inst,
                block=response.block,
                rob=response.rob,
                history_sequence=0,
                coalesced_count=amount,
                valid=True,
                duplicate=duplicate,
                unsolicited=unsolicited,
                identity_mismatch=identity_mismatch,
                kind_mismatch=False,
                invalid_response=invalid_response,
                durability_missing=durability_missing,
                capacity_error=False,
            )
        else:
            brob_done = True

    # NDF: DOC-DAV-SPE-OOO-CMT-ACCEPT
    @ac.rule
    def accept(request, core_id, pe_id, stid, launch_generation):
        nonlocal histories
        nonlocal busy, retained, mpq_sent, brob_sent, brob_done, histories_acked
        nonlocal diagnostics, diagnostic_overflow, diagnostic_dropped
        flow = FlowKey(
            core_id=core_id, pe_id=pe_id, stid=stid, launch_generation=launch_generation
        )
        identity_mismatch = (
            request.epoch.flow != flow
            or request.inst.flow != flow
            or request.block.flow != flow
            or request.rob.flow != flow
        )
        kind_mismatch = request.kind != RobEventKind.MICROCOMMIT
        invalid_response = (
            not request.valid or not request.done or not request.handoff_pending
        )
        need_mpq = (request.handoff_required_mask & HANDOFF_OWNER_MPQ) != 0
        capacity_error = not need_mpq and request.mpq_history_record_count != 0
        bad = identity_mismatch or kind_mismatch or invalid_response or capacity_error
        if not busy or bad:
            if bad:
                old = diagnostics[0]
                amount, overflow, dropped = next_diagnostic_state(
                    old.valid,
                    old.coalesced_count,
                    diagnostic_overflow[0],
                    diagnostic_dropped[0],
                )
                diagnostic_overflow[0] = overflow
                diagnostic_dropped[0] = dropped
                diagnostics[0] = HandoffDiagnostic(
                    owner_mask=0,
                    epoch=request.epoch,
                    inst=request.inst,
                    block=request.block,
                    rob=request.rob,
                    history_sequence=0,
                    coalesced_count=amount,
                    valid=True,
                    duplicate=False,
                    unsolicited=False,
                    identity_mismatch=identity_mismatch,
                    kind_mismatch=kind_mismatch,
                    invalid_response=invalid_response,
                    durability_missing=False,
                    capacity_error=capacity_error,
                )
            else:
                retained = request
                busy = True
                mpq_sent = False
                brob_sent = False
                brob_done = False
                histories_acked = 0
                for i in range(16):
                    histories[i].valid = False

    diagnostic = emit_diagnostic()
    rob_handoff = release()
    mpq_request = send_mpq()
    brob_request = send_brob()
    acknowledge_mpq(mpq_ack)
    acknowledge_brob(brob_ack)
    accept(microcommit, core_id, pe_id, stid, launch_generation)
    return mpq_request, brob_request, rob_handoff, diagnostic


@ac.system
def cmt_system(
    microcommit: RobEvent, mpq_ack: MpqHandoffAck, brob_ack: BrobHandoffAck
) -> tuple[RobEvent, RobEvent, RobHandoff, HandoffDiagnostic]:
    a, b, c, d = cmt(microcommit, mpq_ack, brob_ack)
    return a, b, c, d


@ac.system
def dual_cmt_system(
    left_commit: RobEvent,
    left_mpq: MpqHandoffAck,
    left_brob: BrobHandoffAck,
    right_commit: RobEvent,
    right_mpq: MpqHandoffAck,
    right_brob: BrobHandoffAck,
) -> tuple[
    RobEvent,
    RobEvent,
    RobHandoff,
    HandoffDiagnostic,
    RobEvent,
    RobEvent,
    RobHandoff,
    HandoffDiagnostic,
]:
    a, b, c, d = cmt(left_commit, left_mpq, left_brob, stid=0)
    e, f, g, h = cmt(right_commit, right_mpq, right_brob, stid=1)
    return a, b, c, d, e, f, g, h
