#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/types.h>
#include <vector>

#include "avm_common.hpp"
#include "avm_helper.hpp"
#include "avm_mem_trace.hpp"
#include "avm_trace.hpp"

namespace bb::avm_trace {

/**
 * @brief Constructor of a trace builder of AVM. Only serves to set the capacity of the
 *        underlying traces.
 */
AvmTraceBuilder::AvmTraceBuilder()
{
    main_trace.reserve(AVM_TRACE_SIZE);
}

/**
 * @brief Resetting the internal state so that a new trace can be rebuilt using the same object.
 *
 */
void AvmTraceBuilder::reset()
{
    main_trace.clear();
    mem_trace_builder.reset();
    alu_trace_builder.reset();
}

AvmTraceBuilder::IndirectThreeResolution AvmTraceBuilder::resolve_ind_three(
    uint32_t clk, uint8_t indirect, uint32_t a_offset, uint32_t b_offset, uint32_t dst_offset)
{
    bool indirect_flag_a = is_operand_indirect(indirect, 0);
    bool indirect_flag_b = is_operand_indirect(indirect, 1);
    bool indirect_flag_c = is_operand_indirect(indirect, 2);

    uint32_t direct_a_offset = a_offset;
    uint32_t direct_b_offset = b_offset;
    uint32_t direct_dst_offset = dst_offset;

    bool tag_match = true;

    if (indirect_flag_a) {
        auto read_ind_a = mem_trace_builder.indirect_read_and_load_from_memory(clk, IndirectRegister::IND_A, a_offset);
        direct_a_offset = uint32_t(read_ind_a.val);
        tag_match = tag_match && read_ind_a.tag_match;
    }

    if (indirect_flag_b) {
        auto read_ind_b = mem_trace_builder.indirect_read_and_load_from_memory(clk, IndirectRegister::IND_B, b_offset);
        direct_b_offset = uint32_t(read_ind_b.val);
        tag_match = tag_match && read_ind_b.tag_match;
    }

    if (indirect_flag_c) {
        auto read_ind_c =
            mem_trace_builder.indirect_read_and_load_from_memory(clk, IndirectRegister::IND_C, dst_offset);
        direct_dst_offset = uint32_t(read_ind_c.val);
        tag_match = tag_match && read_ind_c.tag_match;
    }

    return IndirectThreeResolution{
        .tag_match = tag_match,
        .direct_a_offset = direct_a_offset,
        .direct_b_offset = direct_b_offset,
        .direct_dst_offset = direct_dst_offset,
        .indirect_flag_a = indirect_flag_a,
        .indirect_flag_b = indirect_flag_b,
        .indirect_flag_c = indirect_flag_c,
    };
}

/**
 * @brief Addition with direct or indirect memory access.
 *
 * @param indirect A byte encoding information about indirect/direct memory access.
 * @param a_offset An index in memory pointing to the first operand of the addition.
 * @param b_offset An index in memory pointing to the second operand of the addition.
 * @param dst_offset An index in memory pointing to the output of the addition.
 * @param in_tag The instruction memory tag of the operands.
 */
void AvmTraceBuilder::op_add(
    uint8_t indirect, uint32_t a_offset, uint32_t b_offset, uint32_t dst_offset, AvmMemoryTag in_tag)
{
    auto clk = static_cast<uint32_t>(main_trace.size());

    auto const res = resolve_ind_three(clk, indirect, a_offset, b_offset, dst_offset);
    bool tag_match = res.tag_match;

    // Reading from memory and loading into ia resp. ib.
    auto read_a = mem_trace_builder.read_and_load_from_memory(clk, IntermRegister::IA, res.direct_a_offset, in_tag);
    auto read_b = mem_trace_builder.read_and_load_from_memory(clk, IntermRegister::IB, res.direct_b_offset, in_tag);
    tag_match = read_a.tag_match && read_b.tag_match;

    // a + b = c
    FF a = read_a.val;
    FF b = read_b.val;

    // In case of a memory tag error, we do not perform the computation.
    // Therefore, we do not create any entry in ALU table and store the value 0 as
    // output (c) in memory.
    FF c = tag_match ? alu_trace_builder.op_add(a, b, in_tag, clk) : FF(0);

    // Write into memory value c from intermediate register ic.
    mem_trace_builder.write_into_memory(clk, IntermRegister::IC, res.direct_dst_offset, c, in_tag);

    main_trace.push_back(Row{
        .avm_main_clk = clk,
        .avm_main_pc = FF(pc++),
        .avm_main_internal_return_ptr = FF(internal_return_ptr),
        .avm_main_sel_op_add = FF(1),
        .avm_main_in_tag = FF(static_cast<uint32_t>(in_tag)),
        .avm_main_tag_err = FF(static_cast<uint32_t>(!tag_match)),
        .avm_main_ia = a,
        .avm_main_ib = b,
        .avm_main_ic = c,
        .avm_main_mem_op_a = FF(1),
        .avm_main_mem_op_b = FF(1),
        .avm_main_mem_op_c = FF(1),
        .avm_main_rwc = FF(1),
        .avm_main_ind_a = res.indirect_flag_a ? FF(a_offset) : FF(0),
        .avm_main_ind_b = res.indirect_flag_b ? FF(b_offset) : FF(0),
        .avm_main_ind_c = res.indirect_flag_c ? FF(dst_offset) : FF(0),
        .avm_main_ind_op_a = FF(static_cast<uint32_t>(res.indirect_flag_a)),
        .avm_main_ind_op_b = FF(static_cast<uint32_t>(res.indirect_flag_b)),
        .avm_main_ind_op_c = FF(static_cast<uint32_t>(res.indirect_flag_c)),
        .avm_main_mem_idx_a = FF(res.direct_a_offset),
        .avm_main_mem_idx_b = FF(res.direct_b_offset),
        .avm_main_mem_idx_c = FF(res.direct_dst_offset),
    });
}

/**
 * @brief Subtraction with direct or indirect memory access.
 *
 * @param indirect A byte encoding information about indirect/direct memory access.
 * @param a_offset An index in memory pointing to the first operand of the subtraction.
 * @param b_offset An index in memory pointing to the second operand of the subtraction.
 * @param dst_offset An index in memory pointing to the output of the subtraction.
 * @param in_tag The instruction memory tag of the operands.
 */
void AvmTraceBuilder::op_sub(
    uint8_t indirect, uint32_t a_offset, uint32_t b_offset, uint32_t dst_offset, AvmMemoryTag in_tag)
{
    auto clk = static_cast<uint32_t>(main_trace.size());

    auto const res = resolve_ind_three(clk, indirect, a_offset, b_offset, dst_offset);
    bool tag_match = res.tag_match;

    // Reading from memory and loading into ia resp. ib.
    auto read_a = mem_trace_builder.read_and_load_from_memory(clk, IntermRegister::IA, res.direct_a_offset, in_tag);
    auto read_b = mem_trace_builder.read_and_load_from_memory(clk, IntermRegister::IB, res.direct_b_offset, in_tag);
    tag_match = read_a.tag_match && read_b.tag_match;

    // a - b = c
    FF a = read_a.val;
    FF b = read_b.val;

    // In case of a memory tag error, we do not perform the computation.
    // Therefore, we do not create any entry in ALU table and store the value 0 as
    // output (c) in memory.
    FF c = tag_match ? alu_trace_builder.op_sub(a, b, in_tag, clk) : FF(0);

    // Write into memory value c from intermediate register ic.
    mem_trace_builder.write_into_memory(clk, IntermRegister::IC, res.direct_dst_offset, c, in_tag);

    main_trace.push_back(Row{
        .avm_main_clk = clk,
        .avm_main_pc = FF(pc++),
        .avm_main_internal_return_ptr = FF(internal_return_ptr),
        .avm_main_sel_op_sub = FF(1),
        .avm_main_in_tag = FF(static_cast<uint32_t>(in_tag)),
        .avm_main_tag_err = FF(static_cast<uint32_t>(!tag_match)),
        .avm_main_ia = a,
        .avm_main_ib = b,
        .avm_main_ic = c,
        .avm_main_mem_op_a = FF(1),
        .avm_main_mem_op_b = FF(1),
        .avm_main_mem_op_c = FF(1),
        .avm_main_rwc = FF(1),
        .avm_main_ind_a = res.indirect_flag_a ? FF(a_offset) : FF(0),
        .avm_main_ind_b = res.indirect_flag_b ? FF(b_offset) : FF(0),
        .avm_main_ind_c = res.indirect_flag_c ? FF(dst_offset) : FF(0),
        .avm_main_ind_op_a = FF(static_cast<uint32_t>(res.indirect_flag_a)),
        .avm_main_ind_op_b = FF(static_cast<uint32_t>(res.indirect_flag_b)),
        .avm_main_ind_op_c = FF(static_cast<uint32_t>(res.indirect_flag_c)),
        .avm_main_mem_idx_a = FF(res.direct_a_offset),
        .avm_main_mem_idx_b = FF(res.direct_b_offset),
        .avm_main_mem_idx_c = FF(res.direct_dst_offset),
    });
}

/**
 * @brief Multiplication with direct or indirect memory access.
 *
 * @param indirect A byte encoding information about indirect/direct memory access.
 * @param a_offset An index in memory pointing to the first operand of the multiplication.
 * @param b_offset An index in memory pointing to the second operand of the multiplication.
 * @param dst_offset An index in memory pointing to the output of the multiplication.
 * @param in_tag The instruction memory tag of the operands.
 */
void AvmTraceBuilder::op_mul(
    uint8_t indirect, uint32_t a_offset, uint32_t b_offset, uint32_t dst_offset, AvmMemoryTag in_tag)
{
    auto clk = static_cast<uint32_t>(main_trace.size());

    auto const res = resolve_ind_three(clk, indirect, a_offset, b_offset, dst_offset);
    bool tag_match = res.tag_match;

    // Reading from memory and loading into ia resp. ib.
    auto read_a = mem_trace_builder.read_and_load_from_memory(clk, IntermRegister::IA, res.direct_a_offset, in_tag);
    auto read_b = mem_trace_builder.read_and_load_from_memory(clk, IntermRegister::IB, res.direct_b_offset, in_tag);
    tag_match = read_a.tag_match && read_b.tag_match;

    // a * b = c
    FF a = read_a.val;
    FF b = read_b.val;

    // In case of a memory tag error, we do not perform the computation.
    // Therefore, we do not create any entry in ALU table and store the value 0 as
    // output (c) in memory.
    FF c = tag_match ? alu_trace_builder.op_mul(a, b, in_tag, clk) : FF(0);

    // Write into memory value c from intermediate register ic.
    mem_trace_builder.write_into_memory(clk, IntermRegister::IC, res.direct_dst_offset, c, in_tag);

    main_trace.push_back(Row{
        .avm_main_clk = clk,
        .avm_main_pc = FF(pc++),
        .avm_main_internal_return_ptr = FF(internal_return_ptr),
        .avm_main_sel_op_mul = FF(1),
        .avm_main_in_tag = FF(static_cast<uint32_t>(in_tag)),
        .avm_main_tag_err = FF(static_cast<uint32_t>(!tag_match)),
        .avm_main_ia = a,
        .avm_main_ib = b,
        .avm_main_ic = c,
        .avm_main_mem_op_a = FF(1),
        .avm_main_mem_op_b = FF(1),
        .avm_main_mem_op_c = FF(1),
        .avm_main_rwc = FF(1),
        .avm_main_ind_a = res.indirect_flag_a ? FF(a_offset) : FF(0),
        .avm_main_ind_b = res.indirect_flag_b ? FF(b_offset) : FF(0),
        .avm_main_ind_c = res.indirect_flag_c ? FF(dst_offset) : FF(0),
        .avm_main_ind_op_a = FF(static_cast<uint32_t>(res.indirect_flag_a)),
        .avm_main_ind_op_b = FF(static_cast<uint32_t>(res.indirect_flag_b)),
        .avm_main_ind_op_c = FF(static_cast<uint32_t>(res.indirect_flag_c)),
        .avm_main_mem_idx_a = FF(res.direct_a_offset),
        .avm_main_mem_idx_b = FF(res.direct_b_offset),
        .avm_main_mem_idx_c = FF(res.direct_dst_offset),
    });
}

/** TODO: Implement for non finite field types
 * @brief Division with direct or indirect memory access.
 *
 * @param indirect A byte encoding information about indirect/direct memory access.
 * @param a_offset An index in memory pointing to the first operand of the division.
 * @param b_offset An index in memory pointing to the second operand of the division.
 * @param dst_offset An index in memory pointing to the output of the division.
 * @param in_tag The instruction memory tag of the operands.
 */
void AvmTraceBuilder::op_div(
    uint8_t indirect, uint32_t a_offset, uint32_t b_offset, uint32_t dst_offset, AvmMemoryTag in_tag)
{
    auto clk = static_cast<uint32_t>(main_trace.size());

    auto const res = resolve_ind_three(clk, indirect, a_offset, b_offset, dst_offset);
    bool tag_match = res.tag_match;

    // Reading from memory and loading into ia resp. ib.
    auto read_a = mem_trace_builder.read_and_load_from_memory(clk, IntermRegister::IA, res.direct_a_offset, in_tag);
    auto read_b = mem_trace_builder.read_and_load_from_memory(clk, IntermRegister::IB, res.direct_b_offset, in_tag);
    tag_match = read_a.tag_match && read_b.tag_match;

    // a * b^(-1) = c
    FF a = read_a.val;
    FF b = read_b.val;
    FF c;
    FF inv;
    FF error;

    if (!b.is_zero()) {

        inv = b.invert();
        c = a * inv;
        error = 0;
    } else {
        inv = 1;
        c = 0;
        error = 1;
    }

    // Write into memory value c from intermediate register ic.
    mem_trace_builder.write_into_memory(clk, IntermRegister::IC, res.direct_dst_offset, c, in_tag);

    main_trace.push_back(Row{
        .avm_main_clk = clk,
        .avm_main_pc = FF(pc++),
        .avm_main_internal_return_ptr = FF(internal_return_ptr),
        .avm_main_sel_op_div = FF(1),
        .avm_main_in_tag = FF(static_cast<uint32_t>(in_tag)),
        .avm_main_op_err = tag_match ? error : FF(1),
        .avm_main_tag_err = FF(static_cast<uint32_t>(!tag_match)),
        .avm_main_inv = tag_match ? inv : FF(1),
        .avm_main_ia = tag_match ? a : FF(0),
        .avm_main_ib = tag_match ? b : FF(0),
        .avm_main_ic = tag_match ? c : FF(0),
        .avm_main_mem_op_a = FF(1),
        .avm_main_mem_op_b = FF(1),
        .avm_main_mem_op_c = FF(1),
        .avm_main_rwc = FF(1),
        .avm_main_ind_a = res.indirect_flag_a ? FF(a_offset) : FF(0),
        .avm_main_ind_b = res.indirect_flag_b ? FF(b_offset) : FF(0),
        .avm_main_ind_c = res.indirect_flag_c ? FF(dst_offset) : FF(0),
        .avm_main_ind_op_a = FF(static_cast<uint32_t>(res.indirect_flag_a)),
        .avm_main_ind_op_b = FF(static_cast<uint32_t>(res.indirect_flag_b)),
        .avm_main_ind_op_c = FF(static_cast<uint32_t>(res.indirect_flag_c)),
        .avm_main_mem_idx_a = FF(res.direct_a_offset),
        .avm_main_mem_idx_b = FF(res.direct_b_offset),
        .avm_main_mem_idx_c = FF(res.direct_dst_offset),
    });
}

/**
 * @brief Bitwise not with direct or indirect memory access.
 *
 * @param indirect A byte encoding information about indirect/direct memory access.
 * @param a_offset An index in memory pointing to the only operand of Not.
 * @param dst_offset An index in memory pointing to the output of Not.
 * @param in_tag The instruction memory tag of the operands.
 */
void AvmTraceBuilder::op_not(uint8_t indirect, uint32_t a_offset, uint32_t dst_offset, AvmMemoryTag in_tag)
{
    auto clk = static_cast<uint32_t>(main_trace.size());
    bool tag_match = true;
    uint32_t direct_a_offset = a_offset;
    uint32_t direct_dst_offset = dst_offset;

    bool indirect_a_flag = is_operand_indirect(indirect, 0);
    bool indirect_c_flag = is_operand_indirect(indirect, 1);

    if (indirect_a_flag) {
        auto read_ind_a = mem_trace_builder.indirect_read_and_load_from_memory(clk, IndirectRegister::IND_A, a_offset);
        tag_match = read_ind_a.tag_match;
        direct_a_offset = uint32_t(read_ind_a.val);
    }

    if (indirect_c_flag) {
        auto read_ind_c =
            mem_trace_builder.indirect_read_and_load_from_memory(clk, IndirectRegister::IND_C, dst_offset);
        tag_match = tag_match && read_ind_c.tag_match;
        direct_dst_offset = uint32_t(read_ind_c.val);
    }

    // Reading from memory and loading into ia.
    auto read_a = mem_trace_builder.read_and_load_from_memory(clk, IntermRegister::IA, direct_a_offset, in_tag);
    tag_match = read_a.tag_match && tag_match;
    // ~a = c
    FF a = read_a.val;

    // In case of a memory tag error, we do not perform the computation.
    // Therefore, we do not create any entry in ALU table and store the value 0 as
    // output (c) in memory.
    FF c = tag_match ? alu_trace_builder.op_not(a, in_tag, clk) : FF(0);

    // Write into memory value c from intermediate register ic.
    mem_trace_builder.write_into_memory(clk, IntermRegister::IC, direct_dst_offset, c, in_tag);

    main_trace.push_back(Row{
        .avm_main_clk = clk,
        .avm_main_pc = FF(pc++),
        .avm_main_internal_return_ptr = FF(internal_return_ptr),
        .avm_main_sel_op_not = FF(1),
        .avm_main_in_tag = FF(static_cast<uint32_t>(in_tag)),
        .avm_main_tag_err = FF(static_cast<uint32_t>(!read_a.tag_match)),
        .avm_main_ia = a,
        .avm_main_ic = c,
        .avm_main_mem_op_a = FF(1),
        .avm_main_mem_op_c = FF(1),
        .avm_main_rwc = FF(1),
        .avm_main_ind_a = indirect_a_flag ? FF(a_offset) : FF(0),
        .avm_main_ind_c = indirect_c_flag ? FF(dst_offset) : FF(0),
        .avm_main_ind_op_a = FF(static_cast<uint32_t>(indirect_a_flag)),
        .avm_main_ind_op_c = FF(static_cast<uint32_t>(indirect_c_flag)),
        .avm_main_mem_idx_a = FF(direct_a_offset),
        .avm_main_mem_idx_c = FF(direct_dst_offset),
    });
}

/**
 * @brief Equality with direct or indirect memory access.
 *
 * @param indirect A byte encoding information about indirect/direct memory access.
 * @param a_offset An index in memory pointing to the first operand of the equality.
 * @param b_offset An index in memory pointing to the second operand of the equality.
 * @param dst_offset An index in memory pointing to the output of the equality.
 * @param in_tag The instruction memory tag of the operands.
 */
void AvmTraceBuilder::op_eq(
    uint8_t indirect, uint32_t a_offset, uint32_t b_offset, uint32_t dst_offset, AvmMemoryTag in_tag)
{
    auto clk = static_cast<uint32_t>(main_trace.size());

    auto const res = resolve_ind_three(clk, indirect, a_offset, b_offset, dst_offset);
    bool tag_match = res.tag_match;

    // Reading from memory and loading into ia resp. ib.
    auto read_a = mem_trace_builder.read_and_load_from_memory(clk, IntermRegister::IA, res.direct_a_offset, in_tag);
    auto read_b = mem_trace_builder.read_and_load_from_memory(clk, IntermRegister::IB, res.direct_b_offset, in_tag);
    tag_match = read_a.tag_match && read_b.tag_match;

    // c = a == b ? 1 : 0
    FF a = read_a.val;
    FF b = read_b.val;

    // In case of a memory tag error, we do not perform the computation.
    // Therefore, we do not create any entry in ALU table and store the value 0 as
    // output (c) in memory.
    FF c = tag_match ? alu_trace_builder.op_eq(a, b, in_tag, clk) : FF(0);

    // Write into memory value c from intermediate register ic.
    mem_trace_builder.write_into_memory(clk, IntermRegister::IC, res.direct_dst_offset, c, in_tag);

    main_trace.push_back(Row{
        .avm_main_clk = clk,
        .avm_main_pc = FF(pc++),
        .avm_main_internal_return_ptr = FF(internal_return_ptr),
        .avm_main_sel_op_eq = FF(1),
        .avm_main_in_tag = FF(static_cast<uint32_t>(in_tag)),
        .avm_main_tag_err = FF(static_cast<uint32_t>(!tag_match)),
        .avm_main_ia = a,
        .avm_main_ib = b,
        .avm_main_ic = c,
        .avm_main_mem_op_a = FF(1),
        .avm_main_mem_op_b = FF(1),
        .avm_main_mem_op_c = FF(1),
        .avm_main_rwc = FF(1),
        .avm_main_ind_a = res.indirect_flag_a ? FF(a_offset) : FF(0),
        .avm_main_ind_b = res.indirect_flag_b ? FF(b_offset) : FF(0),
        .avm_main_ind_c = res.indirect_flag_c ? FF(dst_offset) : FF(0),
        .avm_main_ind_op_a = FF(static_cast<uint32_t>(res.indirect_flag_a)),
        .avm_main_ind_op_b = FF(static_cast<uint32_t>(res.indirect_flag_b)),
        .avm_main_ind_op_c = FF(static_cast<uint32_t>(res.indirect_flag_c)),
        .avm_main_mem_idx_a = FF(res.direct_a_offset),
        .avm_main_mem_idx_b = FF(res.direct_b_offset),
        .avm_main_mem_idx_c = FF(res.direct_dst_offset),
    });
}

// TODO: Finish SET opcode implementation. This is a partial implementation
// facilitating testing of arithmetic operations over non finite field types.
// We add an entry in the memory trace and a simplified one in the main trace
// without operation selector.
// TODO: PIL relations for the SET opcode need to be implemented.
// No check is performed that val pertains to type defined by in_tag.
/**
 * @brief Set a constant from bytecode with direct memory access.
 *
 * @param val The constant to be written upcasted to u128
 * @param dst_offset Memory destination offset where val is written to
 * @param in_tag The instruction memory tag
 */
void AvmTraceBuilder::set(uint128_t val, uint32_t dst_offset, AvmMemoryTag in_tag)
{
    auto clk = static_cast<uint32_t>(main_trace.size());
    auto val_ff = FF{ uint256_t::from_uint128(val) };

    mem_trace_builder.write_into_memory(clk, IntermRegister::IC, dst_offset, val_ff, in_tag);

    main_trace.push_back(Row{
        .avm_main_clk = clk,
        .avm_main_pc = FF(pc++),
        .avm_main_internal_return_ptr = FF(internal_return_ptr),
        .avm_main_in_tag = FF(static_cast<uint32_t>(in_tag)),
        .avm_main_ic = val_ff,
        .avm_main_mem_op_c = FF(1),
        .avm_main_rwc = FF(1),
        .avm_main_mem_idx_c = FF(dst_offset),
    });
}

/**
 * @brief Copy value and tag from a memory cell at position src_offset to the
 *        memory cell at position dst_offset
 *
 * @param indirect A byte encoding information about indirect/direct memory access.
 * @param src_offset Offset of source memory cell
 * @param dst_offset Offset of destination memory cell
 */
void AvmTraceBuilder::op_mov(uint8_t indirect, uint32_t src_offset, uint32_t dst_offset)
{
    auto const clk = static_cast<uint32_t>(main_trace.size());
    bool tag_match = true;
    uint32_t direct_src_offset = src_offset;
    uint32_t direct_dst_offset = dst_offset;

    bool indirect_src_flag = is_operand_indirect(indirect, 0);
    bool indirect_dst_flag = is_operand_indirect(indirect, 1);

    if (indirect_src_flag) {
        auto read_ind_a =
            mem_trace_builder.indirect_read_and_load_from_memory(clk, IndirectRegister::IND_A, src_offset);
        tag_match = read_ind_a.tag_match;
        direct_src_offset = uint32_t(read_ind_a.val);
    }

    if (indirect_dst_flag) {
        auto read_ind_c =
            mem_trace_builder.indirect_read_and_load_from_memory(clk, IndirectRegister::IND_C, dst_offset);
        tag_match = tag_match && read_ind_c.tag_match;
        direct_dst_offset = uint32_t(read_ind_c.val);
    }

    // Reading from memory and loading into ia without tag check.
    auto const [val, tag] = mem_trace_builder.read_and_load_mov_opcode(clk, direct_src_offset);

    // Write into memory from intermediate register ic.
    mem_trace_builder.write_into_memory(clk, IntermRegister::IC, direct_dst_offset, val, tag);

    main_trace.push_back(Row{
        .avm_main_clk = clk,
        .avm_main_pc = pc++,
        .avm_main_internal_return_ptr = internal_return_ptr,
        .avm_main_sel_mov = 1,
        .avm_main_in_tag = static_cast<uint32_t>(tag),
        .avm_main_tag_err = static_cast<uint32_t>(!tag_match),
        .avm_main_ia = val,
        .avm_main_ic = val,
        .avm_main_mem_op_a = 1,
        .avm_main_mem_op_c = 1,
        .avm_main_rwc = 1,
        .avm_main_ind_a = indirect_src_flag ? src_offset : 0,
        .avm_main_ind_c = indirect_dst_flag ? dst_offset : 0,
        .avm_main_ind_op_a = static_cast<uint32_t>(indirect_src_flag),
        .avm_main_ind_op_c = static_cast<uint32_t>(indirect_dst_flag),
        .avm_main_mem_idx_a = direct_src_offset,
        .avm_main_mem_idx_c = direct_dst_offset,
    });
}

/**
 * @brief CALLDATACOPY opcode with direct memory access, i.e.,
 *        direct: M[dst_offset:dst_offset+copy_size] = calldata[cd_offset:cd_offset+copy_size]
 *        indirect: M[M[dst_offset]:M[dst_offset]+copy_size] = calldata[cd_offset:cd_offset+copy_size]
 *        Simplified version with exclusively memory store operations and
 *        values from calldata passed by an array and loaded into
 *        intermediate registers.
 *        Assume that caller passes call_data_mem which is large enough so that
 *        no out-of-bound memory issues occur.
 *        TODO: taking care of intermediate register values consistency and propagating their
 *        values to the next row when not overwritten.
 *        TODO: error handling if dst_offset + copy_size > 2^32 which would lead to
 *              out-of-bound memory write. Similarly, if cd_offset + copy_size is larger
 *              than call_data_mem.size()
 *
 * @param indirect A byte encoding information about indirect/direct memory access.
 * @param cd_offset The starting index of the region in calldata to be copied.
 * @param copy_size The number of finite field elements to be copied into memory.
 * @param dst_offset The starting index of memory where calldata will be copied to.
 * @param call_data_mem The vector containing calldata.
 */
void AvmTraceBuilder::calldata_copy(
    uint8_t indirect, uint32_t cd_offset, uint32_t copy_size, uint32_t dst_offset, std::vector<FF> const& call_data_mem)
{
    // We parallelize storing memory operations in chunk of 3, i.e., 1 per intermediate register.
    // The variable pos is an index pointing to the first storing operation (pertaining to intermediate
    // register Ia) relative to cd_offset:
    // cd_offset + pos:       Ia memory store operation
    // cd_offset + pos + 1:   Ib memory store operation
    // cd_offset + pos + 2:   Ic memory store operation

    uint32_t pos = 0;
    uint32_t direct_dst_offset = dst_offset; // Will be overwritten in indirect mode.

    while (pos < copy_size) {
        FF ib(0);
        FF ic(0);
        uint32_t mem_op_b(0);
        uint32_t mem_op_c(0);
        uint32_t mem_idx_b(0);
        uint32_t mem_idx_c(0);
        uint32_t rwb(0);
        uint32_t rwc(0);
        auto clk = static_cast<uint32_t>(main_trace.size());

        FF ia = call_data_mem.at(cd_offset + pos);
        uint32_t mem_op_a(1);
        uint32_t rwa = 1;

        bool indirect_flag = false;
        bool tag_match = true;

        if (pos == 0 && is_operand_indirect(indirect, 0)) {
            indirect_flag = true;
            auto ind_read =
                mem_trace_builder.indirect_read_and_load_from_memory(clk, IndirectRegister::IND_A, dst_offset);
            direct_dst_offset = uint32_t(ind_read.val);
            tag_match = ind_read.tag_match;
        }

        uint32_t mem_idx_a = direct_dst_offset + pos;

        // Storing from Ia
        mem_trace_builder.write_into_memory(clk, IntermRegister::IA, mem_idx_a, ia, AvmMemoryTag::FF);

        if (copy_size - pos > 1) {
            ib = call_data_mem.at(cd_offset + pos + 1);
            mem_op_b = 1;
            mem_idx_b = direct_dst_offset + pos + 1;
            rwb = 1;

            // Storing from Ib
            mem_trace_builder.write_into_memory(clk, IntermRegister::IB, mem_idx_b, ib, AvmMemoryTag::FF);
        }

        if (copy_size - pos > 2) {
            ic = call_data_mem.at(cd_offset + pos + 2);
            mem_op_c = 1;
            mem_idx_c = direct_dst_offset + pos + 2;
            rwc = 1;

            // Storing from Ic
            mem_trace_builder.write_into_memory(clk, IntermRegister::IC, mem_idx_c, ic, AvmMemoryTag::FF);
        }

        main_trace.push_back(Row{
            .avm_main_clk = clk,
            .avm_main_pc = FF(pc++),
            .avm_main_internal_return_ptr = FF(internal_return_ptr),
            .avm_main_in_tag = FF(static_cast<uint32_t>(AvmMemoryTag::FF)),
            .avm_main_tag_err = FF(static_cast<uint32_t>(!tag_match)),
            .avm_main_ia = ia,
            .avm_main_ib = ib,
            .avm_main_ic = ic,
            .avm_main_mem_op_a = FF(mem_op_a),
            .avm_main_mem_op_b = FF(mem_op_b),
            .avm_main_mem_op_c = FF(mem_op_c),
            .avm_main_rwa = FF(rwa),
            .avm_main_rwb = FF(rwb),
            .avm_main_rwc = FF(rwc),
            .avm_main_ind_a = indirect_flag ? FF(dst_offset) : FF(0),
            .avm_main_ind_op_a = FF(indirect_flag),
            .avm_main_mem_idx_a = FF(mem_idx_a),
            .avm_main_mem_idx_b = FF(mem_idx_b),
            .avm_main_mem_idx_c = FF(mem_idx_c),
        });

        if (copy_size - pos > 2) { // Guard to prevent overflow if copy_size is close to uint32_t maximum value.
            pos += 3;
        } else {
            pos = copy_size;
        }
    }
}

/**
 * @brief RETURN opcode with direct and indirect memory access, i.e.,
 *        direct:   return(M[ret_offset:ret_offset+ret_size])
 *        indirect: return(M[M[ret_offset]:M[ret_offset]+ret_size])
 *        Simplified version with exclusively memory load operations into
 *        intermediate registers and then values are copied to the returned vector.
 *        TODO: taking care of flagging this row as the last one? Special STOP flag?
 *        TODO: error handling if ret_offset + ret_size > 2^32 which would lead to
 *              out-of-bound memory read.
 *
 * @param indirect A byte encoding information about indirect/direct memory access.
 * @param ret_offset The starting index of the memory region to be returned.
 * @param ret_size The number of elements to be returned.
 * @return The returned memory region as a std::vector.
 */
std::vector<FF> AvmTraceBuilder::return_op(uint8_t indirect, uint32_t ret_offset, uint32_t ret_size)
{
    if (ret_size == 0) {
        halt();
        return {};
    }

    // We parallelize loading memory operations in chunk of 3, i.e., 1 per intermediate register.
    // The variable pos is an index pointing to the first storing operation (pertaining to intermediate
    // register Ia) relative to ret_offset:
    // ret_offset + pos:       Ia memory load operation
    // ret_offset + pos + 1:   Ib memory load operation
    // ret_offset + pos + 2:   Ic memory load operation
    // In indirect mode, ret_offset is first resolved by the first indirect load.

    uint32_t pos = 0;
    std::vector<FF> returnMem;
    uint32_t direct_ret_offset = ret_offset; // Will be overwritten in indirect mode.

    while (pos < ret_size) {
        FF ib(0);
        FF ic(0);
        uint32_t mem_op_b(0);
        uint32_t mem_op_c(0);
        uint32_t mem_idx_b(0);
        uint32_t mem_idx_c(0);
        auto clk = static_cast<uint32_t>(main_trace.size());

        uint32_t mem_op_a(1);
        bool indirect_flag = false;
        bool tag_match = true;

        if (pos == 0 && is_operand_indirect(indirect, 0)) {
            indirect_flag = true;
            auto ind_read =
                mem_trace_builder.indirect_read_and_load_from_memory(clk, IndirectRegister::IND_A, ret_offset);
            direct_ret_offset = uint32_t(ind_read.val);
            tag_match = ind_read.tag_match;
        }

        uint32_t mem_idx_a = direct_ret_offset + pos;

        // Reading and loading to Ia
        auto read_a = mem_trace_builder.read_and_load_from_memory(clk, IntermRegister::IA, mem_idx_a, AvmMemoryTag::FF);
        tag_match = tag_match && read_a.tag_match;

        FF ia = read_a.val;
        returnMem.push_back(ia);

        if (ret_size - pos > 1) {
            mem_op_b = 1;
            mem_idx_b = direct_ret_offset + pos + 1;

            // Reading and loading to Ib
            auto read_b =
                mem_trace_builder.read_and_load_from_memory(clk, IntermRegister::IB, mem_idx_b, AvmMemoryTag::FF);
            tag_match = tag_match && read_b.tag_match;
            ib = read_b.val;
            returnMem.push_back(ib);
        }

        if (ret_size - pos > 2) {
            mem_op_c = 1;
            mem_idx_c = direct_ret_offset + pos + 2;

            // Reading and loading to Ic
            auto read_c =
                mem_trace_builder.read_and_load_from_memory(clk, IntermRegister::IC, mem_idx_c, AvmMemoryTag::FF);
            tag_match = tag_match && read_c.tag_match;
            ic = read_c.val;
            returnMem.push_back(ic);
        }

        main_trace.push_back(Row{
            .avm_main_clk = clk,
            .avm_main_pc = FF(pc),
            .avm_main_internal_return_ptr = FF(internal_return_ptr),
            .avm_main_sel_halt = FF(1),
            .avm_main_in_tag = FF(static_cast<uint32_t>(AvmMemoryTag::FF)),
            .avm_main_tag_err = FF(static_cast<uint32_t>(!tag_match)),
            .avm_main_ia = ia,
            .avm_main_ib = ib,
            .avm_main_ic = ic,
            .avm_main_mem_op_a = FF(mem_op_a),
            .avm_main_mem_op_b = FF(mem_op_b),
            .avm_main_mem_op_c = FF(mem_op_c),
            .avm_main_ind_a = indirect_flag ? FF(ret_offset) : FF(0),
            .avm_main_ind_op_a = FF(static_cast<uint32_t>(indirect_flag)),
            .avm_main_mem_idx_a = FF(mem_idx_a),
            .avm_main_mem_idx_b = FF(mem_idx_b),
            .avm_main_mem_idx_c = FF(mem_idx_c),
        });

        if (ret_size - pos > 2) { // Guard to prevent overflow if ret_size is close to uint32_t maximum value.
            pos += 3;
        } else {
            pos = ret_size;
        }
    }
    pc = UINT32_MAX; // This ensures that no subsequent opcode will be executed.
    return returnMem;
}

/**
 * @brief HALT opcode
 *        This opcode effectively stops program execution, and is used in the relation that
 *        ensures the program counter increments on each opcode.
 *        i.e. the program counter should freeze and the halt flag is set to 1.
 */
void AvmTraceBuilder::halt()
{
    auto clk = main_trace.size();

    main_trace.push_back(Row{
        .avm_main_clk = clk,
        .avm_main_pc = FF(pc),
        .avm_main_internal_return_ptr = FF(internal_return_ptr),
        .avm_main_sel_halt = FF(1),
    });

    pc = UINT32_MAX; // This ensures that no subsequent opcode will be executed.
}

/**
 * @brief JUMP OPCODE
 *        Jumps to a new `jmp_dest`
 *        This function must:
 *          - Set the next program counter to the provided `jmp_dest`.
 *
 * @param jmp_dest - The destination to jump to
 */
void AvmTraceBuilder::jump(uint32_t jmp_dest)
{
    auto clk = main_trace.size();

    main_trace.push_back(Row{
        .avm_main_clk = clk,
        .avm_main_pc = FF(pc),
        .avm_main_internal_return_ptr = FF(internal_return_ptr),
        .avm_main_sel_jump = FF(1),
        .avm_main_ia = FF(jmp_dest),
    });

    // Adjust parameters for the next row
    pc = jmp_dest;
}

/**
 * @brief INTERNAL_CALL OPCODE
 *        This opcode effectively jumps to a new `jmp_dest` and stores the return program counter
 *        (current program counter + 1) onto a call stack.
 *        This function must:
 *          - Set the next program counter to the provided `jmp_dest`.
 *          - Store the current `pc` + 1 onto the call stack (emulated in memory)
 *          - Increment the return stack pointer (a pointer to where the call stack is in memory)
 *
 *        Note: We use intermediate register to perform memory storage operations.
 *
 * @param jmp_dest - The destination to jump to
 */
void AvmTraceBuilder::internal_call(uint32_t jmp_dest)
{
    auto clk = static_cast<uint32_t>(main_trace.size());

    // We store the next instruction as the return location
    uint32_t stored_pc = pc + 1;
    internal_call_stack.push(stored_pc);

    // Add the return location to the memory trace
    mem_trace_builder.write_into_memory(clk, IntermRegister::IB, internal_return_ptr, FF(stored_pc), AvmMemoryTag::U32);

    main_trace.push_back(Row{
        .avm_main_clk = clk,
        .avm_main_pc = FF(pc),
        .avm_main_internal_return_ptr = FF(internal_return_ptr),
        .avm_main_sel_internal_call = FF(1),
        .avm_main_in_tag = FF(static_cast<uint32_t>(AvmMemoryTag::U32)),
        .avm_main_ia = FF(jmp_dest),
        .avm_main_ib = FF(stored_pc),
        .avm_main_mem_op_b = FF(1),
        .avm_main_rwb = FF(1),
        .avm_main_mem_idx_b = FF(internal_return_ptr),
    });

    // Adjust parameters for the next row
    pc = jmp_dest;
    internal_return_ptr++;
}

/**
 * @brief INTERNAL_RETURN OPCODE
 *        The opcode returns from an internal call.
 *        This function must:
 *          - Read the return location from the internal_return_ptr
 *          - Set the next program counter to the return location
 *          - Decrement the return stack pointer
 *
 *  TODO(https://github.com/AztecProtocol/aztec-packages/issues/3740): This function MUST come after a call instruction.
 */
void AvmTraceBuilder::internal_return()
{
    auto clk = static_cast<uint32_t>(main_trace.size());

    // Internal return pointer is decremented
    // We want to load the value pointed by the internal pointer
    auto read_a = mem_trace_builder.read_and_load_from_memory(
        clk, IntermRegister::IA, internal_return_ptr - 1, AvmMemoryTag::U32);

    main_trace.push_back(Row{
        .avm_main_clk = clk,
        .avm_main_pc = pc,
        .avm_main_internal_return_ptr = FF(internal_return_ptr),
        .avm_main_sel_internal_return = FF(1),
        .avm_main_in_tag = FF(static_cast<uint32_t>(AvmMemoryTag::U32)),
        .avm_main_tag_err = FF(static_cast<uint32_t>(!read_a.tag_match)),
        .avm_main_ia = read_a.val,
        .avm_main_mem_op_a = FF(1),
        .avm_main_rwa = FF(0),
        .avm_main_mem_idx_a = FF(internal_return_ptr - 1),
    });

    // We want the next row to be the one pointed by jmp_dest
    // The next pc should be from the top of the internal call stack + 1
    pc = internal_call_stack.top();
    internal_call_stack.pop();
    internal_return_ptr--;
}

// Finalise Lookup Counts
//
// For log derivative lookups, we require a column that contains the number of times each lookup is consumed
// As we build the trace, we keep track of the reads made in a mapping, so that they can be applied to the
// counts column here
//
// NOTE: its coupled to pil - this is not the final iteration
void AvmTraceBuilder::finalise_mem_trace_lookup_counts()
{
    for (auto const& [clk, count] : mem_trace_builder.m_tag_err_lookup_counts) {
        main_trace.at(clk).incl_main_tag_err_counts = count;
    }
}

/**
 * @brief Finalisation of the memory trace and incorporating it to the main trace.
 *        In particular, sorting the memory trace, setting .m_lastAccess and
 *        adding shifted values (first row). The main trace is moved at the end of
 *        this call.
 *
 * @return The main trace
 */
std::vector<Row> AvmTraceBuilder::finalize()
{
    auto mem_trace = mem_trace_builder.finalize();
    auto alu_trace = alu_trace_builder.finalize();
    size_t mem_trace_size = mem_trace.size();
    size_t main_trace_size = main_trace.size();
    size_t alu_trace_size = alu_trace.size();

    // Get tag_err counts from the mem_trace_builder
    finalise_mem_trace_lookup_counts();

    // TODO: We will have to handle this through error handling and not an assertion
    // Smaller than N because we have to add an extra initial row to support shifted
    // elements
    assert(mem_trace_size < AVM_TRACE_SIZE);
    assert(main_trace_size < AVM_TRACE_SIZE);
    assert(alu_trace_size < AVM_TRACE_SIZE);

    // Fill the rest with zeros.
    size_t zero_rows_num = AVM_TRACE_SIZE - main_trace_size - 1;
    while (zero_rows_num-- > 0) {
        main_trace.push_back({});
    }

    main_trace.at(main_trace_size - 1).avm_main_last = FF(1);

    // Memory trace inclusion
    for (size_t i = 0; i < mem_trace_size; i++) {
        auto const& src = mem_trace.at(i);
        auto& dest = main_trace.at(i);

        dest.avm_mem_m_clk = FF(src.m_clk);
        dest.avm_mem_m_sub_clk = FF(src.m_sub_clk);
        dest.avm_mem_m_addr = FF(src.m_addr);
        dest.avm_mem_m_val = src.m_val;
        dest.avm_mem_m_rw = FF(static_cast<uint32_t>(src.m_rw));
        dest.avm_mem_m_in_tag = FF(static_cast<uint32_t>(src.m_in_tag));
        dest.avm_mem_m_tag = FF(static_cast<uint32_t>(src.m_tag));
        dest.avm_mem_m_tag_err = FF(static_cast<uint32_t>(src.m_tag_err));
        dest.avm_mem_m_one_min_inv = src.m_one_min_inv;
        dest.avm_mem_m_sel_mov = FF(static_cast<uint32_t>(src.m_sel_mov));

        dest.incl_mem_tag_err_counts = FF(static_cast<uint32_t>(src.m_tag_err_count_relevant));

        switch (src.m_sub_clk) {
        case AvmMemTraceBuilder::SUB_CLK_LOAD_A:
        case AvmMemTraceBuilder::SUB_CLK_STORE_A:
            dest.avm_mem_m_op_a = 1;
            break;
        case AvmMemTraceBuilder::SUB_CLK_LOAD_B:
        case AvmMemTraceBuilder::SUB_CLK_STORE_B:
            dest.avm_mem_m_op_b = 1;
            break;
        case AvmMemTraceBuilder::SUB_CLK_LOAD_C:
        case AvmMemTraceBuilder::SUB_CLK_STORE_C:
            dest.avm_mem_m_op_c = 1;
            break;
        case AvmMemTraceBuilder::SUB_CLK_IND_LOAD_A:
            dest.avm_mem_m_ind_op_a = 1;
            break;
        case AvmMemTraceBuilder::SUB_CLK_IND_LOAD_B:
            dest.avm_mem_m_ind_op_b = 1;
            break;
        case AvmMemTraceBuilder::SUB_CLK_IND_LOAD_C:
            dest.avm_mem_m_ind_op_c = 1;
            break;
        default:
            break;
        }

        if (i + 1 < mem_trace_size) {
            auto const& next = mem_trace.at(i + 1);
            dest.avm_mem_m_lastAccess = FF(static_cast<uint32_t>(src.m_addr != next.m_addr));
        } else {
            dest.avm_mem_m_lastAccess = FF(1);
            dest.avm_mem_m_last = FF(1);
        }
    }

    // Alu trace inclusion
    for (size_t i = 0; i < alu_trace_size; i++) {
        auto const& src = alu_trace.at(i);
        auto& dest = main_trace.at(i);

        dest.avm_alu_alu_clk = FF(static_cast<uint32_t>(src.alu_clk));

        dest.avm_alu_alu_op_add = FF(static_cast<uint32_t>(src.alu_op_add));
        dest.avm_alu_alu_op_sub = FF(static_cast<uint32_t>(src.alu_op_sub));
        dest.avm_alu_alu_op_mul = FF(static_cast<uint32_t>(src.alu_op_mul));
        dest.avm_alu_alu_op_not = FF(static_cast<uint32_t>(src.alu_op_not));
        dest.avm_alu_alu_op_eq = FF(static_cast<uint32_t>(src.alu_op_eq));

        dest.avm_alu_alu_ff_tag = FF(static_cast<uint32_t>(src.alu_ff_tag));
        dest.avm_alu_alu_u8_tag = FF(static_cast<uint32_t>(src.alu_u8_tag));
        dest.avm_alu_alu_u16_tag = FF(static_cast<uint32_t>(src.alu_u16_tag));
        dest.avm_alu_alu_u32_tag = FF(static_cast<uint32_t>(src.alu_u32_tag));
        dest.avm_alu_alu_u64_tag = FF(static_cast<uint32_t>(src.alu_u64_tag));
        dest.avm_alu_alu_u128_tag = FF(static_cast<uint32_t>(src.alu_u128_tag));

        dest.avm_alu_alu_in_tag = dest.avm_alu_alu_u8_tag + FF(2) * dest.avm_alu_alu_u16_tag +
                                  FF(3) * dest.avm_alu_alu_u32_tag + FF(4) * dest.avm_alu_alu_u64_tag +
                                  FF(5) * dest.avm_alu_alu_u128_tag + FF(6) * dest.avm_alu_alu_ff_tag;

        dest.avm_alu_alu_ia = src.alu_ia;
        dest.avm_alu_alu_ib = src.alu_ib;
        dest.avm_alu_alu_ic = src.alu_ic;

        dest.avm_alu_alu_cf = FF(static_cast<uint32_t>(src.alu_cf));

        dest.avm_alu_alu_u8_r0 = FF(src.alu_u8_r0);
        dest.avm_alu_alu_u8_r1 = FF(src.alu_u8_r1);

        dest.avm_alu_alu_u16_r0 = FF(src.alu_u16_reg.at(0));
        dest.avm_alu_alu_u16_r1 = FF(src.alu_u16_reg.at(1));
        dest.avm_alu_alu_u16_r2 = FF(src.alu_u16_reg.at(2));
        dest.avm_alu_alu_u16_r3 = FF(src.alu_u16_reg.at(3));
        dest.avm_alu_alu_u16_r4 = FF(src.alu_u16_reg.at(4));
        dest.avm_alu_alu_u16_r5 = FF(src.alu_u16_reg.at(5));
        dest.avm_alu_alu_u16_r6 = FF(src.alu_u16_reg.at(6));
        dest.avm_alu_alu_u16_r7 = FF(src.alu_u16_reg.at(7));

        dest.avm_alu_alu_u64_r0 = FF(src.alu_u64_r0);
        dest.avm_alu_alu_op_eq_diff_inv = FF(src.alu_op_eq_diff_inv);

        // Not all rows in ALU are enabled with a selector. For instance,
        // multiplication over u128 is taking two lines.
        if (dest.avm_alu_alu_op_add == FF(1) || dest.avm_alu_alu_op_sub == FF(1) || dest.avm_alu_alu_op_mul == FF(1) ||
            dest.avm_alu_alu_op_eq == FF(1) || dest.avm_alu_alu_op_not == FF(1)) {
            dest.avm_alu_alu_sel = FF(1);
        }
    }

    // Deriving redundant selectors/tags for the main trace.
    for (Row& r : main_trace) {
        if ((r.avm_main_sel_op_add == FF(1) || r.avm_main_sel_op_sub == FF(1) || r.avm_main_sel_op_mul == FF(1) ||
             r.avm_main_sel_op_eq == FF(1) || r.avm_main_sel_op_not == FF(1)) &&
            r.avm_main_tag_err == FF(0)) {
            r.avm_main_alu_sel = FF(1);
        }
    }

    // Adding extra row for the shifted values at the top of the execution trace.
    Row first_row = Row{ .avm_main_first = FF(1), .avm_mem_m_lastAccess = FF(1) };
    main_trace.insert(main_trace.begin(), first_row);

    auto trace = std::move(main_trace);
    reset();

    return trace;
}

} // namespace bb::avm_trace
