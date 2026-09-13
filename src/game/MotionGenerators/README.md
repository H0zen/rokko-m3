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

## The shell

Since P3-B the facade's internals are the kernel's Controller: `Motion::Arbiter` in `src/motion` decides which behaviour is held on which layer and which one is selected, and `MotionMaster` only carries out what it decides. Each held entry is an adapted legacy generator, wrapped as a `LegacyBehaviour` behind the hook matrix the P3-B design lays out. Only the selected behaviour ticks; every other held entry sits idle until the arbiter picks it. Every public call opens an arbiter transaction and commits it at the outermost end, so a hook that re-enters the facade mid-commit still lands under the same generation. `.debug movement dump` prints a unit's held entries by layer, the selected one marked, and, with `Movement.DecisionRing` on, its last thirty-two decisions. The generators themselves and the `LegacyBehaviour` adapter are retired in P5, once nothing legacy is left to adapt.

The behavioral net over the same facade is `src/game/Harness` (`.debug movement scenario`).
