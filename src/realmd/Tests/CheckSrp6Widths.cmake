file(READ "${REALMD_SOURCE}/Auth/AuthSocket.cpp" AUTH_SOCKET)
file(READ "${REALMD_SOURCE}/Auth/Srp6.cpp" SRP6_UNIT)
string(APPEND AUTH_SOCKET "${SRP6_UNIT}")

foreach(FORBIDDEN_TEXT "AsByteArray()" "UpdateBigNumbers")
  string(FIND "${AUTH_SOCKET}" "${FORBIDDEN_TEXT}" POSITION)
  if(NOT POSITION EQUAL -1)
    message(FATAL_ERROR
      "SRP6 width taken from a value instead of the protocol: ${FORBIDDEN_TEXT}")
  endif()
endforeach()

string(FIND "${AUTH_SOCKET}"
  "memcmp(M1, lp.M1, SHA_DIGEST_LENGTH)" PROOF_COMPARE)
if(PROOF_COMPARE EQUAL -1)
  message(FATAL_ERROR "The client proof is not compared at the full digest width")
endif()
