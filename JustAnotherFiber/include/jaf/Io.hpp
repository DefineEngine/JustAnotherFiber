#pragma once

// jaf/Io.hpp -- asynchronous file I/O; a dedicated I/O thread owns a completion
// port and signals the caller's Counter on completion, without blocking a worker.

#include <jaf/Config.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace jaf {

class Counter;

namespace io {

/// Outcome of a read; `Ok` means the full requested span was filled.
enum class IoStatus : std::uint8_t {
    Ok,          ///< Completed; bytes == requested size.
    ShortRead,   ///< Completed but fewer bytes than requested (e.g. EOF).
    OpenFailed,  ///< CreateFile failed (see os_error).
    ReadFailed,  ///< ReadFile / completion reported an error (see os_error).
};

/// A request to read `dst.size()` bytes from `path` starting at `offset`.
struct ReadRequest {
    const char*         path;        ///< File path (UTF-8 / ANSI).
    std::span<std::byte> dst;        ///< Destination buffer; must outlive the read.
    std::uint64_t       offset = 0;  ///< Byte offset within the file.
};

/// Result written by the I/O thread before it signals the counter; must outlive the read.
struct IoResult {
    IoStatus      status   = IoStatus::Ok;
    std::size_t   bytes    = 0;   ///< Bytes actually transferred.
    std::uint32_t os_error = 0;   ///< GetLastError() on failure, else 0.
};

/// Post an asynchronous read; bumps `*on_complete` by 1 immediately and returns without blocking.
void read_file(const ReadRequest& req, Counter* on_complete, IoResult* out = nullptr);

} // namespace io
} // namespace jaf
