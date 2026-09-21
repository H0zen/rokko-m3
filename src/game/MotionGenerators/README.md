# MotionMaster Facade

The facade `MotionMaster` is the movement kernel campaign's shim for the vendored scripts: the entry points below are the whole of what `src/modules/SD3` calls, they keep their meaning through the campaign's phases, and `src/tests/CheckMotionMasterShim.cmake` fails `ctest` (which CI runs) when a script calls anything else.

| Entry Point | Count |
|---|---|
| MovePoint | 445 |
| Clear | 115 |
| Stop | 97 |
| MoveChase | 70 |
| MoveFollow | 56 |
| MoveTargetedHome | 28 |
| MoveWaypoint | 27 |
| ActiveKind | 20 |
| Wander | 16 |
| Finish | 10 |
| MoveJump | 8 |
| MoveFlyOrLand | 4 |
| IsPatrolling | 3 |
| Initialize | 3 |
| IsChasing | 2 |
| Uninhibit | 1 |
| MoveFleeing | 1 |
| Inhibit | 1 |

Counted 2026-09-18 on `feat/movement-one-vocabulary` with `grep -rhoE "Movement\(\)->[A-Za-z_]+" src/modules/SD3 | sort | uniq -c`. `Inhibit`/`Uninhibit` (P5-A Task 4) joined the promise with `grizzly_hills.cpp`'s stun: the one script that names itself as a source of the kernel's block instead of writing a unit-state bit itself. The gate lists 18 entry points; `MoveRandom`, a method the facade never had, left the list in P5-C.

A script's `Inhibit`/`Uninhibit` pair must balance: a source it never releases holds the creature until it dies (death releases the aura and script sources, `Motion::Restrictions::DropDomain`).

## The shell

Since P3-B the facade's internals are the kernel's Controller: `Motion::Arbiter` in `src/motion` decides which behaviour is held on which layer and which one is selected, and `MotionMaster` only carries out what it decides. Each held entry is a native behaviour of the kernel (`src/motion`) inside the shell's one adapter, `NativeBehaviour`, behind the hook matrix the P3-B design lays out; since P5-B family 5 no legacy generator remains and since P5-C no legacy type: the facade, the AI hooks (`CreatureAI::MovementInform(Motion::Kind, id)`, `SummonedMovementInform`, `WaypointPathInform` for an external path's progress), the harness's labels and the GM prints all speak `Motion::Kind`, and the DB's default movement type is its own `CreatureMovementType` (idle, random, waypoint), mapped to a kind by `MotionMaster::Initialize`. Only the selected behaviour ticks; every other held entry sits idle until the arbiter picks it. Every public call opens an arbiter transaction and commits it at the outermost end, so a hook that re-enters the facade mid-commit still lands under the same generation. `.debug movement dump` prints a unit's held entries by layer, the selected one marked, and, with `Movement.DecisionRing` on, its last thirty-two decisions; `.movegens` prints the same entries with what each tracks.

## Typed queries

Since P3-C the facade answers two kinds of question: `Doing()` is the selected kind — what runs now — and `IsChasing()`/`ChaseTarget()`, `IsFollowing()`/`FollowTarget()`, `IsPatrolling()`, `IsOnTaxi()` say whether the entry is held at all, selected or masked beneath a fear, an effect or a taxi (the end of a control episode resumes the masked chase while it still aims at the victim, and requests a fresh one otherwise). `IsReachable()` asks the selected behaviour; `SelectedVariant()` its variant (the timed flee's 1). `CombatStarted()` is the design's event row: `Unit::Attack` calls it when a new combat begins and the arbiter cancels the Distract layer, which priority selection alone would keep above the chase.

The behavioral net over the same facade is `src/game/Harness` (`.debug movement scenario`).

## Control claims

A fear or a confuse holds a Control claim keyed by the aura that applied it (`Motion::ControlClaim(spell, effect, caster)`, P4-A). Claims of one kind coexist and the newest drives (Confused over Fear, then the newest): a second fear on a feared unit replaces the driver in place, an earlier aura's removal changes nothing while a later one runs, and the later one's removal resumes the earlier if it still runs. The shell reads the block through the published state (`MotionMaster::Published`, P5-C2), refreshed at the end of every commit: it carries Feared and Confused while any claim of the kind is held, independent of which claim is finishing, and the run speed a fear was walked down to is restored only once no fear claim survives. `SetFeared`/`SetConfused` release one claim and run the end-of-control rule only when the last claim of the kind went: a creature with a victim resumes its chase, one without goes home; a player gets its control back. The low-health flee and a script's `MoveFleeing` use spell-less identities. The flee and the stagger themselves are the kernel's `Motion::FearBehaviour` and `Motion::ConfusedBehaviour` (P5-B family 4): the flee's bands are parameters at TC's values, the stagger's envelope is `Movement.ConfuseRadius`. The taxi is `Motion::TaxiBehaviour` (P5-B family 5): the route's hops welded into one flight at `Movement.TaxiSpeed`, the client-control update sent around it as retail does, the landing teleported onto the flight node by the server's clock, and no `UNIT_FLAG_MOUNT` on the passenger.

Since P5-C3 the natives' presence and leg bits are the facade's latch bank (`MotionMaster::Latches`): a chase's and a follow's presence and leg, the roaming pair the wander, the patrol and the point family write, a fear's and a confuse's leg. A native writes its own presence and leg (`Effect::Latch`, and the `Roaming` field of a Step or an Outcome) and the facade maps its kind to the channel (`WriteLatches`, `WriteRoaming`), in the recipe's order. The bank is one per unit, shared and destructive as the unit-state bits it replaced were: a Point's finish clears the roaming pair whoever set it, and a native finishing after its binding is gone still writes its own leg. The stops clear the moving legs (`ClearMovingLatches`: the confuse's lurch was never counted), the whole-state wipes clear everything (`ClearAllLatches`), and the Home's first tick wipes the latches with the published state and the dynamic unit-state bits (`WipeLatches`). `Unit::IsStopped` and `Unit::FollowLatched` read it; the gait restore's running state is `LatchBank::RunningLeg()` or the creature's `UNIT_STAT_RUNNING`.
