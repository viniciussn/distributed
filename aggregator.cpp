#include "aggregator.h"

#include <vector>
#include <queue>
#include <mpi.h>

#include "faiss/IndexIVFPQ.h"

namespace {
	struct PartialResult {
		float* dists;
		long* ids;
		bool own_fields;
		float* base_dists;
		long* base_ids;
	};
}

static void merge_results(std::vector<PartialResult>& results, faiss::Index::idx_t* answers, int nshards, int k) {
	int counter[nshards];
	for (int i = 0; i < nshards; i++) counter[i] = 0;

	for (int topi = 0; topi < k; topi++) {
		float bestDist = std::numeric_limits<float>::max();
		long bestId = -1;
		int fromShard = -1;

		for (int shard = 0; shard < nshards; shard++) {
			if (counter[shard] == k) continue;

			if (results[shard].dists[counter[shard]] < bestDist) {
				bestDist = results[shard].dists[counter[shard]];
				bestId = results[shard].ids[counter[shard]];
				fromShard = shard;
			}
		}

		answers[topi] = bestId;
		counter[fromShard]++;
	}
}

static void aggregate_query(std::queue<PartialResult>* queue, int nshards, faiss::Index::idx_t* answers, int k) {
	std::vector<PartialResult> results(nshards);
	
	for (int shard = 0; shard < nshards; shard++) {
		results[shard] = queue[shard].front();
		queue[shard].pop();
	}
				
	merge_results(results, answers, nshards, k);
	
	for (int shard = 0; shard < nshards; shard++) {
		if (results[shard].own_fields) {
			delete [] results[shard].base_dists;
			delete [] results[shard].base_ids;
		}	
	}
}


static faiss::Index::idx_t* load_gt(Config& cfg) {
	long n_out;
	int db_k;
	int *gt_int = ivecs_read(cfg.gnd_path.c_str(), &db_k, &n_out);

	faiss::Index::idx_t* gt = new faiss::Index::idx_t[cfg.k * cfg.distinct_queries];

	for (int i = 0; i < cfg.distinct_queries; i++) {
		for (int j = 0; j < cfg.k; j++) {
			gt[i * cfg.k + j] = gt_int[i * db_k + j];
		}
	}

	delete[] gt_int;
	return gt;
}

static void send_times(std::deque<double>& end_times, int eval_length) {
	double end_times_array[eval_length];

	for (int i = 0; i < eval_length; i++) {
		end_times_array[i] = end_times.front();
		end_times.pop_front();
	}

	MPI_Send(end_times_array, eval_length, MPI_DOUBLE, GENERATOR, 0, MPI_COMM_WORLD);
}

//TODO: make this work for generic k's
static void show_recall(faiss::Index::idx_t* answers, Config& cfg) {
	auto gt = load_gt(cfg);

	int n_1 = 0, n_10 = 0, n_100 = 0;
	
	for (int i = 0; i < cfg.num_blocks * cfg.block_size; i++) {
		int answer_id = i % (cfg.num_blocks * cfg.block_size);
		int nq = i % cfg.distinct_queries;
		int gt_nn = gt[nq * cfg.k];
		
		for (int j = 0; j < cfg.k; j++) {
			if (answers[answer_id * cfg.k + j] == gt_nn) {
				if (j < 1) n_1++;
				if (j < 10) n_10++;
				if (j < 100) n_100++;
			}
		}
	}
	
	std::printf("R@1 = %.4f\n", n_1 / float(cfg.num_blocks * cfg.block_size));
	
	if (cfg.k >= 10) {
		std::printf("R@10 = %.4f\n", n_10 / float(cfg.num_blocks * cfg.block_size));
	}
	
	if (cfg.k >= 100) {
		std::printf("R@100 = %.4f\n", n_100 / float(cfg.num_blocks * cfg.block_size));
	}
	
	
	delete [] gt;
}

void aggregator(int nshards, Config& cfg) {
	auto target_delta = cfg.num_blocks * cfg.block_size / 10;
	auto target = target_delta;
	
	std::deque<double> end_times;

	faiss::Index::idx_t* answers = new faiss::Index::idx_t[cfg.num_blocks * cfg.block_size * cfg.k];
	
	std::queue<PartialResult> queue[nshards];
	std::queue<PartialResult> to_delete;

	long queries_remaining = cfg.num_blocks * cfg.block_size;
	long qn = 0;
	
	std::vector<long> remaining_queries_per_shard(nshards);
	for (int shard = 0; shard < nshards; shard++) {
		remaining_queries_per_shard[shard] = cfg.num_blocks * cfg.block_size;
	}

	while (queries_remaining >= 1) {
		MPI_Status status;
		
		auto I = new faiss::Index::idx_t[cfg.k * cfg.block_size];
		auto D = new float[cfg.k * cfg.block_size];

		MPI_Recv(I, cfg.k * cfg.block_size, MPI_LONG, MPI_ANY_SOURCE, 0, MPI_COMM_WORLD, &status);
		MPI_Recv(D, cfg.k * cfg.block_size, MPI_FLOAT, status.MPI_SOURCE, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

		int shard = status.MPI_SOURCE - 2;
		remaining_queries_per_shard[shard] -= cfg.block_size;
		
		for (int q = 0; q < cfg.block_size; q++) {
			queue[shard].push({ D + cfg.k * q, I + cfg.k * q, q == cfg.block_size - 1, D, I });
		}
		
		while (true) {
			bool hasEmpty = false;

			for (int i = 0; i < nshards; i++) {
				if (queue[i].empty()) {
					hasEmpty = true;
					break;
				}
			}
			
			if (hasEmpty) break;

			aggregate_query(queue, nshards, answers + (qn % (cfg.num_blocks * cfg.block_size)) * cfg.k, cfg.k);
			queries_remaining--;
			qn++;
			
			end_times.push_back(now());
		}
	}
	
	if (cfg.exec_type != ExecType::Bench) {
		if (cfg.show_recall) show_recall(answers, cfg); 
		send_times(end_times, cfg.num_blocks * cfg.block_size);
	}
}
