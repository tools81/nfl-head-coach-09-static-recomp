#pragma once

// Reading and writing the game's database from hooks, through the game's own database call
// (sub_82C66928: r6 the query, r7-r10 its arguments, r3 0 on success). Queries are the game's
// SQL-like text with table and field names written reversed, as the game stores them ('YALP' is
// PLAY); 0x85 marks a 32-bit argument (a value, or after "into" the address the result is written
// to) and 0x8A a cursor. Arguments beyond four are not supported: write constants into the text.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include <ppc_recomp_shared.h>

#include "kernel/heap.h"

extern "C" PPC_FUNC(__imp__sub_82C669F0);  // a cursor's initial state
extern "C" PPC_FUNC(__imp__sub_82C66A10);  // whether a cursor is open
extern "C" PPC_FUNC(__imp__sub_82C66A28);  // closes a cursor

namespace hc09::guest
{
    class Database
    {
    public:
        static constexpr uint32_t kNone = 32767;  // what results hold until a query writes them

        Database(PPCContext& ctx, uint8_t* base) : ctx_(ctx), base_(base)
        {
            static const uint32_t memory = hc09::PoolHeap().Allocate(kSize);
            memory_ = memory;
        }

        bool Ready() const { return memory_ != 0; }

        // The guest address of result n (n < 8), set to kNone.
        uint32_t Out(uint32_t n) { return Put(n * 4, kNone); }
        uint32_t Get(uint32_t n) const { return Word(n * 4); }

        // 8 zero bytes of guest memory.
        uint32_t Zeros()
        {
            Put(36, 0);
            return Put(32, 0);
        }

        // A guest copy of a string (slot < 4, at most 111 characters), kept until the slot is reused.
        uint32_t Text(uint32_t slot, const std::string& text)
        {
            const uint32_t at = kStrings + slot * 112;
            const size_t length = std::min<size_t>(text.size(), 111);
            std::memcpy(base_ + memory_ + at, text.data(), length);
            base_[memory_ + at + length] = 0;
            return memory_ + at;
        }

        bool Query(const char* text, std::initializer_list<uint32_t> args) { return Run(kQuery, text, args); }

        // The rows of a cursor: `declare` is "declare \x8A fastcursor for select ... into \x85 ..."
        // with up to three results (read with Get(0-2) in `row`) and no other arguments.
        template <typename Row>
        void ForEach(const char* declare, uint32_t results, Row row)
        {
            const uint32_t cursor = memory_ + kCursor;
            Call(__imp__sub_82C669F0, cursor);
            std::vector<uint32_t> args = { cursor };
            for (uint32_t n = 0; n < results && n < 3; n++)
                args.push_back(Out(n));
            if (RunList(kDeclare, declare, args))
                while (Run(kQuery, "fetch from \x8A", { cursor }))
                    row();
            if (Call(__imp__sub_82C66A10, cursor) & 0xFF)
                Call(__imp__sub_82C66A28, cursor);
        }

        // Calls a guest function with up to six arguments; returns r3.
        uint32_t Call(PPCFunc* function, std::initializer_list<uint32_t> args)
        {
            PPCContext call = ctx_;
            PPCRegister* registers[] = { &call.r3, &call.r4, &call.r5, &call.r6, &call.r7, &call.r8 };
            size_t i = 0;
            for (uint32_t arg : args)
                if (i < 6)
                    registers[i++]->u64 = arg;
            function(call, base_);
            return call.r3.u32;
        }
        uint32_t Call(PPCFunc* function, uint32_t arg) { return Call(function, { arg }); }

        uint8_t* base() const { return base_; }
        PPCContext& ctx() const { return ctx_; }

    private:
        static constexpr uint32_t kCursor = 48, kQuery = 64, kDeclare = 576, kStrings = 1088, kSize = 1536;

        uint32_t Word(uint32_t offset) const
        {
            return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base_ + memory_ + offset));
        }
        uint32_t Put(uint32_t offset, uint32_t value)
        {
            *reinterpret_cast<uint32_t*>(base_ + memory_ + offset) = __builtin_bswap32(value);
            return memory_ + offset;
        }

        bool Run(uint32_t area, const char* text, std::initializer_list<uint32_t> args)
        {
            return RunList(area, text, std::vector<uint32_t>(args));
        }
        bool RunList(uint32_t area, const char* text, const std::vector<uint32_t>& args)
        {
            const size_t length = std::strlen(text) + 1;
            if (length > 512 || args.size() > 4)
                return false;
            std::memcpy(base_ + memory_ + area, text, length);
            PPCContext call = ctx_;
            call.r3.u64 = 0;
            call.r4.u64 = 0;
            call.r5.u64 = 0;
            call.r6.u64 = memory_ + area;
            PPCRegister* registers[] = { &call.r7, &call.r8, &call.r9, &call.r10 };
            for (size_t i = 0; i < args.size(); i++)
                registers[i]->u64 = args[i];
            sub_82C66928(call, base_);
            return call.r3.u32 == 0;
        }

        PPCContext& ctx_;
        uint8_t* base_;
        uint32_t memory_ = 0;
    };
}
