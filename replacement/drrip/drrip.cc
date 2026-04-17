#include "drrip.h"

#include <algorithm>
#include <cassert>
#include <random>
#include <utility>

#include "champsim.h"

#define HHT_SET 2048
#define HHT_WAY 16

struct HHTEntry {
    bool valid;
    uint32_t tag;
    uint8_t hit_queue[4];
    uint8_t ptr;
};

HHTEntry HHT[HHT_SET][HHT_WAY];
uint8_t HHT_rr_ptr[HHT_SET];

void update_hht(uint64_t addr, uint8_t hit_count);

// Helper functions for HHT
inline uint32_t get_hht_tag(uint64_t addr) {
    return addr >> 44; // top 20 bits
}

inline uint32_t get_hht_index(uint64_t addr) {
    return (addr >> 6) % HHT_SET;
}

// Expected hit prediction based on history
uint8_t get_expected_hits(uint64_t addr) {
    uint32_t tag = get_hht_tag(addr);
    uint32_t idx = get_hht_index(addr);

    for (int i = 0; i < HHT_WAY; i++) {
        if (HHT[idx][i].valid && HHT[idx][i].tag == tag) {
            int sum = 0;
            for (int j = 0; j < 4; j++)
                sum += HHT[idx][i].hit_queue[j];

            return sum / 4;
        }
    }

    return 1; // default value
}

// Update HHT on eviction with hit count
void update_hht(uint64_t addr, uint8_t hit_count) {
    uint32_t tag = get_hht_tag(addr);
    uint32_t idx = get_hht_index(addr);

    // Look for existing entry
    for (int i = 0; i < HHT_WAY; i++) {
        if (HHT[idx][i].valid && HHT[idx][i].tag == tag) {
            HHT[idx][i].hit_queue[HHT[idx][i].ptr] = hit_count;
            HHT[idx][i].ptr = (HHT[idx][i].ptr + 1) % 4;
            return;
        }
    }

    // Insert new entry: use an invalid slot first, then round-robin replacement.
    int victim = -1;
    for (int i = 0; i < HHT_WAY; i++) {
      if (!HHT[idx][i].valid) {
        victim = i;
        break;
      }
    }

    if (victim == -1) {
      victim = HHT_rr_ptr[idx];
      HHT_rr_ptr[idx] = (HHT_rr_ptr[idx] + 1) % HHT_WAY;
    }

    HHT[idx][victim].valid = true;
    HHT[idx][victim].tag = tag;
    HHT[idx][victim].ptr = 0;

    for (int j = 0; j < 4; j++)
        HHT[idx][victim].hit_queue[j] = 0;

    HHT[idx][victim].hit_queue[0] = hit_count;
}

drrip::drrip(CACHE* cache)
    : replacement(cache), NUM_SET(cache->NUM_SET), NUM_WAY(cache->NUM_WAY), load_miss_pressure(static_cast<std::size_t>(NUM_SET), 0),
      rrpv(static_cast<std::size_t>(NUM_SET * NUM_WAY))
{
  // randomly selected sampler sets
  std::size_t TOTAL_SDM_SETS = NUM_CPUS * NUM_POLICY * SDM_SIZE;
  std::generate_n(std::back_inserter(rand_sets), TOTAL_SDM_SETS, std::knuth_b{1});
  std::sort(std::begin(rand_sets), std::end(rand_sets));
  std::fill_n(std::back_inserter(PSEL), NUM_CPUS, typename decltype(PSEL)::value_type{0});

  // Initialize HHT
  for (int i = 0; i < HHT_SET; i++) {
    for (int j = 0; j < HHT_WAY; j++) {
      HHT[i][j].valid = false;
      HHT[i][j].ptr = 0;
      for (int k = 0; k < 4; k++)
        HHT[i][j].hit_queue[k] = 0;
    }
    HHT_rr_ptr[i] = 0;
  }
}

unsigned& drrip::get_rrpv(long set, long way) { return rrpv.at(static_cast<std::size_t>(set * NUM_WAY + way)); }

void drrip::update_bip(long set, long way)
{
  get_rrpv(set, way) = maxRRPV;

  bip_counter++;
  if (bip_counter == BIP_MAX) {
    bip_counter = 0;
    get_rrpv(set, way) = maxRRPV - 1;
  }
}

void drrip::update_srrip(long set, long way) { get_rrpv(set, way) = maxRRPV - 1; }

// called on every cache hit and cache fill
void drrip::update_replacement_state(uint32_t triggering_cpu, long set, long way, champsim::address full_addr, champsim::address ip,
                                     champsim::address victim_addr, access_type type, uint8_t hit)
{
  // do not update replacement state for writebacks
  if (access_type{type} == access_type::WRITE) {
    get_rrpv(set, way) = maxRRPV - 1;
    return;
  }

  auto& block = intern_->block[set * intern_->NUM_WAY + way];
  auto& load_pressure = load_miss_pressure.at(static_cast<std::size_t>(set));

  // cache hit - increment hit counter and decrement expected further hits
  if (hit) {
    get_rrpv(set, way) = 0; // for cache hit, DRRIP always promotes a cache line to the MRU position

    // EHC: Update hit counter (saturating at 7)
    if (block.current_hit_counter < 7)
      block.current_hit_counter++;

    // EHC: Decrement expected further hits
    if (block.expected_further_hits > 0)
      block.expected_further_hits--;

    if (access_type{type} == access_type::LOAD && load_pressure > 0)
      load_pressure--;

    return;
  }

  if (access_type{type} == access_type::LOAD)
    load_pressure = static_cast<uint8_t>(std::min<unsigned>(31, static_cast<unsigned>(load_pressure) + 2));
  else if (load_pressure > 0)
    load_pressure--;

  // cache miss - set expected hits from HHT and reset counter
  block.expected_further_hits = get_expected_hits(full_addr.to<uint64_t>());
  block.current_hit_counter = 0;

  // cache miss - DRRIP insertion policy
  auto begin = std::next(std::begin(rand_sets), triggering_cpu * NUM_POLICY * SDM_SIZE);
  auto end = std::next(begin, NUM_POLICY * SDM_SIZE);
  auto leader = std::find(begin, end, set);

  if (leader == end) { // follower sets
    auto selector = PSEL[triggering_cpu];
    if (selector.value() > (selector.maximum / 2)) { // follow BIP
      update_bip(set, way);
    } else { // follow SRRIP
      update_srrip(set, way);
    }
  } else if (leader == begin) { // leader 0: BIP
    PSEL[triggering_cpu]--;
    update_bip(set, way);
  } else if (leader == std::next(begin)) { // leader 1: SRRIP
    PSEL[triggering_cpu]++;
    update_srrip(set, way);
  }
}

double get_cost(const champsim::cache_block& blk)
{
    // Cost reflects write-back penalty only
    // Clean blocks have no write-back cost regardless of memory type
    if (!blk.dirty)
        return 0.0;  // Clean block - no writeback needed

    if (blk.memory_type == 0) // DRAM dirty
        return 1.0;
    else // NVM dirty - moderate write penalty
      return 6.0;
}


// find replacement victim
long drrip::find_victim(uint32_t triggering_cpu, uint64_t instr_id, long set, const champsim::cache_block* current_set, champsim::address ip,
                        champsim::address full_addr, access_type type)
{
  long victim = 0;
  double best_score = 1e18;

  // Tunable weights: moderated to recover LLC hit rate while still limiting NVM writes.
  constexpr double alpha = 1.0;  // RRPV weight
  constexpr double base_beta = 1.2;
  constexpr double guarded_beta = 0.45;
  constexpr uint8_t load_pressure_threshold = 12;

  const auto pressure = load_miss_pressure.at(static_cast<std::size_t>(set));
  const double beta = (access_type{type} == access_type::LOAD && pressure >= load_pressure_threshold) ? guarded_beta : base_beta;

  for (int way = 0; way < NUM_WAY; way++) {
      auto& blk = intern_->block[set * intern_->NUM_WAY + way];
      double cost = get_cost(blk);

      double score = blk.expected_further_hits
                    - alpha * get_rrpv(set, way)
                    + beta * cost;

      if (score < best_score) {
          best_score = score;
          victim = way;
      }
  }

  // Update HHT only for the actual victim (not all blocks!)
  auto& victim_blk = intern_->block[set * intern_->NUM_WAY + victim];
  if (victim_blk.valid) {
      update_hht(victim_blk.address.to<uint64_t>(), victim_blk.current_hit_counter);
  }

  return victim;
}
