#include <map>
#include <algorithm> // find, random_shuffle
#include <numeric> // iota
#include <random>

#include "netkit/neat/neat.h"
#include "netkit/network/activation_functions.h"
#include "netkit/neat/genome.h"
#include "netkit/neat/innovation.h"

const netkit::neuron_id_t netkit::genome::BIAS_ID = 0;

netkit::genome::genome(neat* neat_instance)
	: m_number_of_inputs(neat_instance->params.number_of_inputs)
	, m_number_of_outputs(neat_instance->params.number_of_outputs)
	, m_genes()
	, m_known_neuron_ids()
	, m_neat(neat_instance)
	, m_fitness(0) {
	m_known_neuron_ids.push_back(BIAS_ID);

	for (neuron_id_t i = 0; i < m_number_of_inputs; i++) {
		m_known_neuron_ids.push_back(i + 1);
	}

	for (neuron_id_t i = 0; i < m_number_of_outputs; i++) {
		m_known_neuron_ids.push_back(i + 1 + m_number_of_inputs);
	}
}

netkit::genome::genome(genome&& other) noexcept
  : m_number_of_inputs(other.m_number_of_inputs)
  , m_number_of_outputs(other.m_number_of_outputs)
  , m_genes(std::move(other.m_genes))
  , m_known_neuron_ids(std::move(other.m_known_neuron_ids))
  , m_neat(other.m_neat)
  , m_fitness(0) {
}

void netkit::genome::add_gene(gene new_gene) {
	// if this genes refers to an unknown neuron, add it to the known neurons list.
	if (std::find(m_known_neuron_ids.begin(), m_known_neuron_ids.end(), new_gene.from) == m_known_neuron_ids.end()) {
		m_known_neuron_ids.push_back(new_gene.from);
	}

	if (std::find(m_known_neuron_ids.begin(), m_known_neuron_ids.end(), new_gene.to) == m_known_neuron_ids.end()) {
		m_known_neuron_ids.push_back(new_gene.to);
	}

	m_genes.emplace_back(new_gene);
}

bool netkit::genome::link_exists(neuron_id_t from, neuron_id_t to) const {
	for (const gene& g : m_genes) {
		if (g.from == from && g.to == to) {
			return true;
		}
	}
	return false;
}

double netkit::genome::distance_to(const genome& other) const {
	unsigned int larger_size = static_cast<unsigned int>(std::max(this->m_genes.size(), other.m_genes.size()));
	if (larger_size <= 4) {
		return 0.;
	}

	unsigned int nb_disjoint_genes = 0;
	unsigned int nb_excess_genes = 0;
	unsigned int nb_matching_genes = 0;
	neuron_value_t sum_weight_difference = 0;

	auto oya1_gene_it = this->m_genes.cbegin();
	auto oya2_gene_it = other.m_genes.cbegin();

	while (oya1_gene_it != this->m_genes.cend() && oya2_gene_it != other.m_genes.cend()) {
		if (oya1_gene_it->innov_num == oya2_gene_it->innov_num) {
			++nb_matching_genes;
			sum_weight_difference += std::abs(oya1_gene_it->weight - oya2_gene_it->weight);
			++oya1_gene_it;
			++oya2_gene_it;
		} else if (oya1_gene_it->innov_num < oya2_gene_it->innov_num) {
			++nb_disjoint_genes;
			++oya1_gene_it;
		} else {
			++nb_disjoint_genes;
			++oya2_gene_it;
		}
	}

	neuron_value_t average_weight_difference = sum_weight_difference / nb_matching_genes;

	while (oya1_gene_it != this->m_genes.cend()) {
		++nb_excess_genes;
		++oya1_gene_it;
	}

	while (oya2_gene_it != other.m_genes.cend()) {
		++nb_excess_genes;
		++oya2_gene_it;
	}

	return m_neat->params.distance_coef_c1 * nb_excess_genes / larger_size
		+ m_neat->params.distance_coef_c2 * nb_disjoint_genes / larger_size
		+ m_neat->params.distance_coef_c3 * average_weight_difference;
}

bool netkit::genome::is_compatible_with(const genome & other) const {
	return distance_to(other) < m_neat->params.compatibility_threshold;
}

netkit::genome netkit::genome::get_random_mutation() const {
	genome offspring(*this);

	unsigned int remaining_tries = 3; // a mutation can fail, so let's give it three tries.
	while (!offspring.random_mutate() && remaining_tries--) {}

	return std::move(offspring);
}

bool netkit::genome::random_mutate() {
	std::uniform_int_distribution<unsigned int> mutation_selector(0, m_neat->params.sum_all_mutation_weights() - 1);
	unsigned int rnd_val = mutation_selector(m_neat->rand_engine);

	if (rnd_val < m_neat->params.mutation_add_link_weight) {
		return mutate_add_link();
	}
	rnd_val -= m_neat->params.mutation_add_link_weight;

	if (rnd_val < m_neat->params.mutation_add_neuron_weight) {
		return mutate_add_neuron();
	}
	rnd_val -= m_neat->params.mutation_add_neuron_weight;

	if (rnd_val < m_neat->params.mutation_all_weights_weight) {
		return mutate_all_weights();
	}
	rnd_val -= m_neat->params.mutation_all_weights_weight;

	if (rnd_val < m_neat->params.mutation_one_weight_weight) {
		return mutate_one_weight();
	}
	rnd_val -= m_neat->params.mutation_one_weight_weight;

	if (rnd_val < m_neat->params.mutation_reset_weights_weight) {
		return mutate_reset_weights();
	}
	rnd_val -= m_neat->params.mutation_reset_weights_weight;

	if (rnd_val < m_neat->params.mutation_remove_gene_weight) {
		return mutate_remove_gene();
	}
	rnd_val -= m_neat->params.mutation_remove_gene_weight;

	if (rnd_val < m_neat->params.mutation_reenable_gene_weight) {
		return mutate_reenable_gene();
	}

	// the last option is...
	return mutate_toggle_enable();
}

bool netkit::genome::mutate_add_link() {
	std::uniform_int_distribution<neuron_id_t> from_selector(0, static_cast<neuron_id_t>(m_known_neuron_ids.size() - 1));
	auto from = from_selector(m_neat->rand_engine);

	// select a destination that is not an input nor the bias...
	std::uniform_int_distribution<neuron_id_t> destination_selector(0, static_cast<neuron_id_t>(m_known_neuron_ids.size() - m_number_of_inputs  - 2));
	auto to = destination_selector(m_neat->rand_engine) + m_number_of_inputs + 1;

	if (link_exists(m_known_neuron_ids[from], m_known_neuron_ids[to])) {
		// already exist in this genome...
		return false;
	}

	std::uniform_real_distribution<netkit::neuron_value_t> perturbator(-m_neat->params.initial_weight_perturbation,
	                                                                   m_neat->params.initial_weight_perturbation);
	std::optional<gene> existing_gene =
		m_neat->innov_pool.find_gene(m_known_neuron_ids[from], m_known_neuron_ids[to]);
	if (existing_gene.has_value()) {
		gene copied_gene(*existing_gene);
		copied_gene.weight = perturbator(m_neat->rand_engine);
		add_gene(copied_gene);
	} else {
		gene new_gene(m_neat->innov_pool.next_innovation(), m_known_neuron_ids[from], m_known_neuron_ids[to], perturbator(m_neat->rand_engine));
		m_neat->innov_pool.register_gene(new_gene);
		m_neat->innov_pool.register_innovation(innovation::new_link_innovation(
			new_gene.innov_num,
			new_gene.from,
			new_gene.to
		));
		add_gene(new_gene);
	}

	return true;
}

bool netkit::genome::mutate_add_neuron() {
	// iterate in genes in a random order...
	std::vector<int> candidates_idx(m_genes.size());
	std::iota(candidates_idx.begin(), candidates_idx.end(), 0);

	static std::random_device rd;
	static std::mt19937 g(rd());
	std::shuffle(candidates_idx.begin(), candidates_idx.end(), g);

	int sel_idx = -1; // selected gene index
	for (int idx : candidates_idx) {
		if (m_genes[idx].enabled) {
			sel_idx = idx;
			break;
		}
	}

	if (sel_idx == -1) {
		return false; // no enabled gene available
	}

	m_genes[sel_idx].enabled = false;

	std::optional<innovation> existing_innovation =
		m_neat->innov_pool.find_innovation(NEW_NEURON, m_genes[sel_idx].from, m_genes[sel_idx].to);

	if (existing_innovation.has_value()) {
		add_gene({existing_innovation->innov_num, existing_innovation->from,
				 existing_innovation->new_neuron_id, m_genes[sel_idx].weight});
		add_gene({existing_innovation->innov_num_2, existing_innovation->new_neuron_id,
				 existing_innovation->to, m_genes[sel_idx].weight});
	} else {
		neuron_id_t new_neuron_id = m_neat->innov_pool.next_hidden_neuron_id();

		gene new_gene_1(m_neat->innov_pool.next_innovation(),
						m_genes[sel_idx].from, new_neuron_id, m_genes[sel_idx].weight);
		gene new_gene_2(m_neat->innov_pool.next_innovation(), new_neuron_id,
						m_genes[sel_idx].to, m_genes[sel_idx].weight);

		m_neat->innov_pool.register_gene(new_gene_1);
		m_neat->innov_pool.register_gene(new_gene_2);
		m_neat->innov_pool.register_innovation(innovation::new_neuron_innovation(
			new_gene_1.innov_num,
			new_gene_2.innov_num,
			m_genes[sel_idx].from,
			m_genes[sel_idx].to,
			new_neuron_id
		));

		add_gene(new_gene_1);
		add_gene(new_gene_2);
	}

	return true;
}

bool netkit::genome::mutate_reenable_gene() {
	std::vector<gene*> candidates;
	for (gene& g : m_genes) {
		if (!g.enabled) {
			candidates.push_back(&g);
		}
	}

	if (candidates.empty()) {
		return false;
	} else {
		std::uniform_int_distribution<size_t> candidate_selector(0, candidates.size() - 1);
		candidates[candidate_selector(m_neat->rand_engine)]->enabled = true;
		return true;
	}
}

bool netkit::genome::mutate_toggle_enable() {
	if (m_genes.empty()) {
		return false;
	}

	std::uniform_int_distribution<size_t> gene_selector(0, m_genes.size() - 1);
	size_t rnd_val = gene_selector(m_neat->rand_engine);
	m_genes[rnd_val].enabled = !m_genes[rnd_val].enabled;

	return true;
}

bool netkit::genome::mutate_one_weight() {
	if (m_genes.empty()) {
		return false;
	}

	std::uniform_real_distribution<netkit::neuron_value_t> perturbator(-m_neat->params.weight_mutation_power,
	                                                                   m_neat->params.weight_mutation_power);
	std::uniform_int_distribution<size_t> gene_selector(0, m_genes.size() - 1);
	size_t rnd_val = gene_selector(m_neat->rand_engine);
	m_genes[rnd_val].weight += perturbator(m_neat->rand_engine);

	return true;
}

bool netkit::genome::mutate_all_weights() {
	std::uniform_real_distribution<netkit::neuron_value_t> perturbator(-m_neat->params.weight_mutation_power,
	                                                                   m_neat->params.weight_mutation_power);
	for (gene& g : m_genes) {
		g.weight += perturbator(m_neat->rand_engine);
	}

	return true;
}

bool netkit::genome::mutate_reset_weights() {
	std::uniform_real_distribution<netkit::neuron_value_t> perturbator(-m_neat->params.initial_weight_perturbation,
	                                                                   m_neat->params.initial_weight_perturbation);
	for (gene& g : m_genes) {
		g.weight = perturbator(m_neat->rand_engine);
	}

	return true;
}

bool netkit::genome::mutate_remove_gene(){
	if (m_genes.empty()) {
		return false;
	}

	std::uniform_int_distribution<size_t> gene_selector(0, m_genes.size() - 1);
	m_genes.erase(m_genes.begin() + gene_selector(m_neat->rand_engine));
	// TODO: check if a neuron goes unknown afterward.

	return true;
}

netkit::genome netkit::genome::random_crossover(const genome& other) const {
	std::uniform_int_distribution<unsigned int> crossover_selector(0, m_neat->params.sum_all_crossover_weights() - 1);
	unsigned int rnd_val = crossover_selector(m_neat->rand_engine);

	if (rnd_val < m_neat->params.crossover_multipoint_avg_weight) {
		return crossover_multipoint_avg(other);
	}
	rnd_val -= m_neat->params.crossover_multipoint_avg_weight;

	if (rnd_val < m_neat->params.crossover_multipoint_best_weight) {
		return crossover_multipoint_best(other);
	}

	// the last option is...
	return crossover_multipoint_rnd(other);
}

netkit::genome netkit::genome::crossover_multipoint_best(const genome& other) const {
	return helper_crossover_multipoint(other, [] (const genome& p1, const gene& g1, const genome& p2, const gene& g2) -> gene {
		if (p1.m_fitness > p2.m_fitness) {
			return g1;
		} else {
			return g2;
		}
	});
}

netkit::genome netkit::genome::crossover_multipoint_rnd(const genome& other) const {
	return helper_crossover_multipoint(other, [this] (const genome& /*p1*/, const gene& g1, const genome& /*p2*/, const gene& g2) -> gene {
		std::bernoulli_distribution true_false_selector(0.5);
		if (true_false_selector(this->m_neat->rand_engine)) {
			return g1;
		} else {
			return g2;
		}
	});
}

netkit::genome netkit::genome::crossover_multipoint_avg(const genome& other) const {
	return helper_crossover_multipoint(other, [&] (const genome& /*p1*/, const gene& g1, const genome& /*p2*/, const gene& g2) -> gene {
		gene new_gene(g1);
		new_gene.weight = (g1.weight + g2.weight) / 2;
		return new_gene;
	});
}

netkit::network netkit::genome::generate_network() const {
	network net;

	// we need to map the genome neuron ids to
	// the network neuron ids.
	std::map<neuron_id_t, neuron_id_t> ids_map;

	ids_map.emplace(BIAS_ID, network::BIAS_ID);

	for (size_t i = 0; i < m_number_of_inputs; i++) {
		neuron_id_t net_neuron_id = net.add_neuron(INPUT, neuron(&steepened_sigmoid));
		ids_map.emplace(i + 1, net_neuron_id);
	}

	for (size_t i = 0; i < m_number_of_outputs; i++) {
		neuron_id_t net_neuron_id = net.add_neuron(OUTPUT, neuron(&steepened_sigmoid));
		ids_map.emplace(i + m_number_of_inputs + 1, net_neuron_id);
	}

	for (size_t i = m_number_of_inputs + m_number_of_outputs + 1; i < m_known_neuron_ids.size(); i++) {
		neuron_id_t net_neuron_id = net.add_neuron(HIDDEN, neuron(&steepened_sigmoid));
		ids_map.emplace(m_known_neuron_ids[i], net_neuron_id);
	}

	for (const gene& g : m_genes) {
		if (g.enabled) {
			neuron_id_t from_id = ids_map.find(g.from)->second;
			neuron_id_t to_id = ids_map.find(g.to)->second;
			net.add_link(from_id, to_id, g.weight);
		}
	}

	return std::move(net);
}

bool netkit::genome::reenable_gene_ok() const {
	std::bernoulli_distribution distrib(0.25); // TODO: externalize in parameters
	return distrib(m_neat->rand_engine);
}

std::ostream & netkit::operator<<(std::ostream & os, const genome & genome) {
	os << "<genome: (fitness = " << genome.m_fitness << ") "
		<< genome.m_number_of_inputs << " input(s) "
		<< genome.m_number_of_outputs << " output(s)" << std::endl;

	os << "\tgenes are:" << std::endl;
	for (const gene& g : genome.m_genes) {
		os << "\t" << g << std::endl;
	}
	os << "\ttotal: " << genome.m_genes.size() << " genes and " << genome.m_known_neuron_ids.size() << " neurons>";

	return os;
}
