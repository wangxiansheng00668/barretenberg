#include "goblin_ultra_circuit_builder.hpp"
#include "barretenberg/crypto/poseidon2/poseidon2_params.hpp"
#include "barretenberg/flavor/goblin_ultra.hpp"
#include <barretenberg/plonk/proof_system/constants.hpp>
#include <unordered_map>
#include <unordered_set>

using namespace bb;
using namespace bb::crypto;

namespace bb {

template <typename FF> void GoblinUltraCircuitBuilder_<FF>::finalize_circuit()
{
    UltraCircuitBuilder_<UltraHonkArith<FF>>::finalize_circuit();
}

/**
 * @brief Ensure all polynomials have at least one non-zero coefficient to avoid commiting to the zero-polynomial
 *
 * @param in Structure containing variables and witness selectors
 */
// TODO(#423): This function adds valid (but arbitrary) gates to ensure that the circuit which includes
// them will not result in any zero-polynomials. It also ensures that the first coefficient of the wire
// polynomials is zero, which is required for them to be shiftable.
template <typename FF> void GoblinUltraCircuitBuilder_<FF>::add_gates_to_ensure_all_polys_are_non_zero()
{
    // Most polynomials are handled via the conventional Ultra method
    UltraCircuitBuilder_<UltraHonkArith<FF>>::add_gates_to_ensure_all_polys_are_non_zero();

    // All that remains is to handle databus related and poseidon2 related polynomials. In what follows we populate the
    // calldata with some mock data then constuct a single calldata read gate

    // Create an arbitrary calldata read gate
    add_public_calldata(FF(25)); // ensure there is at least one entry in calldata
    uint32_t raw_read_idx = 0;   // read first entry in calldata
    auto read_idx = this->add_variable(raw_read_idx);
    FF calldata_value = this->get_variable(public_calldata[raw_read_idx]);
    auto value_idx = this->add_variable(calldata_value);
    create_calldata_lookup_gate({ read_idx, value_idx });
    // TODO(https://github.com/AztecProtocol/barretenberg/issues/821): automate updating of read counts
    calldata_read_counts[raw_read_idx]++;

    // mock a poseidon external gate, with all zeros as input
    this->blocks.poseidon_external.populate_wires(this->zero_idx, this->zero_idx, this->zero_idx, this->zero_idx);
    this->blocks.poseidon_external.q_m().emplace_back(0);
    this->blocks.poseidon_external.q_1().emplace_back(0);
    this->blocks.poseidon_external.q_2().emplace_back(0);
    this->blocks.poseidon_external.q_3().emplace_back(0);
    this->blocks.poseidon_external.q_c().emplace_back(0);
    this->blocks.poseidon_external.q_arith().emplace_back(0);
    this->blocks.poseidon_external.q_4().emplace_back(0);
    this->blocks.poseidon_external.q_sort().emplace_back(0);
    this->blocks.poseidon_external.q_lookup_type().emplace_back(0);
    this->blocks.poseidon_external.q_elliptic().emplace_back(0);
    this->blocks.poseidon_external.q_aux().emplace_back(0);
    this->blocks.poseidon_external.q_busread().emplace_back(0);
    this->blocks.poseidon_external.q_poseidon2_external().emplace_back(1);
    this->blocks.poseidon_external.q_poseidon2_internal().emplace_back(0);
    this->check_selector_length_consistency();
    ++this->num_gates;

    // dummy gate to be read into by previous poseidon external gate via shifts
    this->create_dummy_gate(
        this->blocks.poseidon_external, this->zero_idx, this->zero_idx, this->zero_idx, this->zero_idx);

    // mock a poseidon internal gate, with all zeros as input
    this->blocks.poseidon_internal.populate_wires(this->zero_idx, this->zero_idx, this->zero_idx, this->zero_idx);
    this->blocks.poseidon_internal.q_m().emplace_back(0);
    this->blocks.poseidon_internal.q_1().emplace_back(0);
    this->blocks.poseidon_internal.q_2().emplace_back(0);
    this->blocks.poseidon_internal.q_3().emplace_back(0);
    this->blocks.poseidon_internal.q_c().emplace_back(0);
    this->blocks.poseidon_internal.q_arith().emplace_back(0);
    this->blocks.poseidon_internal.q_4().emplace_back(0);
    this->blocks.poseidon_internal.q_sort().emplace_back(0);
    this->blocks.poseidon_internal.q_lookup_type().emplace_back(0);
    this->blocks.poseidon_internal.q_elliptic().emplace_back(0);
    this->blocks.poseidon_internal.q_aux().emplace_back(0);
    this->blocks.poseidon_internal.q_busread().emplace_back(0);
    this->blocks.poseidon_internal.q_poseidon2_external().emplace_back(0);
    this->blocks.poseidon_internal.q_poseidon2_internal().emplace_back(1);
    this->check_selector_length_consistency();
    ++this->num_gates;

    // dummy gate to be read into by previous poseidon internal gate via shifts
    this->create_dummy_gate(
        this->blocks.poseidon_internal, this->zero_idx, this->zero_idx, this->zero_idx, this->zero_idx);
}

/**
 * @brief Add gates for simple point addition (no mul) and add the raw operation data to the op queue
 *
 * @param point Point to be added into the accumulator
 */
template <typename FF>
ecc_op_tuple GoblinUltraCircuitBuilder_<FF>::queue_ecc_add_accum(const bb::g1::affine_element& point)
{
    // Add raw op to queue
    op_queue->add_accumulate(point);

    // Decompose operation inputs into width-four form and add ecc op gates
    auto op_tuple = decompose_ecc_operands(add_accum_op_idx, point);
    populate_ecc_op_wires(op_tuple);

    return op_tuple;
}

/**
 * @brief Add gates for point mul-then-accumulate and add the raw operation data to the op queue
 *
 * @tparam FF
 * @param point
 * @param scalar The scalar by which point is multiplied prior to being accumulated
 * @return ecc_op_tuple encoding the point and scalar inputs to the mul accum
 */
template <typename FF>
ecc_op_tuple GoblinUltraCircuitBuilder_<FF>::queue_ecc_mul_accum(const bb::g1::affine_element& point, const FF& scalar)
{
    // Add raw op to op queue
    op_queue->mul_accumulate(point, scalar);

    // Decompose operation inputs into width-four form and add ecc op gates
    auto op_tuple = decompose_ecc_operands(mul_accum_op_idx, point, scalar);
    populate_ecc_op_wires(op_tuple);

    return op_tuple;
}

/**
 * @brief Add point equality gates based on the current value of the accumulator internal to the op queue and add the
 * raw operation data to the op queue
 *
 * @return ecc_op_tuple encoding the point to which equality has been asserted
 */
template <typename FF> ecc_op_tuple GoblinUltraCircuitBuilder_<FF>::queue_ecc_eq()
{
    // Add raw op to op queue
    auto point = op_queue->eq();

    // Decompose operation inputs into width-four form and add ecc op gates
    auto op_tuple = decompose_ecc_operands(equality_op_idx, point);
    populate_ecc_op_wires(op_tuple);

    return op_tuple;
}

/**
 * @brief Decompose ecc operands into components, add corresponding variables, return ecc op tuple
 *
 * @param op_idx Index of op code in variables array
 * @param point
 * @param scalar
 * @return ecc_op_tuple Tuple of indices into variables array used to construct pair of ecc op gates
 */
template <typename FF>
ecc_op_tuple GoblinUltraCircuitBuilder_<FF>::decompose_ecc_operands(uint32_t op_idx,
                                                                    const g1::affine_element& point,
                                                                    const FF& scalar)
{
    // Decompose point coordinates (Fq) into hi-lo chunks (Fr)
    const size_t CHUNK_SIZE = 2 * DEFAULT_NON_NATIVE_FIELD_LIMB_BITS;
    auto x_256 = uint256_t(point.x);
    auto y_256 = uint256_t(point.y);
    auto x_lo = FF(x_256.slice(0, CHUNK_SIZE));
    auto x_hi = FF(x_256.slice(CHUNK_SIZE, CHUNK_SIZE * 2));
    auto y_lo = FF(y_256.slice(0, CHUNK_SIZE));
    auto y_hi = FF(y_256.slice(CHUNK_SIZE, CHUNK_SIZE * 2));

    // Split scalar into 128 bit endomorphism scalars
    FF z_1 = 0;
    FF z_2 = 0;
    auto converted = scalar.from_montgomery_form();
    FF::split_into_endomorphism_scalars(converted, z_1, z_2);
    z_1 = z_1.to_montgomery_form();
    z_2 = z_2.to_montgomery_form();

    // Populate ultra ops in OpQueue with the decomposed operands
    op_queue->populate_ultra_ops({ this->variables[op_idx], x_lo, x_hi, y_lo, y_hi, z_1, z_2 });

    // Add variables for decomposition and get indices needed for op wires
    auto x_lo_idx = this->add_variable(x_lo);
    auto x_hi_idx = this->add_variable(x_hi);
    auto y_lo_idx = this->add_variable(y_lo);
    auto y_hi_idx = this->add_variable(y_hi);
    auto z_1_idx = this->add_variable(z_1);
    auto z_2_idx = this->add_variable(z_2);

    return { op_idx, x_lo_idx, x_hi_idx, y_lo_idx, y_hi_idx, z_1_idx, z_2_idx };
}

/**
 * @brief Add ecc operation to queue
 *
 * @param in Variables array indices corresponding to operation inputs
 * @note We dont explicitly set values for the selectors here since their values are fully determined by
 * num_ecc_op_gates. E.g. in the composer we can reconstruct q_ecc_op as the indicator on the first num_ecc_op_gates
 * indices. All other selectors are simply 0 on this domain.
 */
template <typename FF> void GoblinUltraCircuitBuilder_<FF>::populate_ecc_op_wires(const ecc_op_tuple& in)
{
    this->blocks.ecc_op.populate_wires(in.op, in.x_lo, in.x_hi, in.y_lo);
    for (auto& selector : this->blocks.ecc_op.selectors) {
        selector.emplace_back(0);
    }

    this->blocks.ecc_op.populate_wires(this->zero_idx, in.y_hi, in.z_1, in.z_2);
    for (auto& selector : this->blocks.ecc_op.selectors) {
        selector.emplace_back(0);
    }

    num_ecc_op_gates += 2;
};

template <typename FF> void GoblinUltraCircuitBuilder_<FF>::set_goblin_ecc_op_code_constant_variables()
{
    null_op_idx = this->zero_idx;
    add_accum_op_idx = this->put_constant_variable(FF(EccOpCode::ADD_ACCUM));
    mul_accum_op_idx = this->put_constant_variable(FF(EccOpCode::MUL_ACCUM));
    equality_op_idx = this->put_constant_variable(FF(EccOpCode::EQUALITY));
}

/**
 * @brief Create a calldata lookup/read gate
 *
 * @tparam FF
 * @param databus_lookup_gate_ witness indices corresponding to: calldata index, calldata value
 */
template <typename FF>
void GoblinUltraCircuitBuilder_<FF>::create_calldata_lookup_gate(const databus_lookup_gate_<FF>& in)
{
    auto& block = this->blocks.busread;
    block.populate_wires(in.value, in.index, this->zero_idx, this->zero_idx);
    block.q_busread().emplace_back(1);

    // populate all other components with zero
    block.q_m().emplace_back(0);
    block.q_1().emplace_back(0);
    block.q_2().emplace_back(0);
    block.q_3().emplace_back(0);
    block.q_c().emplace_back(0);
    block.q_sort().emplace_back(0);
    block.q_arith().emplace_back(0);
    block.q_4().emplace_back(0);
    block.q_lookup_type().emplace_back(0);
    block.q_elliptic().emplace_back(0);
    block.q_aux().emplace_back(0);
    block.q_poseidon2_external().emplace_back(0);
    block.q_poseidon2_internal().emplace_back(0);
    this->check_selector_length_consistency();

    ++this->num_gates;
}

/**
 * @brief Poseidon2 external round gate, activates the q_poseidon2_external selector and relation
 */
template <typename FF>
void GoblinUltraCircuitBuilder_<FF>::create_poseidon2_external_gate(const poseidon2_external_gate_<FF>& in)
{
    auto& block = this->blocks.poseidon_external;
    block.populate_wires(in.a, in.b, in.c, in.d);
    block.q_m().emplace_back(0);
    block.q_1().emplace_back(Poseidon2Bn254ScalarFieldParams::round_constants[in.round_idx][0]);
    block.q_2().emplace_back(Poseidon2Bn254ScalarFieldParams::round_constants[in.round_idx][1]);
    block.q_3().emplace_back(Poseidon2Bn254ScalarFieldParams::round_constants[in.round_idx][2]);
    block.q_c().emplace_back(0);
    block.q_arith().emplace_back(0);
    block.q_4().emplace_back(Poseidon2Bn254ScalarFieldParams::round_constants[in.round_idx][3]);
    block.q_sort().emplace_back(0);
    block.q_lookup_type().emplace_back(0);
    block.q_elliptic().emplace_back(0);
    block.q_aux().emplace_back(0);
    block.q_busread().emplace_back(0);
    block.q_poseidon2_external().emplace_back(1);
    block.q_poseidon2_internal().emplace_back(0);
    this->check_selector_length_consistency();
    ++this->num_gates;
}

/**
 * @brief Poseidon2 internal round gate, activates the q_poseidon2_internal selector and relation
 */
template <typename FF>
void GoblinUltraCircuitBuilder_<FF>::create_poseidon2_internal_gate(const poseidon2_internal_gate_<FF>& in)
{
    auto& block = this->blocks.poseidon_internal;
    block.populate_wires(in.a, in.b, in.c, in.d);
    block.q_m().emplace_back(0);
    block.q_1().emplace_back(Poseidon2Bn254ScalarFieldParams::round_constants[in.round_idx][0]);
    block.q_2().emplace_back(0);
    block.q_3().emplace_back(0);
    block.q_c().emplace_back(0);
    block.q_arith().emplace_back(0);
    block.q_4().emplace_back(0);
    block.q_sort().emplace_back(0);
    block.q_lookup_type().emplace_back(0);
    block.q_elliptic().emplace_back(0);
    block.q_aux().emplace_back(0);
    block.q_busread().emplace_back(0);
    block.q_poseidon2_external().emplace_back(0);
    block.q_poseidon2_internal().emplace_back(1);
    this->check_selector_length_consistency();
    ++this->num_gates;
}

template class GoblinUltraCircuitBuilder_<bb::fr>;
} // namespace bb