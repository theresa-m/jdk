/*
 * Copyright (c) 2014, 2021, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */
#include "precompiled.hpp"

#include "runtime/os.hpp"
#include "services/mallocSiteTable.hpp"
#include "services/mallocTracker.hpp"
#include "services/mallocTracker.inline.hpp"
#include "services/memTracker.hpp"
#include "utilities/debug.hpp"
#include "utilities/ostream.hpp"

size_t MallocMemorySummary::_snapshot[CALC_OBJ_SIZE_IN_TYPE(MallocMemorySnapshot, size_t)];

#ifdef ASSERT
void MemoryCounter::update_peak_count(size_t count) {
  size_t peak_cnt = peak_count();
  while (peak_cnt < count) {
    size_t old_cnt = Atomic::cmpxchg(&_peak_count, peak_cnt, count, memory_order_relaxed);
    if (old_cnt != peak_cnt) {
      peak_cnt = old_cnt;
    }
  }
}

void MemoryCounter::update_peak_size(size_t sz) {
  size_t peak_sz = peak_size();
  while (peak_sz < sz) {
    size_t old_sz = Atomic::cmpxchg(&_peak_size, peak_sz, sz, memory_order_relaxed);
    if (old_sz != peak_sz) {
      peak_sz = old_sz;
    }
  }
}

size_t MemoryCounter::peak_count() const {
  return Atomic::load(&_peak_count);
}

size_t MemoryCounter::peak_size() const {
  return Atomic::load(&_peak_size);
}
#endif

// Total malloc invocation count
size_t MallocMemorySnapshot::total_count() const {
  size_t amount = 0;
  for (int index = 0; index < mt_number_of_types; index ++) {
    amount += _malloc[index].malloc_count();
  }
  return amount;
}

// Total malloc'd memory amount
size_t MallocMemorySnapshot::total() const {
  size_t amount = 0;
  for (int index = 0; index < mt_number_of_types; index ++) {
    amount += _malloc[index].malloc_size();
  }
  amount += _tracking_header.size() + total_arena();
  return amount;
}

// Total malloc'd memory used by arenas
size_t MallocMemorySnapshot::total_arena() const {
  size_t amount = 0;
  for (int index = 0; index < mt_number_of_types; index ++) {
    amount += _malloc[index].arena_size();
  }
  return amount;
}

// Make adjustment by subtracting chunks used by arenas
// from total chunks to get total free chunk size
void MallocMemorySnapshot::make_adjustment() {
  size_t arena_size = total_arena();
  int chunk_idx = NMTUtil::flag_to_index(mtChunk);
  _malloc[chunk_idx].record_free(arena_size);
}


void MallocMemorySummary::initialize() {
  assert(sizeof(_snapshot) >= sizeof(MallocMemorySnapshot), "Sanity Check");
  // Uses placement new operator to initialize static area.
  ::new ((void*)_snapshot)MallocMemorySnapshot();
}

void MallocHeader::mark_block_as_dead() {
  _canary = _header_canary_dead_mark;
  NOT_LP64(_alt_canary = _header_alt_canary_dead_mark);
  set_footer_byte(_footer_canary_dead_mark);
}

void MallocHeader::release() {
  // Tracking already shutdown, no housekeeping is needed anymore
  if (MemTracker::tracking_level() <= NMT_minimal) return;

  check_block_integrity();

  MallocMemorySummary::record_free(size(), flags());
  MallocMemorySummary::record_free_malloc_header(sizeof(MallocHeader));
  if (MemTracker::tracking_level() == NMT_detail) {
    MallocSiteTable::deallocation_at(size(), _bucket_idx, _pos_idx);
  }

  mark_block_as_dead();
}

void MallocHeader::print_block_on_error(outputStream* st, address bad_address) const {
  st->print_cr("NMT Block at " PTR_FORMAT ", corruption at: " PTR_FORMAT ": ",
               p2i(this), p2i(bad_address));
  address from = align_down((address)this, sizeof(void*)) - 8;
  address to = from + 64;
  // Note: print_hex_dump uses SafeFetch, so it should be able to handle unmapped memory.
  os::print_hex_dump(st, from, to, 1);
  assert(bad_address >= from, "sanity");
  // if the corruption is in the block body of in the footer, print out that part too
  // unless it has been part of the first hexdump
  address from2 = align_down(bad_address, sizeof(void*)) - 8;
  from2 = MAX2(to, from2);
  address to2 = from2 + 96;
  if (to2 > to) {
    if (from2 > to) {
      st->print_cr("...");
    }
    os::print_hex_dump(st, from2, to2, 1);
  }
}

// Check block integrity. If block is broken, print out a report
// to tty (optionally with hex dump surrounding the broken block),
// then trigger a fatal error.
void MallocHeader::check_block_integrity() const {

  // Note: if you modify the error messages here, make sure you
  // adapt the associated gtests too.

  // Weed out obviously wrong block addresses of NULL or very low
  // values. Note that we should not call this for ::free(NULL),
  // which should be handled by os::free() above us.
  if (((size_t)p2i(this)) < K) {
    fatal("Block at " PTR_FORMAT ": invalid block address", p2i(this));
  }

  // From here on we assume the block pointer to be valid. We could
  //  use SafeFetch but since this is a hot path we don't. If we are
  //  wrong, we will crash when accessing the canary, which hopefully
  //  generates distinct crash report.

  // Also weed out unaligned addresses. Note that the alignment requirements
  // we check here are the bare minimum of what we know will malloc() give us
  // (which is 64-bit even on 32-bit platforms).
  if (!is_aligned(this, sizeof(uint64_t))) {
    print_block_on_error(tty, (address)this);
    fatal("Block at " PTR_FORMAT ": block address is unaligned", p2i(this));
  }

  // Check header canary
  if (_canary != _header_canary_life_mark) {
    print_block_on_error(tty, (address)this);
    fatal("Block at " PTR_FORMAT ": header canary broken.", p2i(this));
  }

#ifndef _LP64
  // On 32-bit we have a second canary, check that one too.
  if (_alt_canary != _header_alt_canary_life_mark) {
    print_block_on_error(tty, (address)this);
    fatal("Block at " PTR_FORMAT ": header alternate canary broken.", p2i(this));
  }
#endif

  // Does block size seems reasonable?
  if (_size >= max_reasonable_malloc_size) {
    print_block_on_error(tty, (address)this);
    fatal("Block at " PTR_FORMAT ": header looks invalid (weirdly large block size)", p2i(this));
  }

  // Check footer canary
  if (get_footer_byte() != _footer_canary_life_mark) {
    print_block_on_error(tty, footer_address());
    fatal("Block at " PTR_FORMAT ": footer canary broken at " PTR_FORMAT " (buffer overflow?)",
          p2i(this), p2i(footer_address()));
  }
}

bool MallocHeader::record_malloc_site(const NativeCallStack& stack, size_t size,
  size_t* bucket_idx, size_t* pos_idx, MEMFLAGS flags) const {
  bool ret = MallocSiteTable::allocation_at(stack, size, bucket_idx, pos_idx, flags);

  // Something went wrong, could be OOM or overflow malloc site table.
  // We want to keep tracking data under OOM circumstance, so transition to
  // summary tracking.
  if (!ret) {
    MemTracker::transition_to(NMT_summary);
  }
  return ret;
}

bool MallocHeader::get_stack(NativeCallStack& stack) const {
  return MallocSiteTable::access_stack(stack, _bucket_idx, _pos_idx);
}

bool MallocTracker::initialize(NMT_TrackingLevel level) {
  if (level >= NMT_summary) {
    MallocMemorySummary::initialize();
  }

  if (level == NMT_detail) {
    return MallocSiteTable::initialize();
  }
  return true;
}

bool MallocTracker::transition(NMT_TrackingLevel from, NMT_TrackingLevel to) {
  assert(from != NMT_off, "Can not transition from off state");
  assert(to != NMT_off, "Can not transition to off state");
  assert (from != NMT_minimal, "cannot transition from minimal state");

  if (from == NMT_detail) {
    assert(to == NMT_minimal || to == NMT_summary, "Just check");
    MallocSiteTable::shutdown();
  }
  return true;
}

// Record a malloc memory allocation
void* MallocTracker::record_malloc(void* malloc_base, size_t size, MEMFLAGS flags,
  const NativeCallStack& stack, NMT_TrackingLevel level) {
  assert(level != NMT_off, "precondition");
  void*         memblock;      // the address for user data
  MallocHeader* header = NULL;

  if (malloc_base == NULL) {
    return NULL;
  }

  // Uses placement global new operator to initialize malloc header

  header = ::new (malloc_base)MallocHeader(size, flags, stack, level);
  memblock = (void*)((char*)malloc_base + sizeof(MallocHeader));

  // The alignment check: 8 bytes alignment for 32 bit systems.
  //                      16 bytes alignment for 64-bit systems.
  assert(((size_t)memblock & (sizeof(size_t) * 2 - 1)) == 0, "Alignment check");

#ifdef ASSERT
  if (level > NMT_minimal) {
    // Read back
    assert(get_size(memblock) == size,   "Wrong size");
    assert(get_flags(memblock) == flags, "Wrong flags");
  }
#endif

  return memblock;
}

void* MallocTracker::record_free(void* memblock) {
  assert(MemTracker::tracking_level() != NMT_off && memblock != NULL, "precondition");
  MallocHeader* header = malloc_header(memblock);
  header->release();
  return (void*)header;
}
