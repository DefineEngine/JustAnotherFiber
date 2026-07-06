#pragma once

// context/FContext.hpp -- extern "C" declarations for the vendored Boost.Context
// fcontext assembly primitives (Windows x64 _ms_pe_masm variants).

#include <cstddef>

namespace jaf::detail
{
    extern "C" {
    using fcontext_t = void*;

    struct transfer_t
    {
        fcontext_t fctx;
        void* data;
    };

    transfer_t jump_fcontext(fcontext_t to, void* vp);
    fcontext_t make_fcontext(void* sp, std::size_t size, void (*fn)(transfer_t));
    transfer_t ontop_fcontext(fcontext_t to, void* vp, transfer_t (*fn)(transfer_t));
    } // extern "C"
} // namespace jaf::detail
