#pragma once

#include <string>

namespace mangos::patcher {

// Reads one `Name = value` line out of a client.secret or server.secret.
//
// The format is the server's own config syntax, so an operator who can read
// mangosd.conf can read this, and neither side needs a JSON parser to agree on
// what a key is. `#` starts a comment; surrounding space and quotes are trimmed.
//
// Lives in the patcher library rather than beside the generator because both
// ends read these files and only one end writes them: the generator links proto
// and the server's signer, and the patcher must keep linking nothing but the
// tree's hashes.
bool ReadSecretField(const std::string& path, const std::string& field,
                     std::string& out, std::string& error);

}  // namespace mangos::patcher
