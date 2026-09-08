#!/usr/bin/env python3
"""Lift a 4.3.4 client packet reader out of IDA pseudocode into Wire elements.

Usage:
    python lift_client_reader.py <Wow-64.c> <reader-function> [--control <table>]
    python lift_client_reader.py <Wow-64.c> <reader-function> --raw

The decompiled client is not in the tree; pass its path (build 15595's
Wow-64.c). The lifted element lists are what gets committed, in
gen_movement_layouts.py's LIFTED table.

Either mode exits 1 when the lift met a callee the lifter has no name for --
the element list it printed is then short by whatever that callee read, so it
must not be copied into LIFTED -- and --control also when the lift and the
registry table differ.

WHY. Three of the Cataclysm Preservation Project's movement tables read gated
fields (timestamp, pitch, fall, transport) without carrying any of the presence
gates, so P1-A excluded them: a reader cannot know from such a table what to
read. The client's own readers do carry the gates, so they are the better
source -- if we can read them mechanically. That is this tool.

WHAT IT RECOGNISES. A reader is `sub_X(a1, a2)`: a1 is the message struct, a2
is the CDataStore. Every wire read ends up stored into a fixed offset of a1,
and the offset is what names the element (OFFSETS below). The read kinds:

  u32    `sub_1405A7DD0(store, &v)` then `*(_DWORD *)(a1 + OFF) = v;`
         -- four bytes (a float or a uint32).
  u8     `sub_1405A7CE0(store, &v)` then `*(_BYTE *)(a1 + OFF) = v;`
         -- one whole byte, read aligned (the transport seat).
  gbyte  `sub_1405A7CE0(store, &v)` then `*(_BYTE *)(a1 + OFF) ^= v;`
         -- one packed-guid byte, xored into the slot its presence bit set.
  bit    one bit of the bit stream. The decompiler keeps the bit cursor in a
         shift register: a fetched byte is walked by repeated `2 *`, and the
         bit is `... >> 7` (with `!= 0` / `== 0` skins; `== 0` is an inverted
         gate, and the `if (x >> 7) f = 0; else f = 1.0;` form is the same bit
         written as a sentinel float). `if (n == 8) { refetch; n = 0; }` blocks
         are the refill and carry no element.
  bits   an inline multi-bit field: one store whose right-hand side ORs several
         shifted pieces together -- the 12-bit movement flags2, which the
         decompiler inlines rather than calling.
  call   a sub-reader; HELPERS names it.

HELPERS (what the called sub-readers turned out to be):
  sub_140342E40  the 30-bit movement flags: `for (i = 22; i > -2; i -= 8)`
                 pulls three bytes into bits 6..29, then sub_140342300 pulls
                 the low six bits (its `v7 >> 2` after one byte).   -> Flags
  sub_140342300  a 6-bit read; only ever seen as sub_140342E40's tail.
  sub_1405A7CE0  CDataStore: fetch one byte.
  sub_1405A7DD0  CDataStore: fetch four bytes.
  sub_1400430B0  CDataStore: read a packed guid (families only).

THE OFFSET MAP was learned from SMSG_MOVE_UPDATE_RUN_SPEED's reader
(sub_14038AE90) against the registry's MovementUpdateRunSpeed, which is
CPP-sourced and which P1-B's real-client golden replayed line-exact -- so it is
ground truth, and `--control MovementUpdateRunSpeed` proving the lift equal to
it is what makes the other lifts trustworthy.

Offsets are relative to the client MovementInfo the message embeds, not to the
message: each update message is `vtable, ..., a5 at 24` and then a MovementInfo
whose start its constructor names (`sub_1403439D0(a1 + BASE)`), and BASE is not
the same for every message -- 32 for the run-speed and knockback updates, 40
for the run-back-speed and walk-speed ones, whose own float sits at 32 instead
of after the MovementInfo. The lifter finds BASE itself: the eight guid-byte
slots are the only run of eight consecutive one-byte reads, and their lowest
offset is the MovementInfo's start (--base overrides). Relative offsets:

      0..7  guid bytes 0..7        as bits: the guid presence bits
      8     movement flags         as a bit: HasMovementFlags (inverted)
     12     movement flags2        as a bit: HasMovementFlags2 (inverted)
     16     timestamp              as a bit: HasTimestamp (inverted)
     20/24/28  position x / y / z
     32     orientation            as a bit: HasOrientation (sentinel float)
     40     HasTransportData
     48..55 transport guid bytes 0..7   as bits: their presence bits
     56/60/64/68  transport position x / y / z / o
     72     transport seat         (a whole aligned byte, not a guid byte)
     76     transport time
     80     HasTransportTime2
     84     transport time2
     88     HasVehicleId
     92     transport vehicle id
     96     pitch                  as a bit: HasPitch (sentinel float)
    100     HasFallData
    104     fall time
    108     fall vertical speed
    112     HasFallDirection
    116     fall cos angle         (Wire's name; see gen's ELEMENT_NAMES)
    120     fall sin angle
    124     fall horizontal speed
    128     spline elevation       as a bit: HasSplineElevation (sentinel float)
    132     HasSpline
    133     the always-present unknown bit
    136     the packet's own float (a speed), when it follows the MovementInfo
     -8     the packet's own float, when it precedes it

Offsets outside the map print raw (`u32@176`), which is also what --raw forces
everywhere: the family readers (set-active-mover, knockback, teleport) do not
fill a MovementInfo, so their offsets mean nothing here and only the read order
is wanted.

HOW FAR IT WAS CHECKED. Beyond the control, the lifter was run against every
registry table whose reader the dispatch table resolves -- 43 rows, of which
three are the lifted tables themselves. Of the other 40: one (the control) is
exact; 31 agree on every element and differ only where the lift has no name for
a slot outside the MovementInfo (a small packet's own counter or float,
`u32@40`); one agrees but for CPP's per-packet naming of the bit at +133
(MSEZeroBit vs MSEHasHeightChangeFailed); one (SMSG_MOVE_SET_COLLISION_HGT, a
non-status packet) stops the lifter with an explicit error rather than a guess;
four agree except that the client reads the fall angle pair the other way round
from the control; and two disagree structurally, in both of which the client is
plainly right and CPP's table wrong.

The four with the fall pair the other way round are MovementUpdateFlightSpeed,
MovementUpdateSwimSpeed, MoveUpdateSwimBackSpeed and
MovementUpdateCollisionHeight. "The other way round" means this: the two angles
live at fixed struct offsets, +116 and +120, and the control fixes which is
which (its reader takes +120 where CPP's run-speed table, after the P1-B
rename, says FallSinAngle). These four readers take +116 where their CPP table
says FallSinAngle and +120 where it says FallCosAngle -- the same two slots, the
opposite labels -- so those four tables and the control cannot both be right.
The control is the one a real-client golden replayed, so it is those four
tables, still CPP-sourced and live in the registry, that need a witness and a
fix; this tool only reports it.

The two that disagree structurally:

  MoveUpdateFlightBackSpeed -- CPP misses the +132 bit the reader takes first
      of all, and calls the +80 bit (HasTransportTime2) MSEOneBit while
      putting a HasTransportTime2 four bits later.
  MoveSetRunBackSpeed -- CPP has guid bits 4 and 5 the other way round; the
      shift register is unambiguous (`v9 = v4; v4 *= 2;` then `v9 >>= 7`
      three statements later, so v9's bit comes first).

The same sweep against the hand-written family codecs (--raw, since those
messages hold no MovementInfo) agreed with SMSG_MOVE_SET_ACTIVE_MOVER's and
SMSG_MOVE_KNOCK_BACK's mask and byte orders exactly, and found one difference
in SMSG_MOVE_TELEPORT: the vehicle seat is one byte, which TeleportCodec now
writes and reads as one.
"""
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# MaNGOS is a full featured server for World of Warcraft, supporting
# the following clients: 1.12.x, 2.4.3, 3.3.5a, 4.3.4a and 5.4.8
#
# Copyright (C) 2005-2026 MaNGOS <https://www.getmangos.eu>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <https://www.gnu.org/licenses/>.
#
# World of Warcraft, and all World of Warcraft or Warcraft art, images,
# and lore are copyrighted by Blizzard Entertainment, Inc.
#
import io, os, re, sys

FETCH_U8 = "sub_1405A7CE0"
FETCH_U32 = "sub_1405A7DD0"
FETCH_GUID = "sub_1400430B0"

HELPERS = {
    "sub_140342E40": "Flags",       # the 30-bit movement flags
}

# (kind, offset relative to the embedded MovementInfo) -> Wire::Element, from
# the control. See the module docstring for what each offset turned out to be.
OFFSETS = {}
for i in range(8):
    OFFSETS[("bit", i)] = "GuidBit%d" % i
    OFFSETS[("gbyte", i)] = "GuidByte%d" % i
    OFFSETS[("bit", 48 + i)] = "TransportGuidBit%d" % i
    OFFSETS[("gbyte", 48 + i)] = "TransportGuidByte%d" % i
OFFSETS.update({
    ("bit", 8): "HasMovementFlags",   ("bits", 8): "Flags",
    ("bit", 12): "HasMovementFlags2", ("bits", 12): "Flags2",
    ("bit", 16): "HasTimestamp",      ("u32", 16): "Timestamp",
    ("u32", 20): "PositionX", ("u32", 24): "PositionY", ("u32", 28): "PositionZ",
    ("bit", 32): "HasOrientation",    ("u32", 32): "PositionO",
    ("bit", 40): "HasTransportData",
    ("u32", 56): "TransportPositionX", ("u32", 60): "TransportPositionY",
    ("u32", 64): "TransportPositionZ", ("u32", 68): "TransportPositionO",
    ("u8", 72): "TransportSeat",
    ("u32", 76): "TransportTime",
    ("bit", 80): "HasTransportTime2", ("u32", 84): "TransportTime2",
    ("bit", 88): "HasVehicleId",      ("u32", 92): "TransportVehicleId",
    ("bit", 96): "HasPitch",          ("u32", 96): "Pitch",
    ("bit", 100): "HasFallData",
    ("u32", 104): "FallTime",         ("u32", 108): "FallVerticalSpeed",
    ("bit", 112): "HasFallDirection",
    ("u32", 116): "FallCosAngle",     ("u32", 120): "FallSinAngle",
    ("u32", 124): "FallHorizontalSpeed",
    ("bit", 128): "HasSplineElevation", ("u32", 128): "SplineElevation",
    ("bit", 132): "HasSpline",
    ("bit", 133): "HasUnknownBit",
    ("u32", 136): "ExtraFloat",       # the packet's float, after the MovementInfo
    ("u32", -8): "ExtraFloat",        # ... or before it
})

_STORE_PTR = re.compile(r"^\*\(\s*(?:float|_BYTE|_WORD|_DWORD|_QWORD|unsigned __int8|__int64)\s*\*\s*\)"
                        r"\(\s*a1\s*\+\s*(\d+)\s*\)\s*(\^?)=\s*(.*)$")
# the same store where the decompiler typed a1 as a byte pointer: `a1[37] = ...`
_STORE_IDX = re.compile(r"^a1\[(\d+)\]\s*(\^?)=\s*(.*)$")

CALL = re.compile(r"(sub_[0-9A-Fa-f]+)\s*\(")
BIT = re.compile(r">>\s*7\b")
SIMPLE = re.compile(r"^\(?\s*(?:\(\s*(?:_BYTE|_WORD|_DWORD|unsigned __int8|unsigned int)\s*\)\s*)?"
                    r"([A-Za-z_][A-Za-z_0-9]*)\s*\)?$")
ZEROED = re.compile(r"^(?:LOBYTE|LOWORD|LODWORD)?\(?([A-Za-z_][A-Za-z_0-9]*)\)?\s*=\s*0$")
VARASSIGN = re.compile(r"^(?:LOBYTE|LOWORD|LODWORD)\s*\(\s*([A-Za-z_][A-Za-z_0-9]*)\s*\)\s*=\s*(.*)$"
                       r"|^([A-Za-z_][A-Za-z_0-9]*)\s*=\s*(.*)$")


def STORE_match(s):
    """Groups (offset, "^" or "", right-hand side) if `s` stores into a1."""
    return _STORE_PTR.match(s) or _STORE_IDX.match(s)


def function_body(path, name):
    """The text of `name`'s definition: the `<type> __fastcall name(...)` line
    that is followed by `{`, then to the matching `}`."""
    src = io.open(path, encoding="utf-8", errors="replace").read()
    pat = re.compile(r"^[^\n;{}]*\b%s\s*\([^\n;]*\)\s*\n\{" % re.escape(name), re.M)
    hits = list(pat.finditer(src))
    if len(hits) != 1:
        sys.exit("%s: %d definitions found" % (name, len(hits)))
    i = src.index("{", hits[0].end() - 1)
    depth, j = 0, i
    while j < len(src):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                break
        j += 1
    return src[i:j + 1]


def statements(body):
    """The body's logical statements, comments and declarations dropped."""
    out, buf = [], ""
    for raw in body.split("\n"):
        line = re.sub(r"//.*", "", raw).strip()
        if not line:
            continue
        buf = (buf + " " + line).strip() if buf else line
        if buf.count("(") != buf.count(")"):
            continue                       # a wrapped expression
        if not (buf.endswith((";", "{", "}"))
                or re.match(r"^(?:if|while|for)\s*\(.*\)$", buf)
                or buf in ("else", "do")):
            continue                       # a wrapped expression, balanced so far
        out.append(buf.rstrip(";").strip())
        buf = ""
    # declarations: `<type> vN` with no assignment and no call
    decl = re.compile(r"^[A-Za-z_][A-Za-z_0-9 *]*\s[A-Za-z_][A-Za-z_0-9]*(?:\[\d+\])?$")
    return [s for s in out if not (decl.match(s) and "=" not in s and "(" not in s)]


def lift(path, name, unknown=None):
    """The reader's reads, in wire order: a list of (kind, offset) pairs, plus
    the pseudo-kind ("flush", None) where the bit section ends. Any callee the
    lifter has no name for is warned about and added to `unknown` (a set the
    caller may pass in to act on).

    Neither the order the bits are stored nor the order they are textually
    taken is the wire order: the decompiler both sinks a store past the next
    bit's and reorders the takes themselves (`v6 >>= 7` two statements after
    the register has moved on). What survives is the shift register's
    arithmetic, so the lifter tracks it: the byte a fetch puts in the register
    starts at a position one past everything seen, `2 *` and `<< n` and copies
    move a position along, and a bit's position is that of whatever `>> 7` is
    applied to. Bits are then ordered by position, within each run of them.
    Where a bit is taken into a variable, the slot stays open until that
    variable is stored -- the store is the only thing that names it."""
    stmts = statements(function_body(path, name))
    unknown = set() if unknown is None else unknown
    reads = []               # entries are [kind, offset, bit position]
    pending_u32 = 0          # four-byte fetches whose store has not been seen
    pending_u8 = None        # the offset a byte fetch was already emitted for
    parked = {}              # variable -> the open bit slot it will name
    helper_vars = set()      # variables holding a sub-reader's result
    branch_assign = set()    # statements that are a bit-`if`'s sentinel writes
    in_bits = False          # a bit has been taken since the last aligned read
    pos = {}                 # variable -> the bit position its top bit is
    high = [-1]              # the highest position handed out so far
    slot_at = {}             # bit position -> the slot that took it

    def norm(expr):
        return re.sub(r"\s+|\(\s*(?:unsigned\s+)?(?:__int8|__int16|__int32|_BYTE|_WORD|_DWORD|int|char)\s*\)",
                      "", expr)

    def setpos(var, p):
        if p is None:
            pos.pop(var, None)
        else:
            pos[var] = p
            high[0] = max(high[0], p)

    def bitpos(expr):
        """The bit position `expr` -- a shift-register variable, possibly scaled
        or shifted -- has at its top."""
        e = norm(expr)
        while e.startswith("(") and e.endswith(")") and e.count("(") == e.count(")"):
            e = e[1:-1]
        for pat, mul in ((r"^(\d+)\*([A-Za-z_]\w*)$", 0), (r"^([A-Za-z_]\w*)\*(\d+)$", 1)):
            m = re.match(pat, e)
            if m:
                k, v = (int(m.group(1)), m.group(2)) if mul == 0 else (int(m.group(2)), m.group(1))
                if v in pos and k and not (k & (k - 1)):
                    return pos[v] + k.bit_length() - 1
                return None
        m = re.match(r"^([A-Za-z_]\w*)<<(\d+)$", e)
        if m and m.group(1) in pos:
            return pos[m.group(1)] + int(m.group(2))
        m = re.match(r"^([A-Za-z_]\w*)$", e)
        return pos.get(m.group(1)) if m else None

    def taken(expr):
        """The position of the bit `expr` takes off the register -- as
        `<something> >> 7`, or as the sign test `<something> < 0` the
        decompiler uses when the register is signed."""
        e = norm(expr)
        m = re.search(r"^(.*?)>>7", e) or re.match(r"^(.*?)<0$", e)
        return bitpos(m.group(1)) if m else None

    def emit(kind, off, at=None):
        nonlocal in_bits
        if kind in ("u32", "u8", "gbyte") and in_bits:
            reads.append(["flush", None, None])
            in_bits = False
        if kind in ("bit", "bits", "call"):
            in_bits = True
        reads.append([kind, off, at])
        return len(reads) - 1

    def bit(off, at):
        """Take one bit. A position already taken is the decompiler saying the
        same thing twice (it re-tests a gate, or writes `result = v >> 7` beside
        the store of the same expression), so it names that slot instead of
        opening a second one; the return is the new slot, or None if it was a
        repeat."""
        if at is not None and at in slot_at:
            slot = slot_at[at]
            if off is not None and reads[slot][1] is None:
                reads[slot][1] = off
            return None
        i = emit("bit", off, at)
        if at is not None:
            slot_at[at] = i
            high[0] = max(high[0], at)   # a taken position is a used one
        return i

    def park(slot, keys):
        for k in keys:
            parked[k] = slot

    def byte_fetch_is_value(idx, var):
        """A fetched byte is either a whole field (its slot is stored, possibly
        through a copy) or the bit register's refill (it is shifted). Decide by
        the first thing that happens to it."""
        live = {var}
        for k in range(idx + 1, min(idx + 10, len(stmts))):
            s = stmts[k]
            st = STORE_match(s)
            if st and st.group(3).strip() in live:
                return int(st.group(1)), st.group(2) == "^"
            if ">>" in s and any(re.search(r"\b%s\b" % v, s) for v in live):
                return None
            v = VARASSIGN.match(s)
            if v:
                tgt, rhs = (v.group(1) or v.group(3)), (v.group(2) or v.group(4)).strip()
                live.discard(tgt)
                if rhs in live:
                    live.add(tgt)
            if not live:
                return None
        return None

    def sentinel_writes(idx):
        """The variables an `if`'s unbraced constant-assign arms write, and the
        statements they are, so the main walk does not see them twice."""
        vars_, where = [], []
        for k in range(idx + 1, min(idx + 5, len(stmts))):
            if stmts[k] == "else":
                continue
            v = VARASSIGN.match(stmts[k])
            if not (v and re.match(r"^-?\d+$", (v.group(2) or v.group(4) or "").strip())):
                break
            vars_.append(v.group(1) or v.group(3))
            where.append(k)
        return vars_, where

    for idx, s in enumerate(stmts):
        if s in ("{", "}", "else", "do") or s.startswith("return"):
            continue
        if idx in branch_assign:
            continue                         # already accounted for by its `if`

        m = ZEROED.match(s)
        if m:
            parked.pop(m.group(1), None)
            setpos(m.group(1), None)
            continue

        m = re.match(r"^([A-Za-z_]\w*)\s*(\*=\s*2|<<=\s*(\d+))$", s)
        if m and m.group(1) in pos:
            setpos(m.group(1), pos[m.group(1)] + (1 if m.group(3) is None else int(m.group(3))))
            continue

        m = re.match(r"^([A-Za-z_]\w*)\s*>>=\s*7$", s)      # `v6 >>= 7`: a take
        if m and m.group(1) in pos:
            slot = bit(None, pos[m.group(1)])
            if slot is not None:
                park(slot, [m.group(1)])
            setpos(m.group(1), None)
            continue

        calls = CALL.findall(s)

        # a sub-reader whose meaning we know
        helper = next((c for c in calls if c in HELPERS), None)
        if helper:
            emit("call", helper)
            v = VARASSIGN.match(s)
            if v:
                helper_vars.add(v.group(1) or v.group(3))
            continue

        if FETCH_GUID in calls:
            emit("packedguid", None)
            continue

        if FETCH_U32 in calls:
            pending_u32 += 1
            m = re.search(r"&\s*([A-Za-z_][A-Za-z_0-9]*)", s)
            if m:
                setpos(m.group(1), None)
            continue

        if FETCH_U8 in calls:
            # a whole byte, or the bit register's refill: the fetch's own use says
            m = re.search(r"&\s*([A-Za-z_][A-Za-z_0-9]*)", s)
            hit = byte_fetch_is_value(idx, m.group(1)) if m else None
            if hit:
                emit("gbyte" if hit[1] else "u8", hit[0])
                pending_u8 = hit[0]
                if m:
                    setpos(m.group(1), None)
            else:
                pending_u8 = None
                if m:
                    setpos(m.group(1), high[0] + 1)   # the register's next byte
            continue

        if calls:
            # Some other sub_XXXX. It may read nothing at all, but it may be a
            # sub-reader HELPERS has no name for, in which case this lift is
            # quietly short -- so say so rather than walking past it.
            unknown.update(calls)

        st = STORE_match(s)
        if st:
            off, xor, rhs = int(st.group(1)), st.group(2) == "^", st.group(3).strip()
            if pending_u8 == off:
                pending_u8 = None            # already emitted at its fetch
                continue
            if "|" in rhs:
                # An inlined multi-bit field. Its half-assembled pieces can
                # include a `>> 7` that looked like a bit of its own; the OR
                # that consumes them says otherwise.
                for v in [v for v in re.findall(r"[A-Za-z_][A-Za-z_0-9]*", rhs) if v in parked]:
                    if reads[parked[v]][1] is None:
                        reads[parked[v]][0] = "dead"
                        slot_at.pop(reads[parked[v]][2], None)
                    parked.pop(v, None)
                emit("bits", off)
                continue
            if BIT.search(rhs) or taken(rhs) is not None:
                bit(off, taken(rhs))          # taken and stored in one statement
                continue
            ids = [v for v in re.findall(r"[A-Za-z_][A-Za-z_0-9]*", rhs) if v in parked]
            if ids:
                reads[parked[ids[0]]][1] = off   # names a slot opened earlier
                for v in ids:
                    parked.pop(v, None)
                continue
            sm = SIMPLE.match(rhs)
            if sm and sm.group(1) in helper_vars:
                helper_vars.discard(sm.group(1))
                continue                     # already emitted at the call
            if sm and pending_u32:
                emit("u32", off)
                pending_u32 -= 1
                continue
            continue                         # a store of something already read

        if s.startswith("if") or s.startswith("while"):
            vars_, where = sentinel_writes(idx)
            cond = norm(re.sub(r"^\w+\s*\(|\)$", "", s, count=2))
            if BIT.search(s) or taken(cond) is not None:
                # the condition takes the bit; the arms park it for its store
                slot = bit(None, taken(cond))
                if slot is None:
                    continue   # the gate re-tested, not a second bit off the wire
                if not vars_:
                    sys.exit("%s: bit taken in `%s` and never stored" % (name, s))
                park(slot, vars_)
                branch_assign.update(where)
            elif vars_:
                held = [v for v in re.findall(r"[A-Za-z_][A-Za-z_0-9]*", s) if v in parked]
                if held:
                    # `v24 = v21 >> 7; if (v24 != 0) v26 = 0; else v26 = 1.0;`
                    park(parked[held[0]], vars_)
                    for v in held:
                        parked.pop(v, None)
                    branch_assign.update(where)
            continue

        v = VARASSIGN.match(s)
        if v:
            var, rhs = (v.group(1) or v.group(3)), (v.group(2) or v.group(4)).strip()
            parked.pop(var, None)
            helper_vars.discard(var)
            if BIT.search(rhs) or taken(rhs) is not None:
                slot = bit(None, taken(rhs))
                if slot is not None:
                    park(slot, [var])
                setpos(var, None)
            else:
                setpos(var, bitpos(rhs))
            continue

    for c in sorted(unknown):
        sys.stderr.write("%s: WARNING: calls %s, which the lifter has no name for; "
                         "if it reads the stream this lift is short. Add it to HELPERS.\n"
                         % (name, c))
    if pending_u32:
        sys.exit("%s: %d four-byte fetches with no store" % (name, pending_u32))
    reads = [r for r in reads if r[0] != "dead"]
    open_slots = [i for i, r in enumerate(reads) if r[0] == "bit" and r[1] is None]
    if open_slots:
        sys.exit("%s: %d bits taken but never stored (at %s)"
                 % (name, len(open_slots), open_slots))
    unplaced = [i for i, r in enumerate(reads) if r[0] == "bit" and r[2] is None]
    if unplaced:
        sys.exit("%s: %d bits whose shift register was not followed (at %s)"
                 % (name, len(unplaced), unplaced))

    # Each run of consecutive bits goes out in shift-register order, not the
    # order the decompiler wrote the takes down in.
    out, i = [], 0
    while i < len(reads):
        j = i
        while j < len(reads) and reads[j][0] == "bit":
            j += 1
        out += sorted(reads[i:j], key=lambda r: r[2]) if j > i else [reads[i]]
        i = j if j > i else i + 1
    return [(k, o) for k, o, _ in out]


def movement_info_base(reads):
    """Where the message's embedded MovementInfo starts: the lowest offset that
    begins a run of eight consecutive guid-byte slots (the guid itself; the
    transport guid is the other such run, 48 bytes higher). 0 if there is none,
    which is how a family reader -- no MovementInfo at all -- comes out."""
    gbytes = set(off for kind, off in reads if kind == "gbyte")
    runs = sorted(o for o in gbytes if all(o + i in gbytes for i in range(8)))
    return runs[0] if runs else 0


def names(reads, raw=False, base=None):
    if base is None:
        base = movement_info_base(reads)
    out = []
    for kind, off in reads:
        if kind == "flush":
            out.append("FlushBits")
        elif kind == "call":
            out.append(HELPERS[off] if not raw else "call:%s" % off)
        elif kind == "packedguid":
            out.append("PackedGuid")
        elif not raw and (kind, off - base) in OFFSETS:
            out.append(OFFSETS[(kind, off - base)])
        else:
            out.append("%s@%s" % (kind, off))
    return out


def registry_table(name):
    inc = os.path.join(os.path.dirname(os.path.abspath(__file__)), "MovementLayouts.inc")
    text = io.open(inc, encoding="utf-8").read()
    m = re.search(r"^LAYOUT\(%s,(.*?)\)\s*$" % re.escape(name), text, re.S | re.M)
    if not m:
        sys.exit("no LAYOUT(%s) in MovementLayouts.inc" % name)
    return [e for e in re.findall(r"E::([A-Za-z0-9_]+)", m.group(1)) if e != "End"]


def usage():
    sys.exit("\n".join(__doc__.split("\n")[2:6]))


def main():
    args = [a for a in sys.argv[1:]]
    raw = "--raw" in args
    if raw:
        args.remove("--raw")
    control, base = None, None
    for flag in ("--control", "--base"):
        if flag in args:
            i = args.index(flag)
            if i + 1 >= len(args):
                usage()                      # a trailing flag with no value
            if flag == "--control":
                control = args[i + 1]
            else:
                base = int(args[i + 1], 0)
            del args[i:i + 2]
    if len(args) != 2:
        usage()
    path, func = args

    unknown = set()
    reads = lift(path, func, unknown)
    if base is None:
        base = movement_info_base(reads)
    lifted = names(reads, raw, base)

    if not control:
        print("%s: %d elements (MovementInfo at +%d)" % (func, len(lifted), base))
        for i in range(0, len(lifted), 6):
            print("    " + ", ".join('"%s"' % e for e in lifted[i:i + 6]) + ",")
        if unknown:
            # A plain lift is the one that gets copied into LIFTED, so it is the
            # one place the refusal matters: a lift that walked past a callee the
            # lifter has no name for is short by whatever that callee read, and
            # the list printed above is not the table. Refuse it here too, so
            # HELPERS grows instead of the tables.
            print("\nUNRECOGNISED CALLEE: %s (see the warnings above)" % " ".join(sorted(unknown)))
            return 1
        return 0

    want = registry_table(control)
    print("%-34s %-34s" % ("%s (lifted from %s)" % (control, func), "%s (registry)" % control))
    first = None
    for i in range(max(len(lifted), len(want))):
        a = lifted[i] if i < len(lifted) else ""
        b = want[i] if i < len(want) else ""
        mark = " " if a == b else "<"
        if a != b and first is None:
            first = i
        print("%3d %-34s %-34s %s" % (i, a, b, mark))
    if unknown:
        # A calibration that matched while walking past an unnamed sub-reader
        # matched by luck; refuse it, so HELPERS grows instead of the tables.
        print("\nUNRECOGNISED CALLEE: %s (see the warnings above)" % " ".join(sorted(unknown)))
        return 1
    if first is None:
        print("\nMATCH: %d elements" % len(lifted))
        return 0
    print("\nFIRST DIFFERENCE at %d: lifted %r, registry %r" % (first, lifted[first]
          if first < len(lifted) else None, want[first] if first < len(want) else None))
    return 1


if __name__ == "__main__":
    sys.exit(main())
