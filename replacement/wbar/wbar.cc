#include "wbar.h"

#include <algorithm>
#include <cassert>

#include "champsim.h"

// Initial weights based on block type:
// NVM dirty = 0, DRAM dirty = 3, NVM clean = 5, DRAM clean = 7
constexpr uint8_t WEIGHT_NVM_DIRTY = 0;
constexpr uint8_t WEIGHT_DRAM_DIRTY = 3;
constexpr uint8_t WEIGHT_NVM_CLEAN = 5;
constexpr uint8_t WEIGHT_DRAM_CLEAN = 7;
constexpr uint8_t MAX_WEIGHT = 15;  // Maximum weight for 16-way cache

wbar::wbar(CACHE* cache)
    : replacement(cache), NUM_SET(cache->NUM_SET), NUM_WAY(cache->NUM_WAY),
      weight(static_cast<std::size_t>(NUM_SET * NUM_WAY), MAX_WEIGHT)
{
}

uint8_t& wbar::get_weight(long set, long way)
{
  return weight.at(static_cast<std::size_t>(set * NUM_WAY + way));
}

// Find victim: block with MAXIMUM weight
long wbar::find_victim(uint32_t triggering_cpu, uint64_t instr_id, long set, const champsim::cache_block* current_set, champsim::address ip,
                       champsim::address full_addr, access_type type)
{
  long victim = 0;
  uint8_t max_weight = 0;

  for (long way = 0; way < NUM_WAY; way++) {
    uint8_t w = get_weight(set, way);
    if (w > max_weight) {
      max_weight = w;
      victim = way;
    }
  }

  return victim;
}

// On fill: Set initial weight based on block type (memory_type and dirty status)
// and simulate queue insertion by incrementing weights of blocks >= new weight
void wbar::replacement_cache_fill(uint32_t triggering_cpu, long set, long way, champsim::address full_addr, champsim::address ip,
                                  champsim::address victim_addr, access_type type)
{
  auto& block = intern_->block[set * intern_->NUM_WAY + way];
  
  // Determine initial weight based on memory_type and dirty status
  uint8_t new_weight;
  if (block.memory_type == 1) {  // NVM
    new_weight = block.dirty ? WEIGHT_NVM_DIRTY : WEIGHT_NVM_CLEAN;
  } else {  // DRAM
    new_weight = block.dirty ? WEIGHT_DRAM_DIRTY : WEIGHT_DRAM_CLEAN;
  }

  // Simulate queue insertion: increment weights of blocks with weight >= new_weight
  for (long w = 0; w < NUM_WAY; w++) {
    if (w != way) {
      uint8_t& other_weight = get_weight(set, w);
      if (other_weight >= new_weight && other_weight < MAX_WEIGHT) {
        other_weight++;
      }
    }
  }

  get_weight(set, way) = new_weight;
}

// On hit: Decrement hit block's weight (min 0)
// Increment weight of blocks with weight < original hit weight (simulate left shift)
void wbar::update_replacement_state(uint32_t triggering_cpu, long set, long way, champsim::address full_addr, champsim::address ip,
                                    champsim::address victim_addr, access_type type, uint8_t hit)
{
  // Skip for writebacks
  if (access_type{type} == access_type::WRITE) {
    return;
  }

  if (hit) {
    uint8_t original_weight = get_weight(set, way);

    // Decrement hit block's weight (min 0)
    if (original_weight > 0) {
      get_weight(set, way) = original_weight - 1;
    }

    // Increment weights of blocks "to the left" (weight < original hit weight)
    for (long w = 0; w < NUM_WAY; w++) {
      if (w != way) {
        uint8_t& other_weight = get_weight(set, w);
        if (other_weight < original_weight && other_weight < MAX_WEIGHT) {
          other_weight++;
        }
      }
    }
  }
}
