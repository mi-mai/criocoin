// Copyright (c) 2018-2019, The NERVA Project
// Copyright (c) 2017-2018, The Masari Project
// Copyright (c) 2014-2019, The Monero Project
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "db_lmdb.h"

#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/circular_buffer.hpp>
#include <memory>  // std::unique_ptr
#include <cstring>  // memcpy

#include "string_tools.h"
#include "file_io_utils.h"
#include "common/util.h"
#include "cryptonote_basic/cryptonote_format_utils.h"
#include "cryptonote_basic/random_numbers.h"
#include "crypto/crypto.h"
#include "profile_tools.h"
#include "ringct/rctOps.h"

#undef MONERO_DEFAULT_LOG_CATEGORY
#define MONERO_DEFAULT_LOG_CATEGORY "blockchain.db.lmdb"


#if defined(__i386) || defined(__x86_64)
#define MISALIGNED_OK	1
#endif

using epee::string_tools::pod_to_hex;
using namespace crypto;

// Increase when the DB changes in a non backward compatible way, and there
// is no automatic conversion, so that a full resync is needed.
#define VERSION 3

namespace
{

#pragma pack(push, 1)
// This MUST be identical to output_data_t, without the extra rct data at the end
struct pre_rct_output_data_t
{
  crypto::public_key pubkey;       //!< the output's public key (for spend verification)
  uint64_t           unlock_time;  //!< the output's unlock time (or height)
  uint64_t           height;       //!< the height of the block which created the output
};
#pragma pack(pop)

template <typename T>
inline void throw0(const T &e)
{
  LOG_PRINT_L0(e.what());
  throw e;
}

template <typename T>
inline void throw1(const T &e)
{
  LOG_PRINT_L1(e.what());
  throw e;
}

#define MDB_val_set(var, val)   MDB_val var = {sizeof(val), (void *)&val}

#define MDB_val_sized(var, val) MDB_val var = {val.size(), (void *)val.data()}

#define MDB_val_str(var, val) MDB_val var = {strlen(val) + 1, (void *)val}

template<typename T>
struct MDB_val_copy: public MDB_val
{
  MDB_val_copy(const T &t) :
    t_copy(t)
  {
    mv_size = sizeof (T);
    mv_data = &t_copy;
  }
private:
  T t_copy;
};

template<>
struct MDB_val_copy<cryptonote::blobdata>: public MDB_val
{
  MDB_val_copy(const cryptonote::blobdata &bd) :
    data(new char[bd.size()])
  {
    memcpy(data.get(), bd.data(), bd.size());
    mv_size = bd.size();
    mv_data = data.get();
  }
private:
  std::unique_ptr<char[]> data;
};

template<>
struct MDB_val_copy<const char*>: public MDB_val
{
  MDB_val_copy(const char *s):
    size(strlen(s)+1), // include the NUL, makes it easier for compares
    data(new char[size])
  {
    mv_size = size;
    mv_data = data.get();
    memcpy(mv_data, s, size);
  }
private:
  size_t size;
  std::unique_ptr<char[]> data;
};

}

namespace cryptonote
{

int BlockchainLMDB::compare_uint64(const MDB_val *a, const MDB_val *b)
{
  uint64_t va, vb;
  memcpy(&va, a->mv_data, sizeof(va));
  memcpy(&vb, b->mv_data, sizeof(vb));
  return (va < vb) ? -1 : va > vb;
}

int BlockchainLMDB::compare_hash32(const MDB_val *a, const MDB_val *b)
{
  uint32_t *va = (uint32_t*) a->mv_data;
  uint32_t *vb = (uint32_t*) b->mv_data;
  for (int n = 7; n >= 0; n--)
  {
    if (va[n] == vb[n])
      continue;
    return va[n] < vb[n] ? -1 : 1;
  }

  return 0;
}

int BlockchainLMDB::compare_string(const MDB_val *a, const MDB_val *b)
{
  const char *va = (const char*) a->mv_data;
  const char *vb = (const char*) b->mv_data;
  return strcmp(va, vb);
}

}

namespace
{

/* DB schema:
 *
 * Table            Key          Data
 * -----            ---          ----
 * blocks           block ID     block blob
 * block_heights    block hash   block height
 * block_info       block ID     {block metadata}
 *
 * txs              txn ID       txn blob
 * tx_indices       txn hash     {txn ID, metadata}
 * tx_outputs       txn ID       [txn amount output indices]
 *
 * output_txs       output ID    {txn hash, local index}
 * output_amounts   amount       [{amount output index, metadata}...]
 *
 * spent_keys       input hash   -
 *
 * txpool_meta      txn hash     txn metadata
 * txpool_blob      txn hash     txn blob
 *
 * alt_blocks       block hash   {block data, block blob}
 *
 * Note: where the data items are of uniform size, DUPFIXED tables have
 * been used to save space. In most of these cases, a dummy "zerokval"
 * key is used when accessing the table; the Key listed above will be
 * attached as a prefix on the Data to serve as the DUPSORT key.
 * (DUPFIXED saves 8 bytes per record.)
 *
 * The output_amounts table doesn't use a dummy key, but uses DUPSORT.
 */
const char* const LMDB_BLOCKS = "blocks";
const char* const LMDB_BLOCK_HEIGHTS = "block_heights";
const char* const LMDB_BLOCK_INFO = "block_info";

const char* const LMDB_TXS = "txs";
const char* const LMDB_TX_INDICES = "tx_indices";
const char* const LMDB_TX_OUTPUTS = "tx_outputs";

const char* const LMDB_OUTPUT_TXS = "output_txs";
const char* const LMDB_OUTPUT_AMOUNTS = "output_amounts";
const char* const LMDB_SPENT_KEYS = "spent_keys";

const char* const LMDB_TXPOOL_META = "txpool_meta";
const char* const LMDB_TXPOOL_BLOB = "txpool_blob";

const char* const LMDB_ALT_BLOCKS = "alt_blocks";

const char* const LMDB_HF_STARTING_HEIGHTS = "hf_starting_heights";
const char* const LMDB_HF_VERSIONS = "hf_versions";

const char* const LMDB_PROPERTIES = "properties";

const char zerokey[8] = {0};
const MDB_val zerokval = { sizeof(zerokey), (void *)zerokey };

const std::string lmdb_error(const std::string& error_string, int mdb_res)
{
  const std::string full_string = error_string + mdb_strerror(mdb_res);
  return full_string;
}

inline void lmdb_db_open(MDB_txn* txn, const char* name, int flags, MDB_dbi& dbi, const std::string& error_string)
{
  if (auto res = mdb_dbi_open(txn, name, flags, &dbi))
    throw0(cryptonote::DB_OPEN_FAILURE((lmdb_error(error_string + " : ", res) + std::string(" - you may want to start with --db-salvage")).c_str()));
}


}  // anonymous namespace

#define CURSOR(name) \
	if (!m_cur_ ## name) { \
	  int result = mdb_cursor_open(*m_write_txn, m_ ## name, &m_cur_ ## name); \
	  if (result) \
        throw0(DB_ERROR(lmdb_error("Failed to open cursor: ", result).c_str())); \
	}

#define RCURSOR(name) \
	if (!m_cur_ ## name) { \
	  int result = mdb_cursor_open(m_txn, m_ ## name, (MDB_cursor **)&m_cur_ ## name); \
	  if (result) \
        throw0(DB_ERROR(lmdb_error("Failed to open cursor: ", result).c_str())); \
	  if (m_cursors != &m_wcursors) \
	    m_tinfo->m_ti_rflags.m_rf_ ## name = true; \
	} else if (m_cursors != &m_wcursors && !m_tinfo->m_ti_rflags.m_rf_ ## name) { \
	  int result = mdb_cursor_renew(m_txn, m_cur_ ## name); \
      if (result) \
        throw0(DB_ERROR(lmdb_error("Failed to renew cursor: ", result).c_str())); \
	  m_tinfo->m_ti_rflags.m_rf_ ## name = true; \
	}

namespace cryptonote
{

typedef struct blk_height {
    crypto::hash bh_hash;
    uint64_t bh_height;
} blk_height;

typedef struct pre_rct_outkey {
    uint64_t amount_index;
    uint64_t output_id;
    pre_rct_output_data_t data;
} pre_rct_outkey;

typedef struct outkey {
    uint64_t amount_index;
    uint64_t output_id;
    output_data_t data;
} outkey;

typedef struct outtx {
    uint64_t output_id;
    crypto::hash tx_hash;
    uint64_t local_index;
} outtx;

std::atomic<uint64_t> mdb_txn_safe::num_active_txns{0};
std::atomic_flag mdb_txn_safe::creation_gate = ATOMIC_FLAG_INIT;

mdb_threadinfo::~mdb_threadinfo()
{
  MDB_cursor **cur = &m_ti_rcursors.m_txc_blocks;
  unsigned i;
  for (i=0; i<sizeof(mdb_txn_cursors)/sizeof(MDB_cursor *); i++)
    if (cur[i])
      mdb_cursor_close(cur[i]);
  if (m_ti_rtxn)
    mdb_txn_abort(m_ti_rtxn);
}

mdb_txn_safe::mdb_txn_safe(const bool check) : m_txn(NULL), m_tinfo(NULL), m_check(check)
{
  if (check)
  {
    while (creation_gate.test_and_set());
    num_active_txns++;
    creation_gate.clear();
  }
}

mdb_txn_safe::~mdb_txn_safe()
{
  if (!m_check)
    return;
  LOG_PRINT_L3("mdb_txn_safe: destructor");
  if (m_tinfo != nullptr)
  {
    mdb_txn_reset(m_tinfo->m_ti_rtxn);
    memset(&m_tinfo->m_ti_rflags, 0, sizeof(m_tinfo->m_ti_rflags));
  } else if (m_txn != nullptr)
  {
    if (m_batch_txn) // this is a batch txn and should have been handled before this point for safety
    {
      LOG_PRINT_L0("WARNING: mdb_txn_safe: m_txn is a batch txn and it's not NULL in destructor - calling mdb_txn_abort()");
    }
    else
    {
      // Example of when this occurs: a lookup fails, so a read-only txn is
      // aborted through this destructor. However, successful read-only txns
      // ideally should have been committed when done and not end up here.
      //
      // NOTE: not sure if this is ever reached for a non-batch write
      // transaction, but it's probably not ideal if it did.
      LOG_PRINT_L3("mdb_txn_safe: m_txn not NULL in destructor - calling mdb_txn_abort()");
    }
    mdb_txn_abort(m_txn);
  }
  num_active_txns--;
}

void mdb_txn_safe::uncheck()
{
  num_active_txns--;
  m_check = false;
}

void mdb_txn_safe::commit(std::string message)
{
  if (message.size() == 0)
  {
    message = "Failed to commit a transaction to the db";
  }

  if (auto result = mdb_txn_commit(m_txn))
  {
    m_txn = nullptr;
    throw0(DB_ERROR(lmdb_error(message + ": ", result).c_str()));
  }
  m_txn = nullptr;
}

void mdb_txn_safe::abort()
{
  LOG_PRINT_L3("mdb_txn_safe: abort()");
  if(m_txn != nullptr)
  {
    mdb_txn_abort(m_txn);
    m_txn = nullptr;
  }
  else
  {
    LOG_PRINT_L0("WARNING: mdb_txn_safe: abort() called, but m_txn is NULL");
  }
}

uint64_t mdb_txn_safe::num_active_tx() const
{
  return num_active_txns;
}

void mdb_txn_safe::prevent_new_txns()
{
  while (creation_gate.test_and_set());
}

void mdb_txn_safe::wait_no_active_txns()
{
  while (num_active_txns > 0);
}

void mdb_txn_safe::allow_new_txns()
{
  creation_gate.clear();
}

void lmdb_resized(MDB_env *env)
{
  mdb_txn_safe::prevent_new_txns();

  MGINFO("LMDB map resize detected.");

  MDB_envinfo mei;

  mdb_env_info(env, &mei);
  uint64_t old = mei.me_mapsize;

  mdb_txn_safe::wait_no_active_txns();

  int result = mdb_env_set_mapsize(env, 0);
  if (result)
    throw0(DB_ERROR(lmdb_error("Failed to set new mapsize: ", result).c_str()));

  mdb_env_info(env, &mei);
  uint64_t new_mapsize = mei.me_mapsize;

  MGINFO("LMDB Mapsize increased." << "  Old: " << old / (1024 * 1024) << "MiB" << ", New: " << new_mapsize / (1024 * 1024) << "MiB");

  mdb_txn_safe::allow_new_txns();
}

inline int lmdb_txn_begin(MDB_env *env, MDB_txn *parent, unsigned int flags, MDB_txn **txn)
{
  int res = mdb_txn_begin(env, parent, flags, txn);
  if (res == MDB_MAP_RESIZED) {
    lmdb_resized(env);
    res = mdb_txn_begin(env, parent, flags, txn);
  }
  return res;
}

inline int lmdb_txn_renew(MDB_txn *txn)
{
  int res = mdb_txn_renew(txn);
  if (res == MDB_MAP_RESIZED) {
    lmdb_resized(mdb_txn_env(txn));
    res = mdb_txn_renew(txn);
  }
  return res;
}

inline void BlockchainLMDB::check_open() const
{
  if (!m_open)
    throw0(DB_ERROR("DB operation attempted on a not-open DB instance"));
}

void BlockchainLMDB::do_resize(uint64_t increase_size)
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  CRITICAL_REGION_LOCAL(m_synchronization_lock);
  const uint64_t add_size = 1LL << 30;

  // check disk capacity
  try
  {
    boost::filesystem::path path(m_folder);
    boost::filesystem::space_info si = boost::filesystem::space(path);
    if(si.available < add_size)
    {
      MERROR("!! WARNING: Insufficient free space to extend database !!: " <<
          (si.available >> 20L) << " MB available, " << (add_size >> 20L) << " MB needed");
      return;
    }
  }
  catch(...)
  {
    // print something but proceed.
    MWARNING("Unable to query free disk space.");
  }

  MDB_envinfo mei;

  mdb_env_info(m_env, &mei);

  MDB_stat mst;

  mdb_env_stat(m_env, &mst);

  // add 1Gb per resize, instead of doing a percentage increase
  uint64_t new_mapsize = (uint64_t) mei.me_mapsize + add_size;

  // If given, use increase_size instead of above way of resizing.
  // This is currently used for increasing by an estimated size at start of new
  // batch txn.
  if (increase_size > 0)
    new_mapsize = mei.me_mapsize + increase_size;

  new_mapsize += (new_mapsize % mst.ms_psize);

  mdb_txn_safe::prevent_new_txns();

  if (m_write_txn != nullptr)
  {
    if (m_batch_active)
    {
      throw0(DB_ERROR("lmdb resizing not yet supported when batch transactions enabled!"));
    }
    else
    {
      throw0(DB_ERROR("attempting resize with write transaction in progress, this should not happen!"));
    }
  }

  mdb_txn_safe::wait_no_active_txns();

  int result = mdb_env_set_mapsize(m_env, new_mapsize);
  if (result)
    throw0(DB_ERROR(lmdb_error("Failed to set new mapsize: ", result).c_str()));

  MGINFO("LMDB Mapsize increased." << "  Old: " << mei.me_mapsize / (1024 * 1024) << "MiB" << ", New: " << new_mapsize / (1024 * 1024) << "MiB");

  mdb_txn_safe::allow_new_txns();
}

// threshold_size is used for batch transactions
bool BlockchainLMDB::need_resize(uint64_t threshold_size) const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
#if defined(ENABLE_AUTO_RESIZE)
  MDB_envinfo mei;

  mdb_env_info(m_env, &mei);

  MDB_stat mst;

  mdb_env_stat(m_env, &mst);

  // size_used doesn't include data yet to be committed, which can be
  // significant size during batch transactions. For that, we estimate the size
  // needed at the beginning of the batch transaction and pass in the
  // additional size needed.
  uint64_t size_used = mst.ms_psize * mei.me_last_pgno;

  MDEBUG("DB map size:     " << mei.me_mapsize);
  MDEBUG("Space used:      " << size_used);
  MDEBUG("Space remaining: " << mei.me_mapsize - size_used);
  MDEBUG("Size threshold:  " << threshold_size);
  float resize_percent = RESIZE_PERCENT;
  MDEBUG(boost::format("Percent used: %.04f  Percent threshold: %.04f") % ((double)size_used/mei.me_mapsize) % resize_percent);

  if (threshold_size > 0)
  {
    if (mei.me_mapsize - size_used < threshold_size)
    {
      MINFO("Threshold met (size-based)");
      return true;
    }
    else
      return false;
  }

  if ((double)size_used / mei.me_mapsize  > resize_percent)
  {
    MINFO("Threshold met (percent-based)");
    return true;
  }
  return false;
#else
  return false;
#endif
}

void BlockchainLMDB::check_and_resize_for_batch(uint64_t batch_num_blocks, uint64_t batch_bytes)
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  MTRACE("[" << __func__ << "] " << "checking DB size");
  const uint64_t min_increase_size = 512 * (1 << 20);
  uint64_t threshold_size = 0;
  uint64_t increase_size = 0;
  if (batch_num_blocks > 0)
  {
    threshold_size = get_estimated_batch_size(batch_num_blocks, batch_bytes);
    MDEBUG("calculated batch size: " << threshold_size);

    // The increased DB size could be a multiple of threshold_size, a fixed
    // size increase (> threshold_size), or other variations.
    //
    // Currently we use the greater of threshold size and a minimum size. The
    // minimum size increase is used to avoid frequent resizes when the batch
    // size is set to a very small numbers of blocks.
    increase_size = (threshold_size > min_increase_size) ? threshold_size : min_increase_size;
    MDEBUG("increase size: " << increase_size);
  }

  // if threshold_size is 0 (i.e. number of blocks for batch not passed in), it
  // will fall back to the percent-based threshold check instead of the
  // size-based check
  if (need_resize(threshold_size))
  {
    MGINFO("[batch] DB resize needed");
    do_resize(increase_size);
  }
}

uint64_t BlockchainLMDB::get_estimated_batch_size(uint64_t batch_num_blocks, uint64_t batch_bytes) const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  uint64_t threshold_size = 0;

  // batch size estimate * batch safety factor = final size estimate
  // Takes into account "reasonable" block size increases in batch.
  float batch_safety_factor = 1.7f;
  float batch_fudge_factor = batch_safety_factor * batch_num_blocks;
  // estimate of stored block expanded from raw block, including denormalization and db overhead.
  // Note that this probably doesn't grow linearly with block size.
  float db_expand_factor = 4.5f;
  uint64_t num_prev_blocks = 500;
  // For resizing purposes, allow for at least 4k average block size.
  uint64_t min_block_size = 4 * 1024;

  uint64_t block_stop = 0;
  uint64_t m_height = height();
  if (m_height > 1)
    block_stop = m_height - 1;
  uint64_t block_start = 0;
  if (block_stop >= num_prev_blocks)
    block_start = block_stop - num_prev_blocks + 1;
  uint32_t num_blocks_used = 0;
  uint64_t total_block_size = 0;
  MDEBUG("[" << __func__ << "] " << "m_height: " << m_height << "  block_start: " << block_start << "  block_stop: " << block_stop);
  size_t avg_block_size = 0;
  if (batch_bytes)
  {
    avg_block_size = batch_bytes / batch_num_blocks;
    goto estim;
  }
  if (m_height == 0)
  {
    MDEBUG("No existing blocks to check for average block size");
  }
  else if (m_cum_count >= num_prev_blocks)
  {
    avg_block_size = m_cum_size / m_cum_count;
    MDEBUG("average block size across recent " << m_cum_count << " blocks: " << avg_block_size);
    m_cum_size = 0;
    m_cum_count = 0;
  }
  else
  {
    MDB_txn *rtxn;
    mdb_txn_cursors *rcurs;
    bool my_rtxn = block_rtxn_start(&rtxn, &rcurs);
    for (uint64_t block_num = block_start; block_num <= block_stop; ++block_num)
    {
      uint32_t block_size = get_block_size(block_num);
      total_block_size += block_size;
      // Track number of blocks being totalled here instead of assuming, in case
      // some blocks were to be skipped for being outliers.
      ++num_blocks_used;
    }
    if (my_rtxn) block_rtxn_stop();
    avg_block_size = total_block_size / (num_blocks_used ? num_blocks_used : 1);
    MDEBUG("average block size across recent " << num_blocks_used << " blocks: " << avg_block_size);
  }
estim:
  if (avg_block_size < min_block_size)
    avg_block_size = min_block_size;
  MDEBUG("estimated average block size for batch: " << avg_block_size);

  // bigger safety margin on smaller block sizes
  if (batch_fudge_factor < 5000.0)
    batch_fudge_factor = 5000.0;
  threshold_size = avg_block_size * db_expand_factor * batch_fudge_factor;
  return threshold_size;
}

void BlockchainLMDB::add_block(const block& blk, size_t block_size, const difficulty_type& cumulative_difficulty, const uint64_t& coins_generated, const crypto::hash& blk_hash)
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();
  mdb_txn_cursors *m_cursors = &m_wcursors;
  uint64_t m_height = height();

  CURSOR(block_heights)
  blk_height bh = {blk_hash, m_height};
  MDB_val_set(val_h, bh);
  if (mdb_cursor_get(m_cur_block_heights, (MDB_val *)&zerokval, &val_h, MDB_GET_BOTH) == 0)
    throw1(BLOCK_EXISTS("Attempting to add block that's already in the db"));

  if (m_height > 0)
  {
    MDB_val_set(parent_key, blk.prev_id);
    int result = mdb_cursor_get(m_cur_block_heights, (MDB_val *)&zerokval, &parent_key, MDB_GET_BOTH);
    if (result)
    {
      LOG_PRINT_L3("m_height: " << m_height);
      LOG_PRINT_L3("parent_key: " << blk.prev_id);
      throw0(DB_ERROR(lmdb_error("Failed to get top block hash to check for new block's parent: ", result).c_str()));
    }
    blk_height *prev = (blk_height *)parent_key.mv_data;
    if (prev->bh_height != m_height - 1)
      throw0(BLOCK_PARENT_DNE("Top block is not new block's parent"));
  }

  int result = 0;

  MDB_val_set(key, m_height);

  CURSOR(blocks)
  CURSOR(block_info)

  // this call to mdb_cursor_put will change height()
  cryptonote::blobdata block_blob(block_to_blob(blk));
  MDB_val_sized(blob, block_blob);
  result = mdb_cursor_put(m_cur_blocks, &key, &blob, MDB_APPEND);
  if (result)
    throw0(DB_ERROR(lmdb_error("Failed to add block blob to db transaction: ", result).c_str()));

  mdb_block_info bi;
  bi.bi_height = m_height;
  bi.bi_timestamp = blk.timestamp;
  bi.bi_coins = coins_generated;
  bi.bi_size = block_size;
  bi.bi_diff = cumulative_difficulty;
  bi.bi_hash = blk_hash;

  MDB_val_set(val, bi);
  result = mdb_cursor_put(m_cur_block_info, (MDB_val *)&zerokval, &val, MDB_APPENDDUP);
  if (result)
    throw0(DB_ERROR(lmdb_error("Failed to add block info to db transaction: ", result).c_str()));

  result = mdb_cursor_put(m_cur_block_heights, (MDB_val *)&zerokval, &val_h, 0);
  if (result)
    throw0(DB_ERROR(lmdb_error("Failed to add block height by hash to db transaction: ", result).c_str()));

  m_cum_size += block_size;
  m_cum_count++;
}

void BlockchainLMDB::remove_block()
{
  int result;

  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();
  uint64_t m_height = height();

  if (m_height == 0)
    throw0(BLOCK_DNE ("Attempting to remove block from an empty blockchain"));

  mdb_txn_cursors *m_cursors = &m_wcursors;
  CURSOR(block_info)
  CURSOR(block_heights)
  CURSOR(blocks)
  MDB_val_copy<uint64_t> k(m_height - 1);
  MDB_val h = k;
  if ((result = mdb_cursor_get(m_cur_block_info, (MDB_val *)&zerokval, &h, MDB_GET_BOTH)))
      throw1(BLOCK_DNE(lmdb_error("Attempting to remove block that's not in the db: ", result).c_str()));

  // must use h now; deleting from m_block_info will invalidate it
  mdb_block_info *bi = (mdb_block_info *)h.mv_data;
  blk_height bh = {bi->bi_hash, 0};
  h.mv_data = (void *)&bh;
  h.mv_size = sizeof(bh);
  if ((result = mdb_cursor_get(m_cur_block_heights, (MDB_val *)&zerokval, &h, MDB_GET_BOTH)))
      throw1(DB_ERROR(lmdb_error("Failed to locate block height by hash for removal: ", result).c_str()));
  if ((result = mdb_cursor_del(m_cur_block_heights, 0)))
      throw1(DB_ERROR(lmdb_error("Failed to add removal of block height by hash to db transaction: ", result).c_str()));

  if ((result = mdb_cursor_del(m_cur_blocks, 0)))
      throw1(DB_ERROR(lmdb_error("Failed to add removal of block to db transaction: ", result).c_str()));

  if ((result = mdb_cursor_del(m_cur_block_info, 0)))
      throw1(DB_ERROR(lmdb_error("Failed to add removal of block info to db transaction: ", result).c_str()));
}

uint64_t BlockchainLMDB::add_transaction_data(const crypto::hash& blk_hash, const std::pair<transaction, blobdata>& txp, const crypto::hash& tx_hash)
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();
  mdb_txn_cursors *m_cursors = &m_wcursors;
  uint64_t m_height = height();

  int result;
  uint64_t tx_id = get_tx_count();

  CURSOR(txs)
  CURSOR(tx_indices)

  MDB_val_set(val_tx_id, tx_id);
  MDB_val_set(val_h, tx_hash);
  result = mdb_cursor_get(m_cur_tx_indices, (MDB_val *)&zerokval, &val_h, MDB_GET_BOTH);
  if (result == 0) {
    txindex *tip = (txindex *)val_h.mv_data;
    throw1(TX_EXISTS(std::string("Attempting to add transaction that's already in the db (tx id ").append(boost::lexical_cast<std::string>(tip->data.tx_id)).append(")").c_str()));
  } else if (result != MDB_NOTFOUND) {
    throw1(DB_ERROR(lmdb_error(std::string("Error checking if tx index exists for tx hash ") + epee::string_tools::pod_to_hex(tx_hash) + ": ", result).c_str()));
  }

  const cryptonote::transaction &tx = txp.first;
  txindex ti;
  ti.key = tx_hash;
  ti.data.tx_id = tx_id;
  ti.data.unlock_time = tx.unlock_time;
  ti.data.block_id = m_height;  // we don't need blk_hash since we know m_height

  val_h.mv_size = sizeof(ti);
  val_h.mv_data = (void *)&ti;

  result = mdb_cursor_put(m_cur_tx_indices, (MDB_val *)&zerokval, &val_h, 0);
  if (result)
    throw0(DB_ERROR(lmdb_error("Failed to add tx data to db transaction: ", result).c_str()));

  const cryptonote::blobdata &blob = txp.second;
  MDB_val_sized(blobval, blob);
  result = mdb_cursor_put(m_cur_txs, &val_tx_id, &blobval, MDB_APPEND);
  if (result)
    throw0(DB_ERROR(lmdb_error("Failed to add tx blob to db transaction: ", result).c_str()));

  return tx_id;
}

// TODO: compare pros and cons of looking up the tx hash's tx index once and
// passing it in to functions like this
void BlockchainLMDB::remove_transaction_data(const crypto::hash& tx_hash, const transaction& tx)
{
  int result;

  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();

  mdb_txn_cursors *m_cursors = &m_wcursors;
  CURSOR(tx_indices)
  CURSOR(txs)
  CURSOR(tx_outputs)

  MDB_val_set(val_h, tx_hash);

  if (mdb_cursor_get(m_cur_tx_indices, (MDB_val *)&zerokval, &val_h, MDB_GET_BOTH))
      throw1(TX_DNE("Attempting to remove transaction that isn't in the db"));
  txindex *tip = (txindex *)val_h.mv_data;
  MDB_val_set(val_tx_id, tip->data.tx_id);

  if ((result = mdb_cursor_get(m_cur_txs, &val_tx_id, NULL, MDB_SET)))
      throw1(DB_ERROR(lmdb_error("Failed to locate tx for removal: ", result).c_str()));
  result = mdb_cursor_del(m_cur_txs, 0);
  if (result)
      throw1(DB_ERROR(lmdb_error("Failed to add removal of tx to db transaction: ", result).c_str()));

  remove_tx_outputs(tip->data.tx_id, tx);

  result = mdb_cursor_get(m_cur_tx_outputs, &val_tx_id, NULL, MDB_SET);
  if (result == MDB_NOTFOUND)
    LOG_PRINT_L1("tx has no outputs to remove: " << tx_hash);
  else if (result)
    throw1(DB_ERROR(lmdb_error("Failed to locate tx outputs for removal: ", result).c_str()));
  if (!result)
  {
    result = mdb_cursor_del(m_cur_tx_outputs, 0);
    if (result)
      throw1(DB_ERROR(lmdb_error("Failed to add removal of tx outputs to db transaction: ", result).c_str()));
  }

  // Don't delete the tx_indices entry until the end, after we're done with val_tx_id
  if (mdb_cursor_del(m_cur_tx_indices, 0))
      throw1(DB_ERROR("Failed to add removal of tx index to db transaction"));
}

uint64_t BlockchainLMDB::add_output(const crypto::hash& tx_hash,
    const tx_out& tx_output,
    const uint64_t& local_index,
    const uint64_t unlock_time,
    const rct::key *commitment)
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();
  mdb_txn_cursors *m_cursors = &m_wcursors;
  uint64_t m_height = height();
  uint64_t m_num_outputs = num_outputs();

  int result = 0;

  CURSOR(output_txs)
  CURSOR(output_amounts)

  if (tx_output.target.type() != typeid(txout_to_key))
    throw0(DB_ERROR("Wrong output type: expected txout_to_key"));
  if (tx_output.amount == 0 && !commitment)
    throw0(DB_ERROR("RCT output without commitment"));

  outtx ot = {m_num_outputs, tx_hash, local_index};
  MDB_val_set(vot, ot);

  result = mdb_cursor_put(m_cur_output_txs, (MDB_val *)&zerokval, &vot, MDB_APPENDDUP);
  if (result)
    throw0(DB_ERROR(lmdb_error("Failed to add output tx hash to db transaction: ", result).c_str()));

  outkey ok;
  MDB_val data;
  MDB_val_copy<uint64_t> val_amount(tx_output.amount);
  result = mdb_cursor_get(m_cur_output_amounts, &val_amount, &data, MDB_SET);
  if (!result)
    {
      mdb_size_t num_elems = 0;
      result = mdb_cursor_count(m_cur_output_amounts, &num_elems);
      if (result)
        throw0(DB_ERROR(std::string("Failed to get number of outputs for amount: ").append(mdb_strerror(result)).c_str()));
      ok.amount_index = num_elems;
    }
  else if (result != MDB_NOTFOUND)
    throw0(DB_ERROR(lmdb_error("Failed to get output amount in db transaction: ", result).c_str()));
  else
    ok.amount_index = 0;
  ok.output_id = m_num_outputs;
  ok.data.pubkey = boost::get < txout_to_key > (tx_output.target).key;
  ok.data.unlock_time = unlock_time;
  ok.data.height = m_height;
  if (tx_output.amount == 0)
  {
    ok.data.commitment = *commitment;
    data.mv_size = sizeof(ok);
  }
  else
  {
    data.mv_size = sizeof(pre_rct_outkey);
  }
  data.mv_data = &ok;

  if ((result = mdb_cursor_put(m_cur_output_amounts, &val_amount, &data, MDB_APPENDDUP)))
      throw0(DB_ERROR(lmdb_error("Failed to add output pubkey to db transaction: ", result).c_str()));

  return ok.amount_index;
}

void BlockchainLMDB::add_tx_amount_output_indices(const uint64_t tx_id,
    const std::vector<uint64_t>& amount_output_indices)
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();
  mdb_txn_cursors *m_cursors = &m_wcursors;
  CURSOR(tx_outputs)

  int result = 0;

  size_t num_outputs = amount_output_indices.size();

  MDB_val_set(k_tx_id, tx_id);
  MDB_val v;
  v.mv_data = num_outputs ? (void *)amount_output_indices.data() : (void*)"";
  v.mv_size = sizeof(uint64_t) * num_outputs;
  // LOG_PRINT_L1("tx_outputs[tx_hash] size: " << v.mv_size);

  result = mdb_cursor_put(m_cur_tx_outputs, &k_tx_id, &v, MDB_APPEND);
  if (result)
    throw0(DB_ERROR(std::string("Failed to add <tx hash, amount output index array> to db transaction: ").append(mdb_strerror(result)).c_str()));
}

void BlockchainLMDB::remove_tx_outputs(const uint64_t tx_id, const transaction& tx)
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);

  std::vector<std::vector<uint64_t>> amount_output_indices_set = get_tx_amount_output_indices(tx_id, 1);
  const std::vector<uint64_t> &amount_output_indices = amount_output_indices_set.front();

  if (amount_output_indices.empty())
  {
    if (tx.vout.empty())
      LOG_PRINT_L2("tx has no outputs, so no output indices");
    else
      throw0(DB_ERROR("tx has outputs, but no output indices found"));
  }

  bool is_pseudo_rct = tx.vin.size() == 1 && tx.vin[0].type() == typeid(txin_gen);
  for (size_t i = tx.vout.size(); i-- > 0;)
  {
    uint64_t amount = is_pseudo_rct ? 0 : tx.vout[i].amount;
    remove_output(amount, amount_output_indices[i]);
  }
}

void BlockchainLMDB::remove_output(const uint64_t amount, const uint64_t& out_index)
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();
  mdb_txn_cursors *m_cursors = &m_wcursors;
  CURSOR(output_amounts);
  CURSOR(output_txs);

  MDB_val_set(k, amount);
  MDB_val_set(v, out_index);

  auto result = mdb_cursor_get(m_cur_output_amounts, &k, &v, MDB_GET_BOTH);
  if (result == MDB_NOTFOUND)
    throw1(OUTPUT_DNE("Attempting to get an output index by amount and amount index, but amount not found"));
  else if (result)
    throw0(DB_ERROR(lmdb_error("DB error attempting to get an output", result).c_str()));

  const pre_rct_outkey *ok = (const pre_rct_outkey *)v.mv_data;
  MDB_val_set(otxk, ok->output_id);
  result = mdb_cursor_get(m_cur_output_txs, (MDB_val *)&zerokval, &otxk, MDB_GET_BOTH);
  if (result == MDB_NOTFOUND)
  {
    throw0(DB_ERROR("Unexpected: global output index not found in m_output_txs"));
  }
  else if (result)
  {
    throw1(DB_ERROR(lmdb_error("Error adding removal of output tx to db transaction", result).c_str()));
  }
  result = mdb_cursor_del(m_cur_output_txs, 0);
  if (result)
    throw0(DB_ERROR(lmdb_error(std::string("Error deleting output index ").append(boost::lexical_cast<std::string>(out_index).append(": ")).c_str(), result).c_str()));

  // now delete the amount
  result = mdb_cursor_del(m_cur_output_amounts, 0);
  if (result)
    throw0(DB_ERROR(lmdb_error(std::string("Error deleting amount for output index ").append(boost::lexical_cast<std::string>(out_index).append(": ")).c_str(), result).c_str()));
}

void BlockchainLMDB::add_spent_key(const crypto::key_image& k_image)
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();
  mdb_txn_cursors *m_cursors = &m_wcursors;

  CURSOR(spent_keys)

  MDB_val k = {sizeof(k_image), (void *)&k_image};
  if (auto result = mdb_cursor_put(m_cur_spent_keys, (MDB_val *)&zerokval, &k, MDB_NODUPDATA)) {
    if (result == MDB_KEYEXIST)
      throw1(KEY_IMAGE_EXISTS("Attempting to add spent key image that's already in the db"));
    else
      throw1(DB_ERROR(lmdb_error("Error adding spent key image to db transaction: ", result).c_str()));
  }
}

void BlockchainLMDB::remove_spent_key(const crypto::key_image& k_image)
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();
  mdb_txn_cursors *m_cursors = &m_wcursors;

  CURSOR(spent_keys)

  MDB_val k = {sizeof(k_image), (void *)&k_image};
  auto result = mdb_cursor_get(m_cur_spent_keys, (MDB_val *)&zerokval, &k, MDB_GET_BOTH);
  if (result != 0 && result != MDB_NOTFOUND)
      throw1(DB_ERROR(lmdb_error("Error finding spent key to remove", result).c_str()));
  if (!result)
  {
    result = mdb_cursor_del(m_cur_spent_keys, 0);
    if (result)
        throw1(DB_ERROR(lmdb_error("Error adding removal of key image to db transaction", result).c_str()));
  }
}

BlockchainLMDB::~BlockchainLMDB()
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);

  // batch transaction shouldn't be active at this point. If it is, consider it aborted.
  if (m_batch_active)
  {
    try { batch_abort(); }
    catch (...) { /* ignore */ }
  }
  if (m_open)
    close();
}

BlockchainLMDB::BlockchainLMDB(bool batch_transactions): BlockchainDB()
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  // initialize folder to something "safe" just in case
  // someone accidentally misuses this class...
  m_folder = "thishsouldnotexistbecauseitisgibberish";

  m_batch_transactions = batch_transactions;
  m_write_txn = nullptr;
  m_write_batch_txn = nullptr;
  m_batch_active = false;
  m_cum_size = 0;
  m_cum_count = 0;

  // reset may also need changing when initialize things here

  m_hardfork = nullptr;
}

void BlockchainLMDB::open(const std::string& filename, const int db_flags, uint32_t db_readers)
{
  int result;
  int mdb_flags = MDB_NORDAHEAD;

  LOG_PRINT_L3("BlockchainLMDB::" << __func__);

  if (m_open)
    throw0(DB_OPEN_FAILURE("Attempted to open db, but it's already open"));

  boost::filesystem::path direc(filename);
  if (boost::filesystem::exists(direc))
  {
    if (!boost::filesystem::is_directory(direc))
      throw0(DB_OPEN_FAILURE("LMDB needs a directory path, but a file was passed"));
  }
  else
  {
    if (!boost::filesystem::create_directories(direc))
      throw0(DB_OPEN_FAILURE(std::string("Failed to create directory ").append(filename).c_str()));
  }

  // check for existing LMDB files in base directory
  boost::filesystem::path old_files = direc.parent_path();
  if (boost::filesystem::exists(old_files / CRYPTONOTE_BLOCKCHAINDATA_FILENAME)
      || boost::filesystem::exists(old_files / CRYPTONOTE_BLOCKCHAINDATA_LOCK_FILENAME))
  {
    LOG_PRINT_L0("Found existing LMDB files in " << old_files.string());
    LOG_PRINT_L0("Move " << CRYPTONOTE_BLOCKCHAINDATA_FILENAME << " and/or " << CRYPTONOTE_BLOCKCHAINDATA_LOCK_FILENAME << " to " << filename << ", or delete them, and then restart");
    throw DB_ERROR("Database could not be opened");
  }

  boost::optional<bool> is_hdd_result = tools::is_hdd(filename.c_str());
  if (is_hdd_result)
  {
    if (is_hdd_result.value())
        MCLOG_RED(el::Level::Warning, "global", "The blockchain is on a rotating drive: this will be very slow, use an SSD if possible");
  }

  m_folder = filename;

#ifdef __OpenBSD__
  if ((mdb_flags & MDB_WRITEMAP) == 0) {
    MCLOG_RED(el::Level::Info, "global", "Running on OpenBSD: forcing WRITEMAP");
    mdb_flags |= MDB_WRITEMAP;
  }
#endif
  // set up lmdb environment
  if ((result = mdb_env_create(&m_env)))
    throw0(DB_ERROR(lmdb_error("Failed to create lmdb environment: ", result).c_str()));
  if ((result = mdb_env_set_maxdbs(m_env, 20)))
    throw0(DB_ERROR(lmdb_error("Failed to set max number of dbs: ", result).c_str()));

  if (db_readers == 0) //this should never happen, command line defaults to 126
  {
    int threads = tools::get_max_concurrency();
    if (threads > 110 &&	/* maxreaders default is 126, leave some slots for other read processes */
      (result = mdb_env_set_maxreaders(m_env, threads+16)))
      throw0(DB_ERROR(lmdb_error("Failed to set max number of readers: ", result).c_str()));
  }
  else
  {
    if ((result = mdb_env_set_maxreaders(m_env, db_readers)))
      throw0(DB_ERROR(lmdb_error("Failed to set max number of readers: ", result).c_str()));
  }

  size_t mapsize = DEFAULT_MAPSIZE;

  if (db_flags & DBF_FAST)
    mdb_flags |= MDB_NOSYNC;
  if (db_flags & DBF_FASTEST)
    mdb_flags |= MDB_NOSYNC | MDB_WRITEMAP | MDB_MAPASYNC;
  if (db_flags & DBF_RDONLY)
    mdb_flags = MDB_RDONLY;
  if (db_flags & DBF_SALVAGE)
    mdb_flags |= MDB_PREVSNAPSHOT;

  if (auto result = mdb_env_open(m_env, filename.c_str(), mdb_flags, 0644))
    throw0(DB_ERROR(lmdb_error("Failed to open lmdb environment: ", result).c_str()));

  MDB_envinfo mei;
  mdb_env_info(m_env, &mei);
  uint64_t cur_mapsize = (uint64_t)mei.me_mapsize;

  if (cur_mapsize < mapsize)
  {
    if (auto result = mdb_env_set_mapsize(m_env, mapsize))
      throw0(DB_ERROR(lmdb_error("Failed to set max memory map size: ", result).c_str()));
    mdb_env_info(m_env, &mei);
    cur_mapsize = (uint64_t)mei.me_mapsize;
    LOG_PRINT_L1("LMDB memory map size: " << cur_mapsize);
  }

  if (need_resize())
  {
    LOG_PRINT_L0("LMDB memory map needs to be resized, doing that now.");
    do_resize();
  }

  int txn_flags = 0;
  if (mdb_flags & MDB_RDONLY)
    txn_flags |= MDB_RDONLY;

  // get a read/write MDB_txn, depending on mdb_flags
  mdb_txn_safe txn;
  if (auto mdb_res = mdb_txn_begin(m_env, NULL, txn_flags, txn))
    throw0(DB_ERROR(lmdb_error("Failed to create a transaction for the db: ", mdb_res).c_str()));

  // open necessary databases, and set properties as needed
  // uses macros to avoid having to change things too many places
  lmdb_db_open(txn, LMDB_BLOCKS, MDB_INTEGERKEY | MDB_CREATE, m_blocks, "Failed to open db handle for m_blocks");

  lmdb_db_open(txn, LMDB_BLOCK_INFO, MDB_INTEGERKEY | MDB_CREATE | MDB_DUPSORT | MDB_DUPFIXED, m_block_info, "Failed to open db handle for m_block_info");
  lmdb_db_open(txn, LMDB_BLOCK_HEIGHTS, MDB_INTEGERKEY | MDB_CREATE | MDB_DUPSORT | MDB_DUPFIXED, m_block_heights, "Failed to open db handle for m_block_heights");

  lmdb_db_open(txn, LMDB_TXS, MDB_INTEGERKEY | MDB_CREATE, m_txs, "Failed to open db handle for m_txs");
  lmdb_db_open(txn, LMDB_TX_INDICES, MDB_INTEGERKEY | MDB_CREATE | MDB_DUPSORT | MDB_DUPFIXED, m_tx_indices, "Failed to open db handle for m_tx_indices");
  lmdb_db_open(txn, LMDB_TX_OUTPUTS, MDB_INTEGERKEY | MDB_CREATE, m_tx_outputs, "Failed to open db handle for m_tx_outputs");

  lmdb_db_open(txn, LMDB_OUTPUT_TXS, MDB_INTEGERKEY | MDB_CREATE | MDB_DUPSORT | MDB_DUPFIXED, m_output_txs, "Failed to open db handle for m_output_txs");
  lmdb_db_open(txn, LMDB_OUTPUT_AMOUNTS, MDB_INTEGERKEY | MDB_DUPSORT | MDB_DUPFIXED | MDB_CREATE, m_output_amounts, "Failed to open db handle for m_output_amounts");

  lmdb_db_open(txn, LMDB_SPENT_KEYS, MDB_INTEGERKEY | MDB_CREATE | MDB_DUPSORT | MDB_DUPFIXED, m_spent_keys, "Failed to open db handle for m_spent_keys");

  lmdb_db_open(txn, LMDB_TXPOOL_META, MDB_CREATE, m_txpool_meta, "Failed to open db handle for m_txpool_meta");
  lmdb_db_open(txn, LMDB_TXPOOL_BLOB, MDB_CREATE, m_txpool_blob, "Failed to open db handle for m_txpool_blob");

  lmdb_db_open(txn, LMDB_ALT_BLOCKS, MDB_CREATE, m_alt_blocks, "Failed to open db handle for m_alt_blocks");

  // this subdb is dropped on sight, so it may not be present when we open the DB.
  // Since we use MDB_CREATE, we'll get an exception if we open read-only and it does not exist.
  // So we don't open for read-only, and also not drop below. It is not used elsewhere.
  if (!(mdb_flags & MDB_RDONLY))
    lmdb_db_open(txn, LMDB_HF_STARTING_HEIGHTS, MDB_CREATE, m_hf_starting_heights, "Failed to open db handle for m_hf_starting_heights");

  lmdb_db_open(txn, LMDB_HF_VERSIONS, MDB_INTEGERKEY | MDB_CREATE, m_hf_versions, "Failed to open db handle for m_hf_versions");

  lmdb_db_open(txn, LMDB_PROPERTIES, MDB_CREATE, m_properties, "Failed to open db handle for m_properties");

  mdb_set_dupsort(txn, m_spent_keys, compare_hash32);
  mdb_set_dupsort(txn, m_block_heights, compare_hash32);
  mdb_set_dupsort(txn, m_tx_indices, compare_hash32);
  mdb_set_dupsort(txn, m_output_amounts, compare_uint64);
  mdb_set_dupsort(txn, m_output_txs, compare_uint64);
  mdb_set_dupsort(txn, m_block_info, compare_uint64);

  mdb_set_compare(txn, m_txpool_meta, compare_hash32);
  mdb_set_compare(txn, m_txpool_blob, compare_hash32);
  mdb_set_compare(txn, m_alt_blocks, compare_hash32);
  mdb_set_compare(txn, m_properties, compare_string);

  if (!(mdb_flags & MDB_RDONLY))
  {
    result = mdb_drop(txn, m_hf_starting_heights, 1);
    if (result && result != MDB_NOTFOUND)
      throw0(DB_ERROR(lmdb_error("Failed to drop m_hf_starting_heights: ", result).c_str()));
  }

  // get and keep current height
  MDB_stat db_stats;
  if ((result = mdb_stat(txn, m_blocks, &db_stats)))
    throw0(DB_ERROR(lmdb_error("Failed to query m_blocks: ", result).c_str()));
  LOG_PRINT_L2("Setting m_height to: " << db_stats.ms_entries);
  uint64_t m_height = db_stats.ms_entries;

  bool compatible = true;

  MDB_val_str(k, "version");
  MDB_val v;
  auto get_result = mdb_get(txn, m_properties, &k, &v);
  if (get_result == MDB_SUCCESS)
  {
    const uint32_t db_version = *(const uint32_t*)v.mv_data;
    if (db_version > VERSION)
    {
      MWARNING("Existing lmdb database was made by a later version (" << db_version << "). We don't know how it will change yet.");
      compatible = false;
    }
    else if (db_version < VERSION)
    {
      if (db_version == 2)
      {
        MWARNING("The database contains deprecated data.");
        MWARNING("You can potentially save disk space by resyncing.");
      }
      else
      {
        compatible = false;
      }
    }
  }

  if (!compatible)
  {
    txn.abort();
    mdb_env_close(m_env);
    m_open = false;
    MFATAL("Existing lmdb database is incompatible with this version.");
    MFATAL("Please delete the existing database and resync.");
    return;
  }

  if (!(mdb_flags & MDB_RDONLY))
  {
    // only write version on an empty DB
    if (m_height == 0)
    {
      MDB_val_str(k, "version");
      MDB_val_copy<uint32_t> v(VERSION);
      auto put_result = mdb_put(txn, m_properties, &k, &v, 0);
      if (put_result != MDB_SUCCESS)
      {
        txn.abort();
        mdb_env_close(m_env);
        m_open = false;
        MERROR("Failed to write version to database.");
        return;
      }
    }
  }

  // commit the transaction
  txn.commit();

  m_block_cache.reserve(m_height);
  m_block_cache_height.store(0, std::memory_order_release);

  m_open = true;
  // from here, init should be finished
}

void BlockchainLMDB::close()
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  if (m_batch_active)
  {
    LOG_PRINT_L3("close() first calling batch_abort() due to active batch transaction");
    batch_abort();
  }
  this->sync();
  m_tinfo.reset();

  // FIXME: not yet thread safe!!!  Use with care.
  mdb_env_close(m_env);

  m_block_cache.clear();
  m_block_cache_height.store(0, std::memory_order_release);

  m_open = false;
}

void BlockchainLMDB::sync()
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();

  if (is_read_only())
    return;

  // Does nothing unless LMDB environment was opened with MDB_NOSYNC or in part
  // MDB_NOMETASYNC. Force flush to be synchronous.
  if (auto result = mdb_env_sync(m_env, true))
  {
    throw0(DB_ERROR(lmdb_error("Failed to sync database: ", result).c_str()));
  }
}

void BlockchainLMDB::safesyncmode(const bool onoff)
{
  MINFO("switching safe mode " << (onoff ? "on" : "off"));
  mdb_env_set_flags(m_env, MDB_NOSYNC|MDB_MAPASYNC, !onoff);
}

void BlockchainLMDB::reset()
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();

  mdb_txn_safe txn;
  if (auto result = lmdb_txn_begin(m_env, NULL, 0, txn))
    throw0(DB_ERROR(lmdb_error("Failed to create a transaction for the db: ", result).c_str()));

  if (auto result = mdb_drop(txn, m_blocks, 0))
    throw0(DB_ERROR(lmdb_error("Failed to drop m_blocks: ", result).c_str()));
  if (auto result = mdb_drop(txn, m_block_info, 0))
    throw0(DB_ERROR(lmdb_error("Failed to drop m_block_info: ", result).c_str()));
  if (auto result = mdb_drop(txn, m_block_heights, 0))
    throw0(DB_ERROR(lmdb_error("Failed to drop m_block_heights: ", result).c_str()));
  if (auto result = mdb_drop(txn, m_tx_indices, 0))
    throw0(DB_ERROR(lmdb_error("Failed to drop m_tx_indices: ", result).c_str()));
  if (auto result = mdb_drop(txn, m_tx_outputs, 0))
    throw0(DB_ERROR(lmdb_error("Failed to drop m_tx_outputs: ", result).c_str()));
  if (auto result = mdb_drop(txn, m_output_txs, 0))
    throw0(DB_ERROR(lmdb_error("Failed to drop m_output_txs: ", result).c_str()));
  if (auto result = mdb_drop(txn, m_output_amounts, 0))
    throw0(DB_ERROR(lmdb_error("Failed to drop m_output_amounts: ", result).c_str()));
  if (auto result = mdb_drop(txn, m_spent_keys, 0))
    throw0(DB_ERROR(lmdb_error("Failed to drop m_spent_keys: ", result).c_str()));
  (void)mdb_drop(txn, m_hf_starting_heights, 0); // this one is dropped in new code
  if (auto result = mdb_drop(txn, m_hf_versions, 0))
    throw0(DB_ERROR(lmdb_error("Failed to drop m_hf_versions: ", result).c_str()));
  if (auto result = mdb_drop(txn, m_properties, 0))
    throw0(DB_ERROR(lmdb_error("Failed to drop m_properties: ", result).c_str()));

  // init with current version
  MDB_val_str(k, "version");
  MDB_val_copy<uint32_t> v(VERSION);
  if (auto result = mdb_put(txn, m_properties, &k, &v, 0))
    throw0(DB_ERROR(lmdb_error("Failed to write version to database: ", result).c_str()));

  txn.commit();
  m_cum_size = 0;
  m_cum_count = 0;
  m_block_cache.clear();
  m_block_cache_height.store(0, std::memory_order_release);
}

std::vector<std::string> BlockchainLMDB::get_filenames() const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  std::vector<std::string> filenames;

  boost::filesystem::path datafile(m_folder);
  datafile /= CRYPTONOTE_BLOCKCHAINDATA_FILENAME;
  boost::filesystem::path lockfile(m_folder);
  lockfile /= CRYPTONOTE_BLOCKCHAINDATA_LOCK_FILENAME;

  filenames.push_back(datafile.string());
  filenames.push_back(lockfile.string());

  return filenames;
}

bool BlockchainLMDB::remove_data_file(const std::string& folder) const
{
  const std::string filename = folder + "/data.mdb";
  try
  {
    boost::filesystem::remove(filename);
  }
  catch (const std::exception &e)
  {
    MERROR("Failed to remove " << filename << ": " << e.what());
    return false;
  }
  return true;
}

std::string BlockchainLMDB::get_db_name() const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);

  return std::string("lmdb");
}

// TODO: this?
bool BlockchainLMDB::lock()
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();
  return false;
}

// TODO: this?
void BlockchainLMDB::unlock()
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();
}

#define TXN_PREFIX(flags); \
  mdb_txn_safe auto_txn; \
  mdb_txn_safe* txn_ptr = &auto_txn; \
  if (m_batch_active) \
    txn_ptr = m_write_txn; \
  else \
  { \
    if (auto mdb_res = lmdb_txn_begin(m_env, NULL, flags, auto_txn)) \
      throw0(DB_ERROR(lmdb_error(std::string("Failed to create a transaction for the db in ")+__FUNCTION__+": ", mdb_res).c_str())); \
  } \

#define TXN_PREFIX_RDONLY() \
  MDB_txn *m_txn; \
  mdb_txn_cursors *m_cursors; \
  mdb_txn_safe auto_txn; \
  bool my_rtxn = block_rtxn_start(&m_txn, &m_cursors); \
  if (my_rtxn) auto_txn.m_tinfo = m_tinfo.get(); \
  else auto_txn.uncheck()
#define TXN_POSTFIX_RDONLY()

#define TXN_POSTFIX_SUCCESS() \
  do { \
    if (! m_batch_active) \
      auto_txn.commit(); \
  } while(0)


// The below two macros are for DB access within block add/remove, whether
// regular batch txn is in use or not. m_write_txn is used as a batch txn, even
// if it's only within block add/remove.
//
// DB access functions that may be called both within block add/remove and
// without should use these. If the function will be called ONLY within block
// add/remove, m_write_txn alone may be used instead of these macros.

#define TXN_BLOCK_PREFIX(flags); \
  mdb_txn_safe auto_txn; \
  mdb_txn_safe* txn_ptr = &auto_txn; \
  if (m_batch_active || m_write_txn) \
    txn_ptr = m_write_txn; \
  else \
  { \
    if (auto mdb_res = lmdb_txn_begin(m_env, NULL, flags, auto_txn)) \
      throw0(DB_ERROR(lmdb_error(std::string("Failed to create a transaction for the db in ")+__FUNCTION__+": ", mdb_res).c_str())); \
  } \

#define TXN_BLOCK_POSTFIX_SUCCESS() \
  do { \
    if (! m_batch_active && ! m_write_txn) \
      auto_txn.commit(); \
  } while(0)

void BlockchainLMDB::add_txpool_tx(const crypto::hash &txid, const cryptonote::blobdata &blob, const txpool_tx_meta_t &meta)
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();
  mdb_txn_cursors *m_cursors = &m_wcursors;

  CURSOR(txpool_meta)
  CURSOR(txpool_blob)

  MDB_val k = {sizeof(txid), (void *)&txid};
  MDB_val v = {sizeof(meta), (void *)&meta};
  if (auto result = mdb_cursor_put(m_cur_txpool_meta, &k, &v, MDB_NODUPDATA)) {
    if (result == MDB_KEYEXIST)
      throw1(DB_ERROR("Attempting to add txpool tx metadata that's already in the db"));
    else
      throw1(DB_ERROR(lmdb_error("Error adding txpool tx metadata to db transaction: ", result).c_str()));
  }
  MDB_val_sized(blob_val, blob);
  if (auto result = mdb_cursor_put(m_cur_txpool_blob, &k, &blob_val, MDB_NODUPDATA)) {
    if (result == MDB_KEYEXIST)
      throw1(DB_ERROR("Attempting to add txpool tx blob that's already in the db"));
    else
      throw1(DB_ERROR(lmdb_error("Error adding txpool tx blob to db transaction: ", result).c_str()));
  }
}

void BlockchainLMDB::update_txpool_tx(const crypto::hash &txid, const txpool_tx_meta_t &meta)
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();
  mdb_txn_cursors *m_cursors = &m_wcursors;

  CURSOR(txpool_meta)
  CURSOR(txpool_blob)

  MDB_val k = {sizeof(txid), (void *)&txid};
  MDB_val v;
  auto result = mdb_cursor_get(m_cur_txpool_meta, &k, &v, MDB_SET);
  if (result != 0)
    throw1(DB_ERROR(lmdb_error("Error finding txpool tx meta to update: ", result).c_str()));
  result = mdb_cursor_del(m_cur_txpool_meta, 0);
  if (result)
    throw1(DB_ERROR(lmdb_error("Error adding removal of txpool tx metadata to db transaction: ", result).c_str()));
  v = MDB_val({sizeof(meta), (void *)&meta});
  if ((result = mdb_cursor_put(m_cur_txpool_meta, &k, &v, MDB_NODUPDATA)) != 0) {
    if (result == MDB_KEYEXIST)
      throw1(DB_ERROR("Attempting to add txpool tx metadata that's already in the db"));
    else
      throw1(DB_ERROR(lmdb_error("Error adding txpool tx metadata to db transaction: ", result).c_str()));
  }
}

uint64_t BlockchainLMDB::get_txpool_tx_count(bool include_unrelayed_txes) const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();

  int result;
  uint64_t num_entries = 0;

  TXN_PREFIX_RDONLY();

  if (include_unrelayed_txes)
  {
    // No filtering, we can get the number of tx the "fast" way
    MDB_stat db_stats;
    if ((result = mdb_stat(m_txn, m_txpool_meta, &db_stats)))
      throw0(DB_ERROR(lmdb_error("Failed to query m_txpool_meta: ", result).c_str()));
    num_entries = db_stats.ms_entries;
  }
  else
  {
    // Filter unrelayed tx out of the result, so we need to loop over transactions and check their meta data
    RCURSOR(txpool_meta);
    RCURSOR(txpool_blob);

    MDB_val k;
    MDB_val v;
    MDB_cursor_op op = MDB_FIRST;
    while (1)
    {
      result = mdb_cursor_get(m_cur_txpool_meta, &k, &v, op);
      op = MDB_NEXT;
      if (result == MDB_NOTFOUND)
        break;
      if (result)
        throw0(DB_ERROR(lmdb_error("Failed to enumerate txpool tx metadata: ", result).c_str()));
      const txpool_tx_meta_t &meta = *(const txpool_tx_meta_t*)v.mv_data;
      if (!meta.do_not_relay)
        ++num_entries;
    }
  }
  TXN_POSTFIX_RDONLY();

  return num_entries;
}

bool BlockchainLMDB::txpool_has_tx(const crypto::hash& txid) const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();

  TXN_PREFIX_RDONLY();
  RCURSOR(txpool_meta)

  MDB_val k = {sizeof(txid), (void *)&txid};
  auto result = mdb_cursor_get(m_cur_txpool_meta, &k, NULL, MDB_SET);
  if (result != 0 && result != MDB_NOTFOUND)
    throw1(DB_ERROR(lmdb_error("Error finding txpool tx meta: ", result).c_str()));
  TXN_POSTFIX_RDONLY();
  return result != MDB_NOTFOUND;
}

void BlockchainLMDB::remove_txpool_tx(const crypto::hash& txid)
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();
  mdb_txn_cursors *m_cursors = &m_wcursors;

  CURSOR(txpool_meta)
  CURSOR(txpool_blob)

  MDB_val k = {sizeof(txid), (void *)&txid};
  auto result = mdb_cursor_get(m_cur_txpool_meta, &k, NULL, MDB_SET);
  if (result != 0 && result != MDB_NOTFOUND)
    throw1(DB_ERROR(lmdb_error("Error finding txpool tx meta to remove: ", result).c_str()));
  if (!result)
  {
    result = mdb_cursor_del(m_cur_txpool_meta, 0);
    if (result)
      throw1(DB_ERROR(lmdb_error("Error adding removal of txpool tx metadata to db transaction: ", result).c_str()));
  }
  result = mdb_cursor_get(m_cur_txpool_blob, &k, NULL, MDB_SET);
  if (result != 0 && result != MDB_NOTFOUND)
    throw1(DB_ERROR(lmdb_error("Error finding txpool tx blob to remove: ", result).c_str()));
  if (!result)
  {
    result = mdb_cursor_del(m_cur_txpool_blob, 0);
    if (result)
      throw1(DB_ERROR(lmdb_error("Error adding removal of txpool tx blob to db transaction: ", result).c_str()));
  }
}

bool BlockchainLMDB::get_txpool_tx_meta(const crypto::hash& txid, txpool_tx_meta_t &meta) const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();

  TXN_PREFIX_RDONLY();
  RCURSOR(txpool_meta)

  MDB_val k = {sizeof(txid), (void *)&txid};
  MDB_val v;
  auto result = mdb_cursor_get(m_cur_txpool_meta, &k, &v, MDB_SET);
  if (result == MDB_NOTFOUND)
      return false;
  if (result != 0)
      throw1(DB_ERROR(lmdb_error("Error finding txpool tx meta: ", result).c_str()));

  meta = *(const txpool_tx_meta_t*)v.mv_data;
  TXN_POSTFIX_RDONLY();
  return true;
}

bool BlockchainLMDB::get_txpool_tx_blob(const crypto::hash& txid, cryptonote::blobdata &bd) const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();

  TXN_PREFIX_RDONLY();
  RCURSOR(txpool_blob)

  MDB_val k = {sizeof(txid), (void *)&txid};
  MDB_val v;
  auto result = mdb_cursor_get(m_cur_txpool_blob, &k, &v, MDB_SET);
  if (result == MDB_NOTFOUND)
    return false;
  if (result != 0)
      throw1(DB_ERROR(lmdb_error("Error finding txpool tx blob: ", result).c_str()));

  bd.assign(reinterpret_cast<const char*>(v.mv_data), v.mv_size);
  TXN_POSTFIX_RDONLY();
  return true;
}

cryptonote::blobdata BlockchainLMDB::get_txpool_tx_blob(const crypto::hash& txid) const
{
  cryptonote::blobdata bd;
  if (!get_txpool_tx_blob(txid, bd))
    throw1(DB_ERROR("Tx not found in txpool: "));
  return bd;
}

bool BlockchainLMDB::for_all_txpool_txes(std::function<bool(const crypto::hash&, const txpool_tx_meta_t&, const cryptonote::blobdata*)> f, bool include_blob, bool include_unrelayed_txes) const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();

  TXN_PREFIX_RDONLY();
  RCURSOR(txpool_meta);
  RCURSOR(txpool_blob);

  MDB_val k;
  MDB_val v;
  bool ret = true;

  MDB_cursor_op op = MDB_FIRST;
  while (1)
  {
    int result = mdb_cursor_get(m_cur_txpool_meta, &k, &v, op);
    op = MDB_NEXT;
    if (result == MDB_NOTFOUND)
      break;
    if (result)
      throw0(DB_ERROR(lmdb_error("Failed to enumerate txpool tx metadata: ", result).c_str()));
    const crypto::hash txid = *(const crypto::hash*)k.mv_data;
    const txpool_tx_meta_t &meta = *(const txpool_tx_meta_t*)v.mv_data;
    if (!include_unrelayed_txes && meta.do_not_relay)
      // Skipping that tx
      continue;
    const cryptonote::blobdata *passed_bd = NULL;
    cryptonote::blobdata bd;
    if (include_blob)
    {
      MDB_val b;
      result = mdb_cursor_get(m_cur_txpool_blob, &k, &b, MDB_SET);
      if (result == MDB_NOTFOUND)
        throw0(DB_ERROR("Failed to find txpool tx blob to match metadata"));
      if (result)
        throw0(DB_ERROR(lmdb_error("Failed to enumerate txpool tx blob: ", result).c_str()));
      bd.assign(reinterpret_cast<const char*>(b.mv_data), b.mv_size);
      passed_bd = &bd;
    }

    if (!f(txid, meta, passed_bd)) {
      ret = false;
      break;
    }
  }

  TXN_POSTFIX_RDONLY();

  return ret;
}

bool BlockchainLMDB::for_all_alt_blocks(std::function<bool(const crypto::hash&, const alt_block_data_t&, const cryptonote::blobdata*)> f, bool include_blob) const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();

  TXN_PREFIX_RDONLY();
  RCURSOR(alt_blocks);

  MDB_val k;
  MDB_val v;
  bool ret = true;

  MDB_cursor_op op = MDB_FIRST;
  while (1)
  {
    int result = mdb_cursor_get(m_cur_alt_blocks, &k, &v, op);
    op = MDB_NEXT;
    if (result == MDB_NOTFOUND)
      break;
    if (result)
      throw0(DB_ERROR(lmdb_error("Failed to enumerate alt blocks: ", result).c_str()));
    const crypto::hash &blkid = *(const crypto::hash*)k.mv_data;
    if (v.mv_size < sizeof(alt_block_data_t))
      throw0(DB_ERROR("alt_blocks record is too small"));
    const alt_block_data_t *data = (const alt_block_data_t*)v.mv_data;
    const cryptonote::blobdata *passed_bd = NULL;
    cryptonote::blobdata bd;
    if (include_blob)
    {
      bd.assign(reinterpret_cast<const char*>(v.mv_data) + sizeof(alt_block_data_t), v.mv_size - sizeof(alt_block_data_t));
      passed_bd = &bd;
    }

    if (!f(blkid, *data, passed_bd)) {
      ret = false;
      break;
    }
  }

  TXN_POSTFIX_RDONLY();

  return ret;
}

bool BlockchainLMDB::block_exists(const crypto::hash& h, uint64_t *height) const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();

  TXN_PREFIX_RDONLY();
  RCURSOR(block_heights);

  bool ret = false;
  MDB_val_set(key, h);
  auto get_result = mdb_cursor_get(m_cur_block_heights, (MDB_val *)&zerokval, &key, MDB_GET_BOTH);
  if (get_result == MDB_NOTFOUND)
  {
    LOG_PRINT_L3("Block with hash " << epee::string_tools::pod_to_hex(h) << " not found in db");
  }
  else if (get_result)
    throw0(DB_ERROR(lmdb_error("DB error attempting to fetch block index from hash", get_result).c_str()));
  else
  {
    if (height)
    {
      const blk_height *bhp = (const blk_height *)key.mv_data;
      *height = bhp->bh_height;
    }
    ret = true;
  }

  TXN_POSTFIX_RDONLY();
  return ret;
}

cryptonote::blobdata BlockchainLMDB::get_block_blob(const crypto::hash& h) const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();

  return get_block_blob_from_height(get_block_height(h));
}

uint64_t BlockchainLMDB::get_block_height(const crypto::hash& h) const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();

  TXN_PREFIX_RDONLY();
  RCURSOR(block_heights);

  MDB_val_set(key, h);
  auto get_result = mdb_cursor_get(m_cur_block_heights, (MDB_val *)&zerokval, &key, MDB_GET_BOTH);
  if (get_result == MDB_NOTFOUND)
    throw1(BLOCK_DNE("Attempted to retrieve non-existent block height"));
  else if (get_result)
    throw0(DB_ERROR("Error attempting to retrieve a block height from the db"));

  blk_height *bhp = (blk_height *)key.mv_data;
  uint64_t ret = bhp->bh_height;
  TXN_POSTFIX_RDONLY();
  return ret;
}

block_header BlockchainLMDB::get_block_header(const crypto::hash& h) const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();

  // block_header object is automatically cast from block object
  return get_block(h);
}

void check_error(int err)
{
  if (err)
    fprintf(stderr, "get_v3_data failed, error %d %s\n", err, mdb_strerror(err));
}

void BlockchainLMDB::build_block_cache(uint64_t height)
{
  if (m_block_cache_height.load(std::memory_order_acquire) >= height)
    return;

  CRITICAL_REGION_BEGIN(m_block_cache_lock);

  m_block_cache.reserve(height);

  MDB_txn *txn;
  MDB_cursor *cur;
  if (auto r = lmdb_txn_begin(m_env, NULL, MDB_RDONLY, &txn))
    throw0(DB_ERROR(lmdb_error("Failed to create a transaction for the db: ", r).c_str()));

  int err = mdb_cursor_open(txn, m_block_info, &cur); check_error(err);

  mdb_block_info *bi;

  for(uint64_t index = m_block_cache.size(); index < height; ++index)
  {
    MDB_val_set(query, index);
    err = mdb_cursor_get(cur, (MDB_val*)&zerokval, &query, MDB_GET_BOTH); check_error(err);
    bi = (mdb_block_info*)query.mv_data;
    m_block_cache.push_back(*bi);
  }

  mdb_cursor_close(cur);
  mdb_txn_abort(txn);

  CRITICAL_REGION_END();

  m_block_cache_height.store(height, std::memory_order_release);
}

void BlockchainLMDB::get_cna_v2_data(cn_random_values_t *rv, uint64_t height, uint32_t scratchpad_size)
{
  crypto::hash h0 = get_block_hash_from_height(height);
  crypto::hash h1 = get_block_hash_from_height(height - (uint64_t)((uint8_t)h0.data[0] ^ (uint8_t)h0.data[16]));
  crypto::hash h2 = get_block_hash_from_height(height - (uint64_t)((uint8_t)h0.data[4] ^ (uint8_t)h0.data[20]));
  crypto::hash h3 = get_block_hash_from_height(height - (uint64_t)((uint8_t)h0.data[8] ^ (uint8_t)h0.data[24]));
  crypto::hash h4 = get_block_hash_from_height(height - (uint64_t)((uint8_t)h0.data[12] ^ (uint8_t)h0.data[28]));

  uint8_t buffer[16];
  for (size_t i = 0; i < 32; i += 4)
  {
    std::memcpy(buffer, h1.data + i, 4);
    std::memcpy(buffer + 4, h2.data + i, 4);
    std::memcpy(buffer + 8, h3.data + i, 4);
    std::memcpy(buffer + 12, h4.data + i, 4);

    rv->operators[i] = (buffer[1] ^ buffer[3]) >> 5;
    rv->values[i] = buffer[5] ^ buffer[7];
    rv->indices[i] = (
      buffer[0] << 24 |
      buffer[2] << 16 |
      buffer[4] << 8 |
      buffer[6]) % scratchpad_size;

    rv->operators[i+1] = (buffer[0] ^ buffer[2]) >> 5;
    rv->values[i+1] = buffer[4] ^ buffer[6];
    rv->indices[i+1] = (
      buffer[1] << 24 |
      buffer[3] << 16 |
      buffer[5] << 8 |
      buffer[7]) % scratchpad_size;

    rv->operators[i+2] = (buffer[9] ^ buffer[11]) >> 5;
    rv->values[i+2] = buffer[13] ^ buffer[15];
    rv->indices[i+2] = (
      buffer[8] << 24 |
      buffer[10] << 16 |
      buffer[12] << 8 |
      buffer[14]) % scratchpad_size;

    rv->operators[i+3] = (buffer[8] ^ buffer[10]) >> 5;
    rv->values[i+3] = buffer[12] ^ buffer[14];
    rv->indices[i+3] = (
      buffer[9] << 24 |
      buffer[11] << 16 |
      buffer[13] << 8 |
      buffer[15]) % scratchpad_size;
  }
}

void BlockchainLMDB::get_cna_v3_data(char *salt, uint64_t height, uint32_t seed)
{
  MDB_txn *txn;
  MDB_cursor *cur;
  angrywasp::mersenne_twister mt(seed);
  char *bd = (char*)malloc(128);
  char *blob_data = bd;
  
  int err = 0;
  uint32_t t = 0;
  uint64_t r = 0, x = 0, y = 0, z = 0, w = 0;
  uint8_t a = 32, b = 64;

  mdb_block_info *bi;

  if (auto r = lmdb_txn_begin(m_env, NULL, MDB_RDONLY, &txn))
      throw0(DB_ERROR(lmdb_error("Failed to create a transaction for the db: ", r).c_str()));

  err = mdb_cursor_open(txn, m_block_info, &cur); check_error(err);

  for (uint32_t i = 0; i < 32; i++)
  {
    //block hash
    r = mt.next(1, (uint32_t)(height - 1));
    MDB_val_set(rh1, r);
    err = mdb_cursor_get(cur, (MDB_val*)&zerokval, &rh1, MDB_GET_BOTH); check_error(err);
    bi = (mdb_block_info*)rh1.mv_data;
    std::memcpy(blob_data, bi->bi_hash.data, 32);

    //random diff/timestamps
    //random coin count hi/lo
    a = 32;
    b = 64;
    for (uint32_t j = 0; j < 4; j++)
    {
      r = mt.next(6, (uint32_t)(height - 6));
      x = mt.next(r - 5, r + 5);
      y = mt.next(r - 5, r + 5);

      r = mt.next(6, (uint32_t)(height - 6));
      z = mt.next(r - 5, r + 5);
      w = mt.next(r - 5, r + 5);

      MDB_val_set(rx, x);
      err = mdb_cursor_get(cur, (MDB_val*)&zerokval, &rx, MDB_GET_BOTH); check_error(err);
      bi = (mdb_block_info*)rx.mv_data;
      t = (uint32_t)bi->bi_timestamp;
      std::memcpy(blob_data + a, &t, 4);
      a += 4;

      MDB_val_set(ry, y);
      err = mdb_cursor_get(cur, (MDB_val*)&zerokval, &ry, MDB_GET_BOTH); check_error(err);
      bi = (mdb_block_info*)ry.mv_data;
      t = (uint32_t)bi->bi_diff;
      std::memcpy(blob_data + a, &t, 4);
      a += 4;

      MDB_val_set(rz, z);
      err = mdb_cursor_get(cur, (MDB_val*)&zerokval, &rz, MDB_GET_BOTH); check_error(err);
      bi = (mdb_block_info*)rz.mv_data;
      t = (uint32_t)(bi->bi_coins >> 32U);
      std::memcpy(blob_data + b, &t, 4);
      b += 4;

      MDB_val_set(rw, w);
      err = mdb_cursor_get(cur, (MDB_val*)&zerokval, &rw, MDB_GET_BOTH); check_error(err);
      bi = (mdb_block_info*)rw.mv_data;
      t = (uint32_t)bi->bi_coins;
      std::memcpy(blob_data + b, &t, 4);
      b += 4;
    }

    //block hash
    r = mt.next(1, (uint32_t)(height - 1));
    MDB_val_set(rh4, r);
    err = mdb_cursor_get(cur, (MDB_val*)&zerokval, &rh4, MDB_GET_BOTH); check_error(err);
    bi = (mdb_block_info*)rh4.mv_data;
    std::memcpy(blob_data + 96, bi->bi_hash.data, 32);

    std::memcpy(salt + (i * 128), blob_data, 128);
  }

  free(bd);
  mdb_cursor_close(cur);
  mdb_txn_abort(txn); 
}

void BlockchainLMDB::get_cna_v4_data(char *salt, uint64_t height, uint32_t seed)
{
  angrywasp::mersenne_twister mt(seed);
  
  int err = 0;
  uint32_t t = 0;
  uint64_t r = 0, x = 0, y = 0, z = 0, w = 0;
  uint8_t a = 32, b = 64;

  build_block_cache(height);
  std::array<uint32_t, 36864> rand_seq = mt.generate_v4_sequence(seed, (uint32_t)height);
  uint32_t i_config = 0;
  mdb_block_info *bi;

  for (uint32_t i = 0; i < 2048; i++)
  {
    r = rand_seq[i_config++];
    bi = &m_block_cache[r];
    std::memcpy(salt + (i * 128), bi->bi_hash.data, 32);

    a = 32;
    b = 64;
    for (uint32_t j = 0; j < 4; j++)
    {
      x = rand_seq[i_config++];
      y = rand_seq[i_config++];
      z = rand_seq[i_config++];
      w = rand_seq[i_config++];

      bi = &m_block_cache[x];
      t = (uint32_t)bi->bi_timestamp;
      std::memcpy(salt + (i * 128) + a, &t, 4);
      a += 4;

      bi = &m_block_cache[y];
      t = (uint32_t)bi->bi_diff;
      std::memcpy(salt + (i * 128) + a, &t, 4);
      a += 4;

      bi = &m_block_cache[z];
      t = (uint32_t)(bi->bi_coins >> 32U);
      std::memcpy(salt + (i * 128) + b, &t, 4);
      b += 4;

      bi = &m_block_cache[w];
      t = (uint32_t)bi->bi_coins;
      std::memcpy(salt + (i * 128) + b, &t, 4);
      b += 4;
    }

    r = rand_seq[i_config++];
    bi = &m_block_cache[r];
    std::memcpy(salt + (i * 128) + 96, bi->bi_hash.data, 32);
  }
}

void BlockchainLMDB::get_cna_v5_data(char *out, HC128_State *rng_state, uint64_t height)
{
  // TODO: Do not assume little-endian architecture
  build_block_cache(height);
  mdb_block_info *bi;
  size_t rng_key_idx = 0;
  unsigned char msg[64];
  size_t msgpos;
  unsigned char *optr = (unsigned char*)out;
  uint64_t count = 0;
  while (count < 2048)
  {
    HC128_NextKeys(rng_state);
 
    for (size_t k = 0; k < 16; k++) {
        bi = &m_block_cache[HC128_U32(rng_state, &rng_key_idx, height)];
        std::memcpy(msg, bi->bi_hash.data, sizeof(crypto::hash));
        msgpos = sizeof(crypto::hash);
 
        bi = &m_block_cache[HC128_U32(rng_state, &rng_key_idx, height)];
        std::memcpy(msg + msgpos, &(bi->bi_timestamp), sizeof(uint64_t));
        msgpos += sizeof(uint64_t);
 
        bi = &m_block_cache[HC128_U32(rng_state, &rng_key_idx, height)];
        std::memcpy(msg + msgpos, &(bi->bi_diff), sizeof(uint64_t));
        msgpos += sizeof(uint64_t);
 
        bi = &m_block_cache[HC128_U32(rng_state, &rng_key_idx, height)];
        std::memcpy(msg + msgpos, &(bi->bi_coins), sizeof(uint64_t));
        msgpos += sizeof(uint64_t);
 
        std::memcpy(msg + msgpos, &count, sizeof(uint64_t));
 
        HC128_EncryptMessage(rng_state, msg, optr, sizeof(msg));
        optr += 16 * sizeof(uint32_t);

        count++;
    }
 
    // Reseed, but don't reset the RNG key index, making the next used key
    // effectively random at the start of each loop iteration (except the first)
    unsigned char *iv = optr - (8 * 16 * sizeof(uint32_t)) + HC128_U32(rng_state, &rng_key_idx, (8 * 16 * sizeof(uint32_t)) - 16);
    unsigned char *key = optr - (16 * 16 * sizeof(uint32_t)) + HC128_U32(rng_state, &rng_key_idx, (8 * 16 * sizeof(uint32_t)) - 16);
    HC128_Init(rng_state, key, iv);
  }
 
  std::memcpy(msg, optr - 131072 + HC128_U32(rng_state, &rng_key_idx, 131072U - 16U), 16);
  std::memcpy(msg, optr - 131072 + HC128_U32(rng_state, &rng_key_idx, 131072U - 16U), 16);
  std::memcpy(msg, optr - 131072 + HC128_U32(rng_state, &rng_key_idx, 131072U - 16U), 16);
  std::memcpy(msg, optr - 131072 + HC128_U32(rng_state, &rng_key_idx, 131072U - 16U), 16);
  HC128_EncryptMessage(rng_state, msg, optr, sizeof(msg));
  HC128_Init(rng_state, optr, optr+16);

  while (count < 4096)
  {
    HC128_NextKeys(rng_state);

    for (size_t k = 0; k < 16; k++) {
        bi = &m_block_cache[HC128_U32(rng_state, &rng_key_idx, height)];
        std::memcpy(msg, bi->bi_hash.data, sizeof(crypto::hash));
        msgpos = sizeof(crypto::hash);

        bi = &m_block_cache[HC128_U32(rng_state, &rng_key_idx, height)];
        std::memcpy(msg + msgpos, &(bi->bi_timestamp), sizeof(uint64_t));
        msgpos += sizeof(uint64_t);

        bi = &m_block_cache[HC128_U32(rng_state, &rng_key_idx, height)];
        std::memcpy(msg + msgpos, &(bi->bi_diff), sizeof(uint64_t));
        msgpos += sizeof(uint64_t);

        bi = &m_block_cache[HC128_U32(rng_state, &rng_key_idx, height)];
        std::memcpy(msg + msgpos, &(bi->bi_coins), sizeof(uint64_t));
        msgpos += sizeof(uint64_t);

        std::memcpy(msg + msgpos, &count, sizeof(uint64_t));

        HC128_EncryptMessage(rng_state, msg, optr, sizeof(msg));
        optr += 16 * sizeof(uint32_t);

        count++;
    }

    // Reseed, but don't reset the RNG key index, making the next used key
    // effectively random at the start of each loop iteration (except the first)
    unsigned char *iv = optr - (8 * 16 * sizeof(uint32_t)) + HC128_U32(rng_state, &rng_key_idx, (8 * 16 * sizeof(uint32_t)) - 16);
    unsigned char *key = optr - (16 * 16 * sizeof(uint32_t)) + HC128_U32(rng_state, &rng_key_idx, (8 * 16 * sizeof(uint32_t)) - 16);
    HC128_Init(rng_state, key, iv);
  }
}

cryptonote::blobdata BlockchainLMDB::get_block_blob_from_height(const uint64_t& height) const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();

  TXN_PREFIX_RDONLY();
  RCURSOR(blocks);

  MDB_val_copy<uint64_t> key(height);
  MDB_val result;
  auto get_result = mdb_cursor_get(m_cur_blocks, &key, &result, MDB_SET);
  if (get_result == MDB_NOTFOUND)
  {
    throw0(BLOCK_DNE(std::string("Attempt to get block from height ").append(boost::lexical_cast<std::string>(height)).append(" failed -- block not in db").c_str()));
  }
  else if (get_result)
    throw0(DB_ERROR("Error attempting to retrieve a block from the db"));

  blobdata bd;
  bd.assign(reinterpret_cast<char*>(result.mv_data), result.mv_size);

  TXN_POSTFIX_RDONLY();

  return bd;
}

uint64_t BlockchainLMDB::get_block_timestamp(const uint64_t& height) const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();

  TXN_PREFIX_RDONLY();
  RCURSOR(block_info);

  MDB_val_set(result, height);
  auto get_result = mdb_cursor_get(m_cur_block_info, (MDB_val *)&zerokval, &result, MDB_GET_BOTH);
  if (get_result == MDB_NOTFOUND)
  {
    throw0(BLOCK_DNE(std::string("Attempt to get timestamp from height ").append(boost::lexical_cast<std::string>(height)).append(" failed -- timestamp not in db").c_str()));
  }
  else if (get_result)
    throw0(DB_ERROR("Error attempting to retrieve a timestamp from the db"));

  mdb_block_info *bi = (mdb_block_info *)result.mv_data;
  uint64_t ret = bi->bi_timestamp;
  TXN_POSTFIX_RDONLY();
  return ret;
}

uint64_t BlockchainLMDB::get_top_block_timestamp() const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();
  uint64_t m_height = height();

  // if no blocks, return 0
  if (m_height == 0)
  {
    return 0;
  }

  return get_block_timestamp(m_height - 1);
}

std::vector<uint64_t> BlockchainLMDB::get_block_cumulative_rct_outputs(const std::vector<uint64_t> &heights) const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();
  std::vector<uint64_t> res;
  int result;

  if (heights.empty())
    return {};
  res.reserve(heights.size());

  TXN_PREFIX_RDONLY();
  RCURSOR(block_info);

  MDB_stat db_stats;
  if ((result = mdb_stat(m_txn, m_blocks, &db_stats)))
    throw0(DB_ERROR(lmdb_error("Failed to query m_blocks: ", result).c_str()));
  for (size_t i = 0; i < heights.size(); ++i)
    if (heights[i] >= db_stats.ms_entries)
      throw0(BLOCK_DNE(std::string("Attempt to get rct distribution from height " + std::to_string(heights[i]) + " failed -- block size not in db").c_str()));

  MDB_val v;

  uint64_t prev_height = heights[0];
  uint64_t range_begin = 0, range_end = 0;
  for (uint64_t height: heights)
  {
    if (height >= range_begin && height < range_end)
    {
      // nohting to do
    }
    else
    {
      if (height == prev_height + 1)
      {
        MDB_val k2;
        result = mdb_cursor_get(m_cur_block_info, &k2, &v, MDB_NEXT_MULTIPLE);
        range_begin = ((const mdb_block_info*)v.mv_data)->bi_height;
        range_end = range_begin + v.mv_size / sizeof(mdb_block_info); // whole records please
        if (height < range_begin || height >= range_end)
          throw0(DB_ERROR(("Height " + std::to_string(height) + " not included in multiple record range: " + std::to_string(range_begin) + "-" + std::to_string(range_end)).c_str()));
      }
      else
      {
        v.mv_size = sizeof(uint64_t);
        v.mv_data = (void*)&height;
        result = mdb_cursor_get(m_cur_block_info, (MDB_val *)&zerokval, &v, MDB_GET_BOTH);
        range_begin = height;
        range_end = range_begin + 1;
      }
      if (result)
        throw0(DB_ERROR(lmdb_error("Error attempting to retrieve rct distribution from the db: ", result).c_str()));
    }
    const mdb_block_info *bi = ((const mdb_block_info *)v.mv_data) + (height - range_begin);
    prev_height = height;
  }

  TXN_POSTFIX_RDONLY();
  return res;
}

size_t BlockchainLMDB::get_block_size(const uint64_t& height) const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();

  TXN_PREFIX_RDONLY();
  RCURSOR(block_info);

  MDB_val_set(result, height);
  auto get_result = mdb_cursor_get(m_cur_block_info, (MDB_val *)&zerokval, &result, MDB_GET_BOTH);
  if (get_result == MDB_NOTFOUND)
  {
    throw0(BLOCK_DNE(std::string("Attempt to get block size from height ").append(boost::lexical_cast<std::string>(height)).append(" failed -- block size not in db").c_str()));
  }
  else if (get_result)
    throw0(DB_ERROR("Error attempting to retrieve a block size from the db"));

  mdb_block_info *bi = (mdb_block_info *)result.mv_data;
  size_t ret = bi->bi_size;
  TXN_POSTFIX_RDONLY();
  return ret;
}

difficulty_type BlockchainLMDB::get_block_cumulative_difficulty(const uint64_t& height) const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();

  TXN_PREFIX_RDONLY();
  RCURSOR(block_info);

  MDB_val_set(result, height);
  auto get_result = mdb_cursor_get(m_cur_block_info, (MDB_val *)&zerokval, &result, MDB_GET_BOTH);
  if (get_result == MDB_NOTFOUND)
  {
    throw0(BLOCK_DNE(std::string("Attempt to get block cumulative difficulty from height ").append(boost::lexical_cast<std::string>(height)).append(" failed -- block size not in db").c_str()));
  }
  else if (get_result)
    throw0(DB_ERROR("Error attempting to retrieve a block cumulative difficulty from the db"));

  mdb_block_info *bi = (mdb_block_info *)result.mv_data;
  difficulty_type ret = bi->bi_diff;
  TXN_POSTFIX_RDONLY();
  return ret;
}

difficulty_type BlockchainLMDB::get_block_difficulty(const uint64_t& height) const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();

  difficulty_type diff1 = 0;
  difficulty_type diff2 = 0;

  diff1 = get_block_cumulative_difficulty(height);
  if (height != 0)
  {
    diff2 = get_block_cumulative_difficulty(height - 1);
  }

  return diff1 - diff2;
}

uint64_t BlockchainLMDB::get_block_already_generated_coins(const uint64_t& height) const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();

  TXN_PREFIX_RDONLY();
  RCURSOR(block_info);

  MDB_val_set(result, height);
  auto get_result = mdb_cursor_get(m_cur_block_info, (MDB_val *)&zerokval, &result, MDB_GET_BOTH);
  if (get_result == MDB_NOTFOUND)
  {
    throw0(BLOCK_DNE(std::string("Attempt to get generated coins from height ").append(boost::lexical_cast<std::string>(height)).append(" failed -- block size not in db").c_str()));
  }
  else if (get_result)
    throw0(DB_ERROR("Error attempting to retrieve a total generated coins from the db"));

  mdb_block_info *bi = (mdb_block_info *)result.mv_data;
  uint64_t ret = bi->bi_coins;
  TXN_POSTFIX_RDONLY();
  return ret;
}

crypto::hash BlockchainLMDB::get_block_hash_from_height(const uint64_t& height) const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();

  TXN_PREFIX_RDONLY();
  RCURSOR(block_info);

  MDB_val_set(result, height);
  auto get_result = mdb_cursor_get(m_cur_block_info, (MDB_val *)&zerokval, &result, MDB_GET_BOTH);
  if (get_result == MDB_NOTFOUND)
  {
    throw0(BLOCK_DNE(std::string("Attempt to get hash from height ").append(boost::lexical_cast<std::string>(height)).append(" failed -- hash not in db").c_str()));
  }
  else if (get_result)
    throw0(DB_ERROR(lmdb_error("Error attempting to retrieve a block hash from the db: ", get_result).c_str()));

  mdb_block_info *bi = (mdb_block_info *)result.mv_data;
  crypto::hash ret = bi->bi_hash;
  TXN_POSTFIX_RDONLY();
  return ret;
}

std::vector<block> BlockchainLMDB::get_blocks_range(const uint64_t& h1, const uint64_t& h2) const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();
  std::vector<block> v;

  for (uint64_t height = h1; height <= h2; ++height)
  {
    v.push_back(get_block_from_height(height));
  }

  return v;
}

std::vector<crypto::hash> BlockchainLMDB::get_hashes_range(const uint64_t& h1, const uint64_t& h2) const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();
  std::vector<crypto::hash> v;

  for (uint64_t height = h1; height <= h2; ++height)
  {
    v.push_back(get_block_hash_from_height(height));
  }

  return v;
}

crypto::hash BlockchainLMDB::top_block_hash(uint64_t *block_height) const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();
  uint64_t m_height = height();
  if (block_height)
    *block_height = m_height - 1;
  if (m_height != 0)
  {
    return get_block_hash_from_height(m_height - 1);
  }

  return null_hash;
}

block BlockchainLMDB::get_top_block() const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();
  uint64_t m_height = height();

  if (m_height != 0)
  {
    return get_block_from_height(m_height - 1);
  }

  block b;
  return b;
}

uint64_t BlockchainLMDB::height() const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();
  TXN_PREFIX_RDONLY();
  int result;

  // get current height
  MDB_stat db_stats;
  if ((result = mdb_stat(m_txn, m_blocks, &db_stats)))
    throw0(DB_ERROR(lmdb_error("Failed to query m_blocks: ", result).c_str()));
  return db_stats.ms_entries;
}

uint64_t BlockchainLMDB::num_outputs() const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();
  TXN_PREFIX_RDONLY();
  int result;

  RCURSOR(output_txs)

  uint64_t num = 0;
  MDB_val k, v;
  result = mdb_cursor_get(m_cur_output_txs, &k, &v, MDB_LAST);
  if (result == MDB_NOTFOUND)
    num = 0;
  else if (result == 0)
    num = 1 + ((const outtx*)v.mv_data)->output_id;
  else
    throw0(DB_ERROR(lmdb_error("Failed to query m_output_txs: ", result).c_str()));

  return num;
}

bool BlockchainLMDB::tx_exists(const crypto::hash& h) const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();

  TXN_PREFIX_RDONLY();
  RCURSOR(tx_indices);

  MDB_val_set(key, h);
  bool tx_found = false;

  TIME_MEASURE_START(time1);
  auto get_result = mdb_cursor_get(m_cur_tx_indices, (MDB_val *)&zerokval, &key, MDB_GET_BOTH);
  if (get_result == 0)
    tx_found = true;
  else if (get_result != MDB_NOTFOUND)
    throw0(DB_ERROR(lmdb_error(std::string("DB error attempting to fetch transaction index from hash ") + epee::string_tools::pod_to_hex(h) + ": ", get_result).c_str()));

  TIME_MEASURE_FINISH(time1);
  time_tx_exists += time1;

  TXN_POSTFIX_RDONLY();

  if (! tx_found)
  {
    LOG_PRINT_L1("transaction with hash " << epee::string_tools::pod_to_hex(h) << " not found in db");
    return false;
  }

  return true;
}

bool BlockchainLMDB::tx_exists(const crypto::hash& h, uint64_t& tx_id) const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();

  TXN_PREFIX_RDONLY();
  RCURSOR(tx_indices);

  MDB_val_set(v, h);

  TIME_MEASURE_START(time1);
  auto get_result = mdb_cursor_get(m_cur_tx_indices, (MDB_val *)&zerokval, &v, MDB_GET_BOTH);
  TIME_MEASURE_FINISH(time1);
  time_tx_exists += time1;
  if (!get_result) {
    txindex *tip = (txindex *)v.mv_data;
    tx_id = tip->data.tx_id;
  }

  TXN_POSTFIX_RDONLY();

  bool ret = false;
  if (get_result == MDB_NOTFOUND)
  {
    LOG_PRINT_L1("transaction with hash " << epee::string_tools::pod_to_hex(h) << " not found in db");
  }
  else if (get_result)
    throw0(DB_ERROR(lmdb_error("DB error attempting to fetch transaction from hash", get_result).c_str()));
  else
    ret = true;

  return ret;
}

uint64_t BlockchainLMDB::get_tx_unlock_time(const crypto::hash& h) const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();

  TXN_PREFIX_RDONLY();
  RCURSOR(tx_indices);

  MDB_val_set(v, h);
  auto get_result = mdb_cursor_get(m_cur_tx_indices, (MDB_val *)&zerokval, &v, MDB_GET_BOTH);
  if (get_result == MDB_NOTFOUND)
    throw1(TX_DNE(lmdb_error(std::string("tx data with hash ") + epee::string_tools::pod_to_hex(h) + " not found in db: ", get_result).c_str()));
  else if (get_result)
    throw0(DB_ERROR(lmdb_error("DB error attempting to fetch tx data from hash: ", get_result).c_str()));

  txindex *tip = (txindex *)v.mv_data;
  uint64_t ret = tip->data.unlock_time;
  TXN_POSTFIX_RDONLY();
  return ret;
}

bool BlockchainLMDB::get_tx_blob(const crypto::hash& h, cryptonote::blobdata &bd) const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();

  TXN_PREFIX_RDONLY();
  RCURSOR(tx_indices);
  RCURSOR(txs);

  MDB_val_set(v, h);
  MDB_val result;
  auto get_result = mdb_cursor_get(m_cur_tx_indices, (MDB_val *)&zerokval, &v, MDB_GET_BOTH);
  if (get_result == 0)
  {
    txindex *tip = (txindex *)v.mv_data;
    MDB_val_set(val_tx_id, tip->data.tx_id);
    get_result = mdb_cursor_get(m_cur_txs, &val_tx_id, &result, MDB_SET);
  }
  if (get_result == MDB_NOTFOUND)
    return false;
  else if (get_result)
    throw0(DB_ERROR(lmdb_error("DB error attempting to fetch tx from hash", get_result).c_str()));

  bd.assign(reinterpret_cast<char*>(result.mv_data), result.mv_size);

  TXN_POSTFIX_RDONLY();

  return true;
}

uint64_t BlockchainLMDB::get_tx_count() const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();

  TXN_PREFIX_RDONLY();
  int result;

  MDB_stat db_stats;
  if ((result = mdb_stat(m_txn, m_txs, &db_stats)))
    throw0(DB_ERROR(lmdb_error("Failed to query m_txs: ", result).c_str()));

  TXN_POSTFIX_RDONLY();

  return db_stats.ms_entries;
}

std::vector<transaction> BlockchainLMDB::get_tx_list(const std::vector<crypto::hash>& hlist) const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();
  std::vector<transaction> v;

  for (auto& h : hlist)
  {
    v.push_back(get_tx(h));
  }

  return v;
}

uint64_t BlockchainLMDB::get_tx_block_height(const crypto::hash& h) const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();

  TXN_PREFIX_RDONLY();
  RCURSOR(tx_indices);

  MDB_val_set(v, h);
  auto get_result = mdb_cursor_get(m_cur_tx_indices, (MDB_val *)&zerokval, &v, MDB_GET_BOTH);
  if (get_result == MDB_NOTFOUND)
  {
    throw1(TX_DNE(std::string("tx_data_t with hash ").append(epee::string_tools::pod_to_hex(h)).append(" not found in db").c_str()));
  }
  else if (get_result)
    throw0(DB_ERROR(lmdb_error("DB error attempting to fetch tx height from hash", get_result).c_str()));

  txindex *tip = (txindex *)v.mv_data;
  uint64_t ret = tip->data.block_id;
  TXN_POSTFIX_RDONLY();
  return ret;
}

uint64_t BlockchainLMDB::get_num_outputs(const uint64_t& amount) const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();

  TXN_PREFIX_RDONLY();
  RCURSOR(output_amounts);

  MDB_val_copy<uint64_t> k(amount);
  MDB_val v;
  mdb_size_t num_elems = 0;
  auto result = mdb_cursor_get(m_cur_output_amounts, &k, &v, MDB_SET);
  if (result == MDB_SUCCESS)
  {
    mdb_cursor_count(m_cur_output_amounts, &num_elems);
  }
  else if (result != MDB_NOTFOUND)
    throw0(DB_ERROR("DB error attempting to get number of outputs of an amount"));

  TXN_POSTFIX_RDONLY();

  return num_elems;
}

output_data_t BlockchainLMDB::get_output_key_only(const uint64_t& amount, const uint64_t& index) const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();

  TXN_PREFIX_RDONLY();
  RCURSOR(output_amounts);

  MDB_val_set(k, amount);
  MDB_val_set(v, index);
  auto get_result = mdb_cursor_get(m_cur_output_amounts, &k, &v, MDB_GET_BOTH);
  if (get_result == MDB_NOTFOUND)
    throw1(OUTPUT_DNE(std::string("Attempting to get output pubkey by index, but key does not exist: amount " +
        std::to_string(amount) + ", index " + std::to_string(index)).c_str()));
  else if (get_result)
    throw0(DB_ERROR("Error attempting to retrieve an output pubkey from the db"));

  output_data_t ret;
  if (amount == 0)
  {
    const outkey *okp = (const outkey *)v.mv_data;
    ret = okp->data;
  }
  else
  {
    const pre_rct_outkey *okp = (const pre_rct_outkey *)v.mv_data;
    memcpy(&ret, &okp->data, sizeof(pre_rct_output_data_t));
  }
  TXN_POSTFIX_RDONLY();
  return ret;
}

tx_out_index BlockchainLMDB::get_output_tx_and_index_from_global(const uint64_t& output_id) const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();

  TXN_PREFIX_RDONLY();
  RCURSOR(output_txs);

  MDB_val_set(v, output_id);

  auto get_result = mdb_cursor_get(m_cur_output_txs, (MDB_val *)&zerokval, &v, MDB_GET_BOTH);
  if (get_result == MDB_NOTFOUND)
    throw1(OUTPUT_DNE("output with given index not in db"));
  else if (get_result)
    throw0(DB_ERROR("DB error attempting to fetch output tx hash"));

  outtx *ot = (outtx *)v.mv_data;
  tx_out_index ret = tx_out_index(ot->tx_hash, ot->local_index);

  TXN_POSTFIX_RDONLY();
  return ret;
}

tx_out_index BlockchainLMDB::get_output_tx_and_index(const uint64_t& amount, const uint64_t& index) const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  std::vector < uint64_t > offsets;
  std::vector<tx_out_index> indices;
  offsets.push_back(index);
  get_output_tx_and_index(amount, offsets, indices);
  if (!indices.size())
    throw1(OUTPUT_DNE("Attempting to get an output index by amount and amount index, but amount not found"));

  return indices[0];
}

std::vector<std::vector<uint64_t>> BlockchainLMDB::get_tx_amount_output_indices(uint64_t tx_id, size_t n_txes) const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);

  check_open();

  TXN_PREFIX_RDONLY();
  RCURSOR(tx_outputs);

  MDB_val_set(k_tx_id, tx_id);
  MDB_val v;
  std::vector<std::vector<uint64_t>> amount_output_indices_set;
  amount_output_indices_set.reserve(n_txes);

  MDB_cursor_op op = MDB_SET;
  while (n_txes-- > 0)
  {
    int result = mdb_cursor_get(m_cur_tx_outputs, &k_tx_id, &v, op);
    if (result == MDB_NOTFOUND)
      LOG_PRINT_L0("WARNING: Unexpected: tx has no amount indices stored in "
          "tx_outputs, but it should have an empty entry even if it's a tx without "
          "outputs");
    else if (result)
      throw0(DB_ERROR(lmdb_error("DB error attempting to get data for tx_outputs[tx_index]", result).c_str()));

    op = MDB_NEXT;

    const uint64_t* indices = (const uint64_t*)v.mv_data;
    size_t num_outputs = v.mv_size / sizeof(uint64_t);

    amount_output_indices_set.resize(amount_output_indices_set.size() + 1);
    std::vector<uint64_t> &amount_output_indices = amount_output_indices_set.back();
    amount_output_indices.reserve(num_outputs);
    for (size_t i = 0; i < num_outputs; ++i)
    {
      amount_output_indices.push_back(indices[i]);
    }
  }

  TXN_POSTFIX_RDONLY();
  return amount_output_indices_set;
}

bool BlockchainLMDB::has_key_image(const crypto::key_image& img) const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();

  bool ret;

  TXN_PREFIX_RDONLY();
  RCURSOR(spent_keys);

  MDB_val k = {sizeof(img), (void *)&img};
  ret = (mdb_cursor_get(m_cur_spent_keys, (MDB_val *)&zerokval, &k, MDB_GET_BOTH) == 0);

  TXN_POSTFIX_RDONLY();
  return ret;
}

bool BlockchainLMDB::for_all_key_images(std::function<bool(const crypto::key_image&)> f) const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();

  TXN_PREFIX_RDONLY();
  RCURSOR(spent_keys);

  MDB_val k, v;
  bool fret = true;

  k = zerokval;
  MDB_cursor_op op = MDB_FIRST;
  while (1)
  {
    int ret = mdb_cursor_get(m_cur_spent_keys, &k, &v, op);
    op = MDB_NEXT;
    if (ret == MDB_NOTFOUND)
      break;
    if (ret < 0)
      throw0(DB_ERROR("Failed to enumerate key images"));
    const crypto::key_image k_image = *(const crypto::key_image*)v.mv_data;
    if (!f(k_image)) {
      fret = false;
      break;
    }
  }

  TXN_POSTFIX_RDONLY();

  return fret;
}

bool BlockchainLMDB::for_blocks_range(const uint64_t& h1, const uint64_t& h2, std::function<bool(uint64_t, const crypto::hash&, const cryptonote::block&)> f) const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();

  TXN_PREFIX_RDONLY();
  RCURSOR(blocks);

  MDB_val k;
  MDB_val v;
  bool fret = true;

  MDB_cursor_op op;
  if (h1)
  {
    k = MDB_val{sizeof(h1), (void*)&h1};
    op = MDB_SET;
  } else
  {
    op = MDB_FIRST;
  }
  while (1)
  {
    int ret = mdb_cursor_get(m_cur_blocks, &k, &v, op);
    op = MDB_NEXT;
    if (ret == MDB_NOTFOUND)
      break;
    if (ret)
      throw0(DB_ERROR("Failed to enumerate blocks"));
    uint64_t height = *(const uint64_t*)k.mv_data;
    blobdata bd;
    bd.assign(reinterpret_cast<char*>(v.mv_data), v.mv_size);
    block b;
    if (!parse_and_validate_block_from_blob(bd, b))
      throw0(DB_ERROR("Failed to parse block from blob retrieved from the db"));
    crypto::hash hash;
    if (!get_block_hash(b, hash))
        throw0(DB_ERROR("Failed to get block hash from blob retrieved from the db"));
    if (!f(height, hash, b)) {
      fret = false;
      break;
    }
    if (height >= h2)
      break;
  }

  TXN_POSTFIX_RDONLY();

  return fret;
}

bool BlockchainLMDB::for_all_transactions(std::function<bool(const crypto::hash&, const cryptonote::transaction&)> f) const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();

  TXN_PREFIX_RDONLY();
  RCURSOR(txs);
  RCURSOR(tx_indices);

  MDB_val k;
  MDB_val v;
  bool fret = true;

  MDB_cursor_op op = MDB_FIRST;
  while (1)
  {
    int ret = mdb_cursor_get(m_cur_tx_indices, &k, &v, op);
    op = MDB_NEXT;
    if (ret == MDB_NOTFOUND)
      break;
    if (ret)
      throw0(DB_ERROR(lmdb_error("Failed to enumerate transactions: ", ret).c_str()));

    txindex *ti = (txindex *)v.mv_data;
    const crypto::hash hash = ti->key;
    k.mv_data = (void *)&ti->data.tx_id;
    k.mv_size = sizeof(ti->data.tx_id);
    ret = mdb_cursor_get(m_cur_txs, &k, &v, MDB_SET);
    if (ret == MDB_NOTFOUND)
      break;
    if (ret)
      throw0(DB_ERROR(lmdb_error("Failed to enumerate transactions: ", ret).c_str()));
    transaction tx;
    blobdata bd;
    bd.assign(reinterpret_cast<char*>(v.mv_data), v.mv_size);
    if (!parse_and_validate_tx_from_blob(bd, tx))
      throw0(DB_ERROR("Failed to parse tx from blob retrieved from the db"));
    if (!f(hash, tx)) {
      fret = false;
      break;
    }
  }

  TXN_POSTFIX_RDONLY();

  return fret;
}

bool BlockchainLMDB::for_all_outputs(std::function<bool(uint64_t amount, const crypto::hash &tx_hash, uint64_t height, size_t tx_idx)> f) const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();

  TXN_PREFIX_RDONLY();
  RCURSOR(output_amounts);

  MDB_val k;
  MDB_val v;
  bool fret = true;

  MDB_cursor_op op = MDB_FIRST;
  while (1)
  {
    int ret = mdb_cursor_get(m_cur_output_amounts, &k, &v, op);
    op = MDB_NEXT;
    if (ret == MDB_NOTFOUND)
      break;
    if (ret)
      throw0(DB_ERROR("Failed to enumerate outputs"));
    uint64_t amount = *(const uint64_t*)k.mv_data;
    outkey *ok = (outkey *)v.mv_data;
    tx_out_index toi = get_output_tx_and_index_from_global(ok->output_id);
    if (!f(amount, toi.first, ok->data.height, toi.second)) {
      fret = false;
      break;
    }
  }

  TXN_POSTFIX_RDONLY();

  return fret;
}

bool BlockchainLMDB::for_all_outputs(uint64_t amount, const std::function<bool(uint64_t height)> &f) const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();

  TXN_PREFIX_RDONLY();
  RCURSOR(output_amounts);

  MDB_val_set(k, amount);
  MDB_val v;
  bool fret = true;

  MDB_cursor_op op = MDB_SET;
  while (1)
  {
    int ret = mdb_cursor_get(m_cur_output_amounts, &k, &v, op);
    op = MDB_NEXT_DUP;
    if (ret == MDB_NOTFOUND)
      break;
    if (ret)
      throw0(DB_ERROR("Failed to enumerate outputs"));
    uint64_t out_amount = *(const uint64_t*)k.mv_data;
    if (amount != out_amount)
    {
      MERROR("Amount is not the expected amount");
      fret = false;
      break;
    }
    const outkey *ok = (const outkey *)v.mv_data;
    if (!f(ok->data.height)) {
      fret = false;
      break;
    }
  }

  TXN_POSTFIX_RDONLY();

  return fret;
}

// batch_num_blocks: (optional) Used to check if resize needed before batch transaction starts.
bool BlockchainLMDB::batch_start(uint64_t batch_num_blocks, uint64_t batch_bytes)
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  if (! m_batch_transactions)
    throw0(DB_ERROR("batch transactions not enabled"));
  if (m_batch_active)
    return false;
  if (m_write_batch_txn != nullptr)
    return false;
  if (m_write_txn)
    throw0(DB_ERROR("batch transaction attempted, but m_write_txn already in use"));
  check_open();

  m_writer = boost::this_thread::get_id();
  check_and_resize_for_batch(batch_num_blocks, batch_bytes);

  m_write_batch_txn = new mdb_txn_safe();

  // NOTE: need to make sure it's destroyed properly when done
  if (auto mdb_res = lmdb_txn_begin(m_env, NULL, 0, *m_write_batch_txn))
  {
    delete m_write_batch_txn;
    m_write_batch_txn = nullptr;
    throw0(DB_ERROR(lmdb_error("Failed to create a transaction for the db: ", mdb_res).c_str()));
  }
  // indicates this transaction is for batch transactions, but not whether it's
  // active
  m_write_batch_txn->m_batch_txn = true;
  m_write_txn = m_write_batch_txn;

  m_batch_active = true;
  memset(&m_wcursors, 0, sizeof(m_wcursors));
  if (m_tinfo.get())
  {
    if (m_tinfo->m_ti_rflags.m_rf_txn)
      mdb_txn_reset(m_tinfo->m_ti_rtxn);
    memset(&m_tinfo->m_ti_rflags, 0, sizeof(m_tinfo->m_ti_rflags));
  }

  LOG_PRINT_L3("batch transaction: begin");
  return true;
}

void BlockchainLMDB::batch_commit()
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  if (! m_batch_transactions)
    throw0(DB_ERROR("batch transactions not enabled"));
  if (! m_batch_active)
    throw1(DB_ERROR("batch transaction not in progress"));
  if (m_write_batch_txn == nullptr)
    throw1(DB_ERROR("batch transaction not in progress"));
  if (m_writer != boost::this_thread::get_id())
    throw1(DB_ERROR("batch transaction owned by other thread"));

  check_open();

  LOG_PRINT_L3("batch transaction: committing...");
  TIME_MEASURE_START(time1);
  m_write_txn->commit();
  TIME_MEASURE_FINISH(time1);
  time_commit1 += time1;
  LOG_PRINT_L3("batch transaction: committed");

  m_write_txn = nullptr;
  delete m_write_batch_txn;
  m_write_batch_txn = nullptr;
  memset(&m_wcursors, 0, sizeof(m_wcursors));
}

void BlockchainLMDB::cleanup_batch()
{
  // for destruction of batch transaction
  m_write_txn = nullptr;
  delete m_write_batch_txn;
  m_write_batch_txn = nullptr;
  m_batch_active = false;
  memset(&m_wcursors, 0, sizeof(m_wcursors));
}

void BlockchainLMDB::batch_stop()
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  if (! m_batch_transactions)
    throw0(DB_ERROR("batch transactions not enabled"));
  if (! m_batch_active)
    throw1(DB_ERROR("batch transaction not in progress"));
  if (m_write_batch_txn == nullptr)
    throw1(DB_ERROR("batch transaction not in progress"));
  if (m_writer != boost::this_thread::get_id())
    throw1(DB_ERROR("batch transaction owned by other thread"));
  check_open();
  LOG_PRINT_L3("batch transaction: committing...");
  TIME_MEASURE_START(time1);
  try
  {
    m_write_txn->commit();
    TIME_MEASURE_FINISH(time1);
    time_commit1 += time1;
    cleanup_batch();
  }
  catch (const std::exception &e)
  {
    cleanup_batch();
    throw;
  }
  LOG_PRINT_L3("batch transaction: end");
}

void BlockchainLMDB::batch_abort()
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  if (! m_batch_transactions)
    throw0(DB_ERROR("batch transactions not enabled"));
  if (! m_batch_active)
    throw1(DB_ERROR("batch transaction not in progress"));
  if (m_write_batch_txn == nullptr)
    throw1(DB_ERROR("batch transaction not in progress"));
  if (m_writer != boost::this_thread::get_id())
    throw1(DB_ERROR("batch transaction owned by other thread"));
  check_open();
  // for destruction of batch transaction
  m_write_txn = nullptr;
  // explicitly call in case mdb_env_close() (BlockchainLMDB::close()) called before BlockchainLMDB destructor called.
  m_write_batch_txn->abort();
  delete m_write_batch_txn;
  m_write_batch_txn = nullptr;
  m_batch_active = false;
  memset(&m_wcursors, 0, sizeof(m_wcursors));
  LOG_PRINT_L3("batch transaction: aborted");
}

void BlockchainLMDB::set_batch_transactions(bool batch_transactions)
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  if ((batch_transactions) && (m_batch_transactions))
  {
    MINFO("batch transaction mode already enabled, but asked to enable batch mode");
  }
  m_batch_transactions = batch_transactions;
  MINFO("batch transactions " << (m_batch_transactions ? "enabled" : "disabled"));
}

// return true if we started the txn, false if already started
bool BlockchainLMDB::block_rtxn_start(MDB_txn **mtxn, mdb_txn_cursors **mcur) const
{
  bool ret = false;
  mdb_threadinfo *tinfo;
  if (m_write_txn && m_writer == boost::this_thread::get_id()) {
    *mtxn = m_write_txn->m_txn;
    *mcur = (mdb_txn_cursors *)&m_wcursors;
    return ret;
  }
  /* Check for existing info and force reset if env doesn't match -
   * only happens if env was opened/closed multiple times in same process
   */
  if (!(tinfo = m_tinfo.get()) || mdb_txn_env(tinfo->m_ti_rtxn) != m_env)
  {
    tinfo = new mdb_threadinfo;
    m_tinfo.reset(tinfo);
    memset(&tinfo->m_ti_rcursors, 0, sizeof(tinfo->m_ti_rcursors));
    memset(&tinfo->m_ti_rflags, 0, sizeof(tinfo->m_ti_rflags));
    if (auto mdb_res = lmdb_txn_begin(m_env, NULL, MDB_RDONLY, &tinfo->m_ti_rtxn))
      throw0(DB_ERROR_TXN_START(lmdb_error("Failed to create a read transaction for the db: ", mdb_res).c_str()));
    ret = true;
  } else if (!tinfo->m_ti_rflags.m_rf_txn)
  {
    if (auto mdb_res = lmdb_txn_renew(tinfo->m_ti_rtxn))
      throw0(DB_ERROR_TXN_START(lmdb_error("Failed to renew a read transaction for the db: ", mdb_res).c_str()));
    ret = true;
  }
  if (ret)
    tinfo->m_ti_rflags.m_rf_txn = true;
  *mtxn = tinfo->m_ti_rtxn;
  *mcur = &tinfo->m_ti_rcursors;

  if (ret)
    LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  return ret;
}

void BlockchainLMDB::block_rtxn_stop() const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  mdb_txn_reset(m_tinfo->m_ti_rtxn);
  memset(&m_tinfo->m_ti_rflags, 0, sizeof(m_tinfo->m_ti_rflags));
}

bool BlockchainLMDB::block_rtxn_start() const
{
  MDB_txn *mtxn;
  mdb_txn_cursors *mcur;
  return block_rtxn_start(&mtxn, &mcur);
}

void BlockchainLMDB::block_wtxn_start()
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  // Distinguish the exceptions here from exceptions that would be thrown while
  // using the txn and committing it.
  //
  // If an exception is thrown in this setup, we don't want the caller to catch
  // it and proceed as if there were an existing write txn, such as trying to
  // call block_txn_abort(). It also indicates a serious issue which will
  // probably be thrown up another layer.
  if (! m_batch_active && m_write_txn)
    throw0(DB_ERROR_TXN_START((std::string("Attempted to start new write txn when write txn already exists in ")+__FUNCTION__).c_str()));
  if (! m_batch_active)
  {
    m_writer = boost::this_thread::get_id();
    m_write_txn = new mdb_txn_safe();
    if (auto mdb_res = lmdb_txn_begin(m_env, NULL, 0, *m_write_txn))
    {
      delete m_write_txn;
      m_write_txn = nullptr;
      throw0(DB_ERROR_TXN_START(lmdb_error("Failed to create a transaction for the db: ", mdb_res).c_str()));
    }
    memset(&m_wcursors, 0, sizeof(m_wcursors));
    if (m_tinfo.get())
    {
      if (m_tinfo->m_ti_rflags.m_rf_txn)
        mdb_txn_reset(m_tinfo->m_ti_rtxn);
      memset(&m_tinfo->m_ti_rflags, 0, sizeof(m_tinfo->m_ti_rflags));
    }
  } else if (m_writer != boost::this_thread::get_id())
    throw0(DB_ERROR_TXN_START((std::string("Attempted to start new write txn when batch txn already exists in ")+__FUNCTION__).c_str()));
}

void BlockchainLMDB::block_wtxn_stop()
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  if (!m_write_txn)
    throw0(DB_ERROR_TXN_START((std::string("Attempted to stop write txn when no such txn exists in ")+__FUNCTION__).c_str()));
  if (m_writer != boost::this_thread::get_id())
    throw0(DB_ERROR_TXN_START((std::string("Attempted to stop write txn from the wrong thread in ")+__FUNCTION__).c_str()));
  {
    if (! m_batch_active)
	{
      TIME_MEASURE_START(time1);
      m_write_txn->commit();
      TIME_MEASURE_FINISH(time1);
      time_commit1 += time1;

      delete m_write_txn;
      m_write_txn = nullptr;
      memset(&m_wcursors, 0, sizeof(m_wcursors));
	}
  }
}

void BlockchainLMDB::block_wtxn_abort()
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  if (!m_write_txn)
    throw0(DB_ERROR_TXN_START((std::string("Attempted to abort write txn when no such txn exists in ")+__FUNCTION__).c_str()));
  if (m_writer != boost::this_thread::get_id())
    throw0(DB_ERROR_TXN_START((std::string("Attempted to abort write txn from the wrong thread in ")+__FUNCTION__).c_str()));

  if (! m_batch_active)
  {
    delete m_write_txn;
    m_write_txn = nullptr;
    memset(&m_wcursors, 0, sizeof(m_wcursors));
  }
}

void BlockchainLMDB::block_rtxn_abort() const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  mdb_txn_reset(m_tinfo->m_ti_rtxn);
  memset(&m_tinfo->m_ti_rflags, 0, sizeof(m_tinfo->m_ti_rflags));
}

uint64_t BlockchainLMDB::add_block(const std::pair<block, blobdata>& blk, size_t block_size, const difficulty_type& cumulative_difficulty, const uint64_t& coins_generated,
    const std::vector<std::pair<transaction, blobdata>>& txs)
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();
  uint64_t m_height = height();

  if (m_height % 1024 == 0)
  {
    // for batch mode, DB resize check is done at start of batch transaction
    if (! m_batch_active && need_resize())
    {
      LOG_PRINT_L0("LMDB memory map needs to be resized, doing that now.");
      do_resize();
    }
  }

  try
  {
    BlockchainDB::add_block(blk, block_size, cumulative_difficulty, coins_generated, txs);
  }
  catch (const DB_ERROR_TXN_START& e)
  {
    throw;
  }

  return ++m_height;
}

void BlockchainLMDB::pop_block(block& blk, std::vector<transaction>& txs)
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();

  block_wtxn_start();

  try
  {
    BlockchainDB::pop_block(blk, txs);
    block_wtxn_stop();
  }
  catch (...)
  {
    block_wtxn_abort();
    throw;
  }
}

void BlockchainLMDB::get_output_tx_and_index_from_global(const std::vector<uint64_t> &global_indices,
    std::vector<tx_out_index> &tx_out_indices) const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();
  tx_out_indices.clear();
  tx_out_indices.reserve(global_indices.size());

  TXN_PREFIX_RDONLY();
  RCURSOR(output_txs);

  for (const uint64_t &output_id : global_indices)
  {
    MDB_val_set(v, output_id);

    auto get_result = mdb_cursor_get(m_cur_output_txs, (MDB_val *)&zerokval, &v, MDB_GET_BOTH);
    if (get_result == MDB_NOTFOUND)
      throw1(OUTPUT_DNE("output with given index not in db"));
    else if (get_result)
      throw0(DB_ERROR("DB error attempting to fetch output tx hash"));

    const outtx *ot = (const outtx *)v.mv_data;
    tx_out_indices.push_back(tx_out_index(ot->tx_hash, ot->local_index));
  }

  TXN_POSTFIX_RDONLY();
}

void BlockchainLMDB::get_output_key(const epee::span<const uint64_t> &amounts, const std::vector<uint64_t> &offsets, std::vector<output_data_t> &outputs, bool v2, bool allow_partial) const
{
  if (amounts.size() != 1 && amounts.size() != offsets.size())
    throw0(DB_ERROR("Invalid sizes of amounts and offets"));

  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  TIME_MEASURE_START(db3);
  check_open();
  outputs.clear();
  outputs.reserve(offsets.size());

  TXN_PREFIX_RDONLY();

  RCURSOR(output_amounts);

  for (size_t i = 0; i < offsets.size(); ++i)
  {
    const uint64_t amount = amounts.size() == 1 ? amounts[0] : amounts[i];
    MDB_val_set(k, amount);
    MDB_val_set(v, offsets[i]);

    auto get_result = mdb_cursor_get(m_cur_output_amounts, &k, &v, MDB_GET_BOTH);
    if (get_result == MDB_NOTFOUND)
    {
      if (allow_partial)
      {
        MDEBUG("Partial result: " << outputs.size() << "/" << offsets.size());
        break;
      }
      throw1(OUTPUT_DNE((std::string("Attempting to get output pubkey by global index (amount ") + boost::lexical_cast<std::string>(amount) + ", index " + boost::lexical_cast<std::string>(offsets[i]) + ", count " + boost::lexical_cast<std::string>(get_num_outputs(amount)) + "), but key does not exist (current height " + boost::lexical_cast<std::string>(height()) + ")").c_str()));
    }
    else if (get_result)
      throw0(DB_ERROR(lmdb_error("Error attempting to retrieve an output pubkey from the db", get_result).c_str()));

    if (amount == 0)
    {
      const outkey *okp = (const outkey *)v.mv_data;
      outputs.push_back(okp->data);
    }
    else
    {
      const pre_rct_outkey *okp = (const pre_rct_outkey *)v.mv_data;
      outputs.resize(outputs.size() + 1);
      output_data_t &data = outputs.back();
      memcpy(&data, &okp->data, sizeof(pre_rct_output_data_t));
      data.commitment = rct::zeroCommit(amount, v2);
    }
  }

  TXN_POSTFIX_RDONLY();

  TIME_MEASURE_FINISH(db3);
  LOG_PRINT_L3("db3: " << db3);
}

void BlockchainLMDB::get_output_tx_and_index(const uint64_t& amount, const std::vector<uint64_t> &offsets, std::vector<tx_out_index> &indices) const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();
  indices.clear();

  std::vector <uint64_t> tx_indices;
  tx_indices.reserve(offsets.size());
  TXN_PREFIX_RDONLY();

  RCURSOR(output_amounts);

  MDB_val_set(k, amount);
  for (const uint64_t &index : offsets)
  {
    MDB_val_set(v, index);

    auto get_result = mdb_cursor_get(m_cur_output_amounts, &k, &v, MDB_GET_BOTH);
    if (get_result == MDB_NOTFOUND)
      throw1(OUTPUT_DNE("Attempting to get output by index, but key does not exist"));
    else if (get_result)
      throw0(DB_ERROR(lmdb_error("Error attempting to retrieve an output from the db", get_result).c_str()));

    const outkey *okp = (const outkey *)v.mv_data;
    tx_indices.push_back(okp->output_id);
  }

  TIME_MEASURE_START(db3);
  if(tx_indices.size() > 0)
  {
    get_output_tx_and_index_from_global(tx_indices, indices);
  }
  TIME_MEASURE_FINISH(db3);
  LOG_PRINT_L3("db3: " << db3);
}

std::map<uint64_t, std::tuple<uint64_t, uint64_t, uint64_t>> BlockchainLMDB::get_output_histogram(const std::vector<uint64_t> &amounts, bool unlocked, uint64_t recent_cutoff, uint64_t min_count) const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();

  TXN_PREFIX_RDONLY();
  RCURSOR(output_amounts);

  std::map<uint64_t, std::tuple<uint64_t, uint64_t, uint64_t>> histogram;
  MDB_val k;
  MDB_val v;

  if (amounts.empty())
  {
    MDB_cursor_op op = MDB_FIRST;
    while (1)
    {
      int ret = mdb_cursor_get(m_cur_output_amounts, &k, &v, op);
      op = MDB_NEXT_NODUP;
      if (ret == MDB_NOTFOUND)
        break;
      if (ret)
        throw0(DB_ERROR(lmdb_error("Failed to enumerate outputs: ", ret).c_str()));
      mdb_size_t num_elems = 0;
      mdb_cursor_count(m_cur_output_amounts, &num_elems);
      uint64_t amount = *(const uint64_t*)k.mv_data;
      if (num_elems >= min_count)
        histogram[amount] = std::make_tuple(num_elems, 0, 0);
    }
  }
  else
  {
    for (const auto &amount: amounts)
    {
      MDB_val_copy<uint64_t> k(amount);
      int ret = mdb_cursor_get(m_cur_output_amounts, &k, &v, MDB_SET);
      if (ret == MDB_NOTFOUND)
      {
        if (0 >= min_count)
          histogram[amount] = std::make_tuple(0, 0, 0);
      }
      else if (ret == MDB_SUCCESS)
      {
        mdb_size_t num_elems = 0;
        mdb_cursor_count(m_cur_output_amounts, &num_elems);
        if (num_elems >= min_count)
          histogram[amount] = std::make_tuple(num_elems, 0, 0);
      }
      else
      {
        throw0(DB_ERROR(lmdb_error("Failed to enumerate outputs: ", ret).c_str()));
      }
    }
  }

  if (unlocked || recent_cutoff > 0) {
    const uint64_t blockchain_height = height();
    for (std::map<uint64_t, std::tuple<uint64_t, uint64_t, uint64_t>>::iterator i = histogram.begin(); i != histogram.end(); ++i) {
      uint64_t amount = i->first;
      uint64_t num_elems = std::get<0>(i->second);
      while (num_elems > 0) {
        const tx_out_index toi = get_output_tx_and_index(amount, num_elems - 1);
        const uint64_t height = get_tx_block_height(toi.first);
        if (height + CRYPTONOTE_DEFAULT_TX_SPENDABLE_AGE <= blockchain_height)
          break;
        --num_elems;
      }
      // modifying second does not invalidate the iterator
      std::get<1>(i->second) = num_elems;

      if (recent_cutoff > 0)
      {
        uint64_t recent = 0;
        while (num_elems > 0) {
          const tx_out_index toi = get_output_tx_and_index(amount, num_elems - 1);
          const uint64_t height = get_tx_block_height(toi.first);
          const uint64_t ts = get_block_timestamp(height);
          if (ts < recent_cutoff)
            break;
          --num_elems;
          ++recent;
        }
        // modifying second does not invalidate the iterator
        std::get<2>(i->second) = recent;
      }
    }
  }

  TXN_POSTFIX_RDONLY();

  return histogram;
}

bool BlockchainLMDB::get_output_distribution(uint64_t amount, uint64_t from_height, uint64_t to_height, std::vector<uint64_t> &distribution, uint64_t &base) const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();

  TXN_PREFIX_RDONLY();
  RCURSOR(output_amounts);

  distribution.clear();
  const uint64_t db_height = height();
  if (from_height >= db_height)
    return false;
  distribution.resize(db_height - from_height, 0);

  bool fret = true;
  MDB_val_set(k, amount);
  MDB_val v;
  MDB_cursor_op op = MDB_SET;
  base = 0;
  while (1)
  {
    int ret = mdb_cursor_get(m_cur_output_amounts, &k, &v, op);
    op = MDB_NEXT_DUP;
    if (ret == MDB_NOTFOUND)
      break;
    if (ret)
      throw0(DB_ERROR("Failed to enumerate outputs"));
    const outkey *ok = (const outkey *)v.mv_data;
    const uint64_t height = ok->data.height;
    if (height >= from_height)
      distribution[height - from_height]++;
    else
      base++;
    if (to_height > 0 && height > to_height)
      break;
  }

  distribution[0] += base;
  for (size_t n = 1; n < distribution.size(); ++n)
    distribution[n] += distribution[n - 1];
  base = 0;

  TXN_POSTFIX_RDONLY();

  return true;
}

void BlockchainLMDB::check_hard_fork_info()
{
}

void BlockchainLMDB::drop_hard_fork_info()
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();

  TXN_PREFIX(0);

  auto result = mdb_drop(*txn_ptr, m_hf_starting_heights, 1);
  if (result)
    throw1(DB_ERROR(lmdb_error("Error dropping hard fork starting heights db: ", result).c_str()));
  result = mdb_drop(*txn_ptr, m_hf_versions, 1);
  if (result)
    throw1(DB_ERROR(lmdb_error("Error dropping hard fork versions db: ", result).c_str()));

  TXN_POSTFIX_SUCCESS();
}

void BlockchainLMDB::set_hard_fork_version(uint64_t height, uint8_t version)
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();

  TXN_BLOCK_PREFIX(0);

  MDB_val_copy<uint64_t> val_key(height);
  MDB_val_copy<uint8_t> val_value(version);
  int result;
  result = mdb_put(*txn_ptr, m_hf_versions, &val_key, &val_value, MDB_APPEND);
  if (result == MDB_KEYEXIST)
    result = mdb_put(*txn_ptr, m_hf_versions, &val_key, &val_value, 0);
  if (result)
    throw1(DB_ERROR(lmdb_error("Error adding hard fork version to db transaction: ", result).c_str()));

  TXN_BLOCK_POSTFIX_SUCCESS();
}

uint8_t BlockchainLMDB::get_hard_fork_version(uint64_t height) const
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();

  TXN_PREFIX_RDONLY();
  RCURSOR(hf_versions);

  MDB_val_copy<uint64_t> val_key(height);
  MDB_val val_ret;
  auto result = mdb_cursor_get(m_cur_hf_versions, &val_key, &val_ret, MDB_SET);
  if (result == MDB_NOTFOUND || result)
    throw0(DB_ERROR(lmdb_error("Error attempting to retrieve a hard fork version at height " + boost::lexical_cast<std::string>(height) + " from the db: ", result).c_str()));

  uint8_t ret = *(const uint8_t*)val_ret.mv_data;
  TXN_POSTFIX_RDONLY();
  return ret;
}

void BlockchainLMDB::set_expected_min_height(uint64_t height)
{
  const size_t size = static_cast<size_t>(height);
  if (m_block_cache.capacity() < size)
    m_block_cache.reserve(size);
}

void BlockchainLMDB::add_alt_block(const crypto::hash &blkid, const cryptonote::alt_block_data_t &data, const cryptonote::blobdata &blob)
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();
  mdb_txn_cursors *m_cursors = &m_wcursors;

  CURSOR(alt_blocks)

  MDB_val k = {sizeof(blkid), (void *)&blkid};
  const size_t val_size = sizeof(alt_block_data_t) + blob.size();
  std::unique_ptr<char[]> val(new char[val_size]);
  memcpy(val.get(), &data, sizeof(alt_block_data_t));
  memcpy(val.get() + sizeof(alt_block_data_t), blob.data(), blob.size());
  MDB_val v = {val_size, (void *)val.get()};
  if (auto result = mdb_cursor_put(m_cur_alt_blocks, &k, &v, MDB_NODUPDATA)) {
    if (result == MDB_KEYEXIST)
      throw1(DB_ERROR("Attempting to add alternate block that's already in the db"));
    else
      throw1(DB_ERROR(lmdb_error("Error adding alternate block to db transaction: ", result).c_str()));
  }
}

bool BlockchainLMDB::get_alt_block(const crypto::hash &blkid, alt_block_data_t *data, cryptonote::blobdata *blob)
{
  LOG_PRINT_L3("BlockchainLMDB:: " << __func__);
  check_open();

  TXN_PREFIX_RDONLY();
  RCURSOR(alt_blocks);

  MDB_val_set(k, blkid);
  MDB_val v;
  int result = mdb_cursor_get(m_cur_alt_blocks, &k, &v, MDB_SET);
  if (result == MDB_NOTFOUND)
    return false;

  if (result)
    throw0(DB_ERROR(lmdb_error("Error attempting to retrieve alternate block " + epee::string_tools::pod_to_hex(blkid) + " from the db: ", result).c_str()));
  if (v.mv_size < sizeof(alt_block_data_t))
    throw0(DB_ERROR("Record size is less than expected"));

  const alt_block_data_t *ptr = (const alt_block_data_t*)v.mv_data;
  if (data)
    *data = *ptr;
  if (blob)
    blob->assign((const char*)(ptr + 1), v.mv_size - sizeof(alt_block_data_t));

  TXN_POSTFIX_RDONLY();
  return true;
}

void BlockchainLMDB::remove_alt_block(const crypto::hash &blkid)
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();
  mdb_txn_cursors *m_cursors = &m_wcursors;

  CURSOR(alt_blocks)

  MDB_val k = {sizeof(blkid), (void *)&blkid};
  MDB_val v;
  int result = mdb_cursor_get(m_cur_alt_blocks, &k, &v, MDB_SET);
  if (result)
    throw0(DB_ERROR(lmdb_error("Error locating alternate block " + epee::string_tools::pod_to_hex(blkid) + " in the db: ", result).c_str()));
  result = mdb_cursor_del(m_cur_alt_blocks, 0);
  if (result)
    throw0(DB_ERROR(lmdb_error("Error deleting alternate block " + epee::string_tools::pod_to_hex(blkid) + " from the db: ", result).c_str()));
}

uint64_t BlockchainLMDB::get_alt_block_count()
{
  LOG_PRINT_L3("BlockchainLMDB:: " << __func__);
  check_open();

  TXN_PREFIX_RDONLY();
  RCURSOR(alt_blocks);

  MDB_stat db_stats;
  int result = mdb_stat(m_txn, m_alt_blocks, &db_stats);
  uint64_t count = 0;
  if (result != MDB_NOTFOUND)
  {
    if (result)
      throw0(DB_ERROR(lmdb_error("Failed to query m_alt_blocks: ", result).c_str()));
    count = db_stats.ms_entries;
  }
  TXN_POSTFIX_RDONLY();
  return count;
}

void BlockchainLMDB::drop_alt_blocks()
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  check_open();

  TXN_PREFIX(0);

  auto result = mdb_drop(*txn_ptr, m_alt_blocks, 0);
  if (result)
    throw1(DB_ERROR(lmdb_error("Error dropping alternative blocks: ", result).c_str()));

  TXN_POSTFIX_SUCCESS();
}

bool BlockchainLMDB::is_read_only() const
{
  unsigned int flags;
  auto result = mdb_env_get_flags(m_env, &flags);
  if (result)
    throw0(DB_ERROR(lmdb_error("Error getting database environment info: ", result).c_str()));

  if (flags & MDB_RDONLY)
    return true;

  return false;
}

uint64_t BlockchainLMDB::get_database_size() const
{
  uint64_t size = 0;
  boost::filesystem::path datafile(m_folder);
  datafile /= CRYPTONOTE_BLOCKCHAINDATA_FILENAME;
  if (!epee::file_io_utils::get_file_size(datafile.string(), size))
    size = 0;
  return size;
}

void BlockchainLMDB::fixup()
{
  LOG_PRINT_L3("BlockchainLMDB::" << __func__);
  // Always call parent as well
  BlockchainDB::fixup();
}

#define RENAME_DB(name) do { \
    char n2[] = name; \
    MDB_dbi tdbi; \
    n2[sizeof(n2)-2]--; \
    /* play some games to put (name) on a writable page */ \
    result = mdb_dbi_open(txn, n2, MDB_CREATE, &tdbi); \
    if (result) \
      throw0(DB_ERROR(lmdb_error("Failed to create " + std::string(n2) + ": ", result).c_str())); \
    result = mdb_drop(txn, tdbi, 1); \
    if (result) \
      throw0(DB_ERROR(lmdb_error("Failed to delete " + std::string(n2) + ": ", result).c_str())); \
    k.mv_data = (void *)name; \
    k.mv_size = sizeof(name)-1; \
    result = mdb_cursor_open(txn, 1, &c_cur); \
    if (result) \
      throw0(DB_ERROR(lmdb_error("Failed to open a cursor for " name ": ", result).c_str())); \
    result = mdb_cursor_get(c_cur, &k, NULL, MDB_SET_KEY); \
    if (result) \
      throw0(DB_ERROR(lmdb_error("Failed to get DB record for " name ": ", result).c_str())); \
    ptr = (char *)k.mv_data; \
    ptr[sizeof(name)-2]++; } while(0)

#define LOGIF(y)    if (ELPP->vRegistry()->allowed(y, "global"))

void BlockchainLMDB::migrate(const uint32_t oldversion)
{
}

}  // namespace cryptonote
