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
| GetCurrentMovementGeneratorType | 25 |
| MoveRandomAroundPoint | 16 |
| MovementExpired | 10 |
| MoveJump | 9 |
| MoveFlyOrLand | 4 |
| Initialize | 3 |
| MoveRandom | 1 |
| MoveFleeing | 1 |

Counted 2026-09-12 on master ebb271cae with `grep -rhoE "GetMotionMaster\(\)->[A-Za-z_]+" src/modules/SD3 | sort | uniq -c`.

The behavioral net over the same facade is `src/game/Harness` (`.debug movement scenario`).
