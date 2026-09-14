# MotionMaster Facade

The facade `MotionMaster` is the movement kernel campaign's shim for the vendored scripts: the entry points below are the whole of what `src/modules/SD3` calls, they keep their meaning through P3's arbiter and P5's deletions, and `src/tests/CheckMotionMasterShim.cmake` fails `ctest` (which CI runs) when a script calls anything else.

| Entry Point | Count |
|---|---|
| MovePoint | 448 |
| Clear | 118 |
| MoveIdle | 100 |
| MoveChase | 75 |
| MoveFollow | 58 |
| MoveTargetedHome | 28 |
| MoveWaypoint | 27 |
| ActiveKind | 20 |
| MoveRandomAroundPoint | 16 |
| MovementExpired | 10 |
| MoveJump | 9 |
| MoveFlyOrLand | 4 |
| Initialize | 3 |
| IsPatrolling | 3 |
| IsChasing | 2 |
| MoveRandom | 1 |
| MoveFleeing | 1 |

Counted 2026-09-14 on `feat/movement-queries` with `grep -rhoE "GetMotionMaster\(\)->[A-Za-z_]+" src/modules/SD3 | sort | uniq -c`.

## The shell

Since P3-B the facade's internals are the kernel's Controller: `Motion::Arbiter` in `src/motion` decides which behaviour is held on which layer and which one is selected, and `MotionMaster` only carries out what it decides. Each held entry is an adapted legacy generator, wrapped as a `LegacyBehaviour` behind the hook matrix the P3-B design lays out. Only the selected behaviour ticks; every other held entry sits idle until the arbiter picks it. Every public call opens an arbiter transaction and commits it at the outermost end, so a hook that re-enters the facade mid-commit still lands under the same generation. `.debug movement dump` prints a unit's held entries by layer, the selected one marked, and, with `Movement.DecisionRing` on, its last thirty-two decisions. The generators themselves and the `LegacyBehaviour` adapter are retired in P5, once nothing legacy is left to adapt.

## Typed queries

Since P3-C the facade answers two kinds of question instead of `GetCurrentMovementGeneratorType() == X`: `ActiveKind()` is the selected kind — what runs now, the stack's "current type" — and `IsChasing()`/`ChaseTarget()`, `IsFollowing()`/`FollowTarget()`, `IsPatrolling()`, `IsOnTaxi()` say whether the entry is held at all, selected or masked beneath a fear, an effect or a taxi (the end of a control episode resumes the masked chase while it still aims at the victim, and requests a fresh one otherwise). `IsReachable()` asks the selected behaviour. `CombatStarted()` is the design's event row: `Unit::Attack` calls it when a new combat begins and the arbiter cancels the Distract layer, which priority selection alone would keep above the chase. `GetCurrentMovementGeneratorType()` stays for the GM prints and the harness's verdict labels until P5 retires the legacy type enum.

The behavioral net over the same facade is `src/game/Harness` (`.debug movement scenario`).

## Control claims

A fear or a confuse holds a Control claim keyed by the aura that applied it (`Motion::ControlClaim(spell, effect, caster)`, P4-A). Claims of one kind coexist and the newest drives (Confused over Fear, then the newest): a second fear on a feared unit replaces the driver in place, an earlier aura's removal changes nothing while a later one runs, and the later one's removal resumes the earlier if it still runs. The shell owns the shared unit state (`UNIT_STAT_FLEEING`, `UNIT_STAT_CONFUSED`): a finishing behaviour clears it, and the shell puts it back while another claim of the kind remains. `SetFeared`/`SetConfused` release one claim and run the end-of-control rule only when the last claim of the kind went: a creature with a victim resumes its chase, one without goes home; a player gets its control back. The low-health flee and a script's `MoveFleeing` use spell-less identities.
