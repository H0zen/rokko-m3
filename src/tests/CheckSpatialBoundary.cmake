set(HIERARCHY_HEADERS
    Object/Object.h
    Object/Unit.h
    Object/Player.h
    Object/Creature.h
    Object/GameObject.h
    Object/DynamicObject.h
    Object/Corpse.h
    Object/Vehicle.h
    Object/Pet.h
    Object/Totem.h
    Object/TemporarySummon.h)

set(FORBIDDEN_MEMBERS
    GetPositionX GetPositionY GetPositionZ GetOrientation
    GetObjectBoundingRadius GetDistance GetDistance2d GetDistanceZ
    GetDistanceOrder IsWithinDist IsWithinDist2d IsWithinDist3d
    IsWithinDistInMap _IsWithinDist IsInRange IsInRange2d IsInRange3d
    GetAngle HasInArc IsInFront IsInBack IsInFrontInMap IsInBackInMap
    IsInMap IsWithinLOS IsWithinLOSInMap IsPositionValid
    Relocate SetOrientation GetNearPoint GetNearPoint2D GetClosePoint
    GetContactPoint GetRandomPoint UpdateGroundPositionZ
    UpdateAllowedPositionZ GetRespawnCoord SetRespawnCoord ResetRespawnCoord
    GetCombatStartPosition SetCombatStartPosition GetCombatReach
    GetCombatDistance CanReachWithMeleeAttack IsNearWaypoint
    NormalizeRotatedPosition CalculateLocalPositionOf RotateLocalPosition
    GetLocalPositionX GetLocalPositionY GetLocalPositionZ GetLocalOrientation
    GetOrientationFromQuat)

set(VIOLATIONS "")

foreach(HEADER IN LISTS HIERARCHY_HEADERS)
  set(PATH "${SOURCE_ROOT}/src/game/${HEADER}")
  if(NOT EXISTS "${PATH}")
    continue()
  endif()
  file(READ "${PATH}" TEXT)
  foreach(NAME IN LISTS FORBIDDEN_MEMBERS)
    if(TEXT MATCHES "\n[ \t]+[A-Za-z_][A-Za-z_0-9:<>,&\\* \t]*[ \t\\*&]${NAME}[ \t]*\\(")
      list(APPEND VIOLATIONS "${HEADER}: ${NAME}")
    endif()
  endforeach()
endforeach()

file(READ "${SOURCE_ROOT}/src/game/Object/Object.h" OBJECT_H)
foreach(REQUIRED_TEXT
    "Geometry::Placement m_placement"
    "Geometry::Placement const& Where() const")
  string(FIND "${OBJECT_H}" "${REQUIRED_TEXT}" POSITION)
  if(POSITION EQUAL -1)
    list(APPEND VIOLATIONS "Object.h no longer holds the component: ${REQUIRED_TEXT}")
  endif()
endforeach()

if(VIOLATIONS)
  string(REPLACE ";" "\n  " REPORT "${VIOLATIONS}")
  message(FATAL_ERROR
    "Spatial geometry is back on the object hierarchy:\n  ${REPORT}\n"
    "Ask the component instead: obj->Where().DistanceTo(other->Where()).")
endif()

message(STATUS "Spatial boundary intact: the hierarchy owns no geometry")
