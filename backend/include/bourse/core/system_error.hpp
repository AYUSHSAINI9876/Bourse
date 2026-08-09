#pragma once

#include <string>

/// \file system_error.hpp
/// Turning a platform error code into a message, safely from any thread.

namespace bourse {

/// Renders `errno` (or a Winsock error) as text.
///
/// `std::strerror` is not thread-safe: it may return a pointer into a shared
/// static buffer, so two threads formatting an error at the same moment can
/// see each other's message or a spliced mixture of both. This server runs N
/// I/O event loops and every one of them formats errors on its failure paths,
/// so that is a live race rather than a theoretical one -- clang-tidy's
/// `concurrency-mt-unsafe` check flagged all seven call sites.
///
/// The failure mode is admittedly mild (a confusing log line, not corruption),
/// which is exactly why it survives so long in codebases: nothing crashes and
/// nobody reads the log closely enough to notice the message is wrong.
[[nodiscard]] std::string describeSystemError(int code);

}  // namespace bourse
