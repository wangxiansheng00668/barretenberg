#include "ultra_prover.hpp"
#include "barretenberg/sumcheck/sumcheck.hpp"

namespace bb {

/**
 * Create UltraProver_ from an instance.
 *
 * @param instance Instance whose proof we want to generate.
 *
 * @tparam a type of UltraFlavor
 * */
template <IsUltraFlavor Flavor>
UltraProver_<Flavor>::UltraProver_(const std::shared_ptr<Instance>& inst, const std::shared_ptr<Transcript>& transcript)
    : instance(std::move(inst))
    , transcript(transcript)
    , commitment_key(instance->proving_key->commitment_key)
    , oink_prover(inst, commitment_key, transcript, "")
{}

/**
 * Create UltraProver_ from a circuit.
 *
 * @param instance Circuit with witnesses whose validity we'd like to prove.
 *
 * @tparam a type of UltraFlavor
 * */
template <IsUltraFlavor Flavor>
UltraProver_<Flavor>::UltraProver_(Builder& circuit)
    : instance(std::make_shared<ProverInstance>(circuit))
    , transcript(std::make_shared<Transcript>())
    , commitment_key(instance->proving_key->commitment_key)
    , oink_prover(instance, commitment_key, transcript, "")
{}

/**
 * @brief Run Sumcheck resulting in u = (u_1,...,u_d) challenges and all evaluations at u being calculated.
 *
 */
template <IsUltraFlavor Flavor> void UltraProver_<Flavor>::execute_relation_check_rounds()
{
    using Sumcheck = SumcheckProver<Flavor>;
    auto circuit_size = instance->proving_key->circuit_size;
    auto sumcheck = Sumcheck(circuit_size, transcript);
    RelationSeparator alphas;
    for (size_t idx = 0; idx < alphas.size(); idx++) {
        alphas[idx] = transcript->template get_challenge<FF>("Sumcheck:alpha_" + std::to_string(idx));
    }
    instance->alphas = alphas;
    std::vector<FF> gate_challenges(numeric::get_msb(circuit_size));
    for (size_t idx = 0; idx < gate_challenges.size(); idx++) {
        gate_challenges[idx] = transcript->template get_challenge<FF>("Sumcheck:gate_challenge_" + std::to_string(idx));
    }
    instance->gate_challenges = gate_challenges;
    sumcheck_output = sumcheck.prove(instance);
}

/**
 * @brief Execute the ZeroMorph protocol to prove the multilinear evaluations produced by Sumcheck
 * @details See https://hackmd.io/dlf9xEwhTQyE3hiGbq4FsA?view for a complete description of the unrolled protocol.
 *
 * */
template <IsUltraFlavor Flavor> void UltraProver_<Flavor>::execute_zeromorph_rounds()
{
    ZeroMorph::prove(instance->prover_polynomials.get_unshifted(),
                     instance->prover_polynomials.get_to_be_shifted(),
                     sumcheck_output.claimed_evaluations.get_unshifted(),
                     sumcheck_output.claimed_evaluations.get_shifted(),
                     sumcheck_output.challenge,
                     commitment_key,
                     transcript);
}

template <IsUltraFlavor Flavor> HonkProof& UltraProver_<Flavor>::export_proof()
{
    proof = transcript->proof_data;
    return proof;
}

template <IsUltraFlavor Flavor> HonkProof& UltraProver_<Flavor>::construct_proof()
{
    // Add circuit size public input size and public inputs to transcript->
    oink_prover.execute_preamble_round();

    // Compute first three wire commitments
    oink_prover.execute_wire_commitments_round();

    // Compute sorted list accumulator and commitment
    oink_prover.execute_sorted_list_accumulator_round();

    // Fiat-Shamir: beta & gamma
    oink_prover.execute_log_derivative_inverse_round();

    // Compute grand product(s) and commitments.
    oink_prover.execute_grand_product_computation_round();

    // Fiat-Shamir: alpha
    // Run sumcheck subprotocol.
    execute_relation_check_rounds();

    // Fiat-Shamir: rho, y, x, z
    // Execute Zeromorph multilinear PCS
    execute_zeromorph_rounds();

    return export_proof();
}

template class UltraProver_<UltraFlavor>;
template class UltraProver_<GoblinUltraFlavor>;

} // namespace bb
