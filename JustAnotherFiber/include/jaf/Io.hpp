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

/// Outcome of a transfer; `Ok` means the full requested span moved.
enum class IoStatus : std::uint8_t {
    Ok,          ///< Completed; bytes == requested size.
    ShortRead,   ///< Completed but fewer bytes than requested (read: EOF; write: out of space).
    OpenFailed,  ///< CreateFileW failed (see os_error).
    ReadFailed,  ///< ReadFile / completion reported an error (see os_error).
    WriteFailed, ///< WriteFile / completion reported an error (see os_error).
};

/// Paths are wide. The platform is Windows-only (see Config.hpp), and an ANSI open would
/// mistranslate any path the active code page cannot represent -- silently, on exactly the
/// non-ASCII content directories a narrow API appears to work for.

/// A request to read `dst.size()` bytes from `path` starting at `offset`.
struct ReadRequest {
    const wchar_t*       path;       ///< File path (UTF-16).
    std::span<std::byte> dst;        ///< Destination buffer; must outlive the read.
    std::uint64_t        offset = 0; ///< Byte offset within the file.
};

/// A request to write `src` to `path`, creating or truncating it.
///
/// Whole-file only, deliberately: the open is CREATE_ALWAYS, so a partial write at an offset
/// would truncate everything it was meant to preserve. A caller wanting to patch a file in
/// place needs a different open mode, and should get a different request type when it does.
struct WriteRequest {
    const wchar_t*             path; ///< File path (UTF-16); created or truncated.
    std::span<const std::byte> src;  ///< Source buffer; must outlive the write.
};

/// Result written by the I/O thread before it signals the counter; must outlive the transfer.
struct IoResult {
    IoStatus      status   = IoStatus::Ok;
    std::size_t   bytes    = 0;   ///< Bytes actually transferred.
    std::uint32_t os_error = 0;   ///< GetLastError() on failure, else 0.
};

/// Post an asynchronous read; bumps `*on_complete` by 1 immediately and returns without blocking.
void read_file(const ReadRequest& req, Counter* on_complete, IoResult* out = nullptr);

/// Post an asynchronous whole-file write; bumps `*on_complete` by 1 immediately and returns
/// without blocking. The counter is signalled on every path, including a synchronous open or
/// issue failure, so a parked waiter can never be stranded.
void write_file(const WriteRequest& req, Counter* on_complete, IoResult* out = nullptr);

} // namespace io
} // namespace jaf
