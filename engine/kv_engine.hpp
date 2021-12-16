/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2021 Intel Corporation
 */

#pragma once

#include <cassert>
#include <cstdint>
#include <ctime>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <list>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

#include "data_record.hpp"
#include "dram_allocator.hpp"
#include "hash_table.hpp"
#include "kvdk/engine.hpp"
#include "logger.hpp"
#include "mvcc.hpp"
#include "pmem_allocator/pmem_allocator.hpp"
#include "queue.hpp"
#include "skiplist.hpp"
#include "structures.hpp"
#include "thread_manager.hpp"
#include "unordered_collection.hpp"
#include "utils.hpp"

namespace KVDK_NAMESPACE {
class KVEngine : public Engine {
  friend class SortedCollectionRebuilder;

public:
  KVEngine(const Configs &configs)
      : thread_cache_(configs.max_write_threads),
        version_controller_(configs.max_write_threads){};
  ~KVEngine();

  static Status Open(const std::string &name, Engine **engine_ptr,
                     const Configs &configs);

  Snapshot *GetSnapshot() override {
    return version_controller_.MakeSnapshot();
  }

  void ReleaseSnapshot(const Snapshot *snapshot) override {
    version_controller_.ReleaseSnapshot(
        static_cast<const SnapshotImpl *>(snapshot));
  }

  // Global Anonymous Collection
  Status Get(const StringView key, std::string *value) override;
  Status Set(const StringView key, const StringView value) override;
  Status Delete(const StringView key) override;
  Status BatchWrite(const WriteBatch &write_batch) override;

  // Sorted Collection
  Status SGet(const StringView collection, const StringView user_key,
              std::string *value) override;
  Status SSet(const StringView collection, const StringView user_key,
              const StringView value) override;
  // TODO: Release delete record and deleted nodes
  Status SDelete(const StringView collection,
                 const StringView user_key) override;
  std::shared_ptr<Iterator>
  NewSortedIterator(const StringView collection) override;

  // Unordered Collection
  virtual Status HGet(StringView const collection_name, StringView const key,
                      std::string *value) override;
  virtual Status HSet(StringView const collection_name, StringView const key,
                      StringView const value) override;
  virtual Status HDelete(StringView const collection_name,
                         StringView const key) override;
  std::shared_ptr<Iterator>
  NewUnorderedIterator(StringView const collection_name) override;

  // Queue
  virtual Status LPop(StringView const collection_name,
                      std::string *value) override {
    return xPop(collection_name, value, QueueOpPosition::Left);
  }

  virtual Status RPop(StringView const collection_name,
                      std::string *value) override {
    return xPop(collection_name, value, QueueOpPosition::Right);
  }

  virtual Status LPush(StringView const collection_name,
                       StringView const value) override {
    return xPush(collection_name, value, QueueOpPosition::Left);
  }

  virtual Status RPush(StringView const collection_name,
                       StringView const value) override {
    return xPush(collection_name, value, QueueOpPosition::Right);
  }

  void ReleaseWriteThread() override { write_thread.Release(); }

  const std::vector<std::shared_ptr<Skiplist>> &GetSkiplists() {
    return skiplists_;
  };

private:
  struct BatchWriteHint {
    TimeStampType timestamp{0};
    SizedSpaceEntry allocated_space{};
    HashTable::KeyHashHint hash_hint{};
    void *pmem_record_to_free = nullptr;
  };

  struct PendingFreeDataRecord {
    void *pmem_data_record;
    TimeStampType newer_version_timestamp;
  };

  struct PendingFreeDeleteRecord {
    void *pmem_delete_record;
    TimeStampType newer_version_timestamp;
    // We need ref to hash entry for clear index of delete record
    HashEntry *hash_entry_ref;
    SpinMutex *hash_entry_lock;
  };

  struct ThreadCache {
    ThreadCache() = default;

    uint64_t newest_restored_ts = 0;
    std::unordered_map<uint64_t, int> visited_skiplist_ids;

    PendingBatch *persisted_pending_batch = nullptr;

    SnapshotImpl holding_snapshot{kMaxTimestamp};

    std::deque<PendingFreeDeleteRecord> pending_free_delete_records{};
    std::deque<PendingFreeDataRecord> pending_free_data_records{};
    SpinMutex pending_free_delete_records_lock;
  };

  bool CheckKeySize(const StringView &key) { return key.size() <= UINT16_MAX; }

  bool CheckValueSize(const StringView &value) {
    return value.size() <= UINT32_MAX;
  }

  Status Init(const std::string &name, const Configs &configs);

  Status HashGetImpl(const StringView &key, std::string *value,
                     uint16_t type_mask);

  inline Status MaybeInitWriteThread();

  Status SearchOrInitCollection(const StringView &collection, Collection **list,
                                bool init, uint16_t collection_type);

  Status SearchOrInitSkiplist(const StringView &collection, Skiplist **skiplist,
                              bool init) {
    if (!CheckKeySize(collection)) {
      return Status::InvalidDataSize;
    }
    return SearchOrInitCollection(collection, (Collection **)skiplist, init,
                                  SortedHeaderRecord);
  }

private:
  std::shared_ptr<UnorderedCollection>
  createUnorderedCollection(StringView const collection_name);
  UnorderedCollection *findUnorderedCollection(StringView collection_name);

  std::unique_ptr<Queue> createQueue(StringView const collection_name);
  Queue *findQueue(StringView const collection_name);

  enum class QueueOpPosition { Left, Right };
  Status xPush(StringView const collection_name, StringView const value,
               QueueOpPosition push_pos);

  Status xPop(StringView const collection_name, std::string *value,
              QueueOpPosition pop_pos);

  Status MaybeInitPendingBatchFile();

  Status StringSetImpl(const StringView &key, const StringView &value);

  Status StringDeleteImpl(const StringView &key);

  Status StringBatchWriteImpl(const WriteBatch::KV &kv,
                              BatchWriteHint &batch_hint);

  Status SSetImpl(Skiplist *skiplist, const StringView &user_key,
                  const StringView &value);

  Status SDeleteImpl(Skiplist *skiplist, const StringView &user_key);

  Status Recovery();

  Status RestoreData(uint64_t thread_id);

  Status RestoreSkiplistHead(DLRecord *pmem_record,
                             const DataEntry &cached_entry);

  Status RestoreStringRecord(StringRecord *pmem_record,
                             const DataEntry &cached_entry);

  Status RestoreSkiplistRecord(DLRecord *pmem_record,
                               const DataEntry &cached_data_entry);

  // Check if a doubly linked record has been successfully inserted, and try
  // repair un-finished prev pointer
  bool CheckAndRepairDLRecord(DLRecord *record);

  bool ValidateRecord(void *data_record);

  bool ValidateRecordAndGetValue(void *data_record, uint32_t expected_checksum,
                                 std::string *value);

  Status RestorePendingBatch();

  Status PersistOrRecoverImmutableConfigs();

  Status RestoreDlistRecords(DLRecord *pmp_record);

  Status RestoreQueueRecords(DLRecord *pmp_record);

  // Regularly works excecuted by background thread
  void backgroundWorkImpl() {
    bg_free_cv_.notify_all();
    version_controller_.UpdatedOldestSnapshot();
    pmem_allocator_->BackgroundWork();
  }

  Status CheckConfigs(const Configs &configs);

  void FreeSkiplistDramNodes();

  void maybeUpdateOldestSnapshot();

  void maybeHandleCachedPendingFreeSpace();

  void handlePendingFreeSpace();

  void backgroundPendingFreeSpaceHandler();

  inline void delayFree(PendingFreeDeleteRecord &&);

  inline void delayFree(PendingFreeDataRecord &&);

  SizedSpaceEntry handlePendingFreeRecord(const PendingFreeDataRecord &);

  SizedSpaceEntry handlePendingFreeRecord(const PendingFreeDeleteRecord &);

  void backgroundWork();

  inline std::string db_file_name() { return dir_ + "data"; }

  inline std::string persisted_pending_block_file(int thread_id) {
    return pending_batch_dir_ + std::to_string(thread_id);
  }

  inline std::string config_file_name() { return dir_ + "configs"; }

  inline bool checkDLRecordLinkageLeft(DLRecord *pmp_record) {
    uint64_t offset = pmem_allocator_->addr2offset_checked(pmp_record);
    DLRecord *pmem_record_prev =
        pmem_allocator_->offset2addr_checked<DLRecord>(pmp_record->prev);
    return pmem_record_prev->next == offset;
  }

  inline bool checkDLRecordLinkageRight(DLRecord *pmp_record) {
    uint64_t offset = pmem_allocator_->addr2offset_checked(pmp_record);
    DLRecord *pmp_next =
        pmem_allocator_->offset2addr_checked<DLRecord>(pmp_record->next);
    return pmp_next->prev == offset;
  }

  bool checkLinkage(DLRecord *pmp_record) {
    uint64_t offset = pmem_allocator_->addr2offset_checked(pmp_record);
    DLRecord *pmp_prev =
        pmem_allocator_->offset2addr_checked<DLRecord>(pmp_record->prev);
    DLRecord *pmp_next =
        pmem_allocator_->offset2addr_checked<DLRecord>(pmp_record->next);
    bool is_linked_left = (pmp_prev->next == offset);
    bool is_linked_right = (pmp_next->prev == offset);

    if (is_linked_left && is_linked_right) {
      return true;
    } else if (!is_linked_left && !is_linked_right) {
      return false;
    } else if (is_linked_left && !is_linked_right) {
      /// TODO: Repair this situation
      GlobalLogger.Error(
          "Broken DLDataEntry linkage: prev<=>curr->right, abort...\n");
      std::abort();
    } else {
      GlobalLogger.Error("Broken DLDataEntry linkage: prev<-curr<=>right, "
                         "which is logically impossible! Abort...\n");
      std::abort();
    }
  }

  inline void purgeAndFree(void *pmem_record) {
    DataEntry *data_entry = static_cast<DataEntry *>(pmem_record);
    data_entry->Destroy();
    pmem_allocator_->Free(SizedSpaceEntry(
        pmem_allocator_->addr2offset_checked(pmem_record),
        data_entry->header.record_size, data_entry->meta.timestamp));
  }

  inline void free(void *pmem_record) {
    DataEntry *data_entry = static_cast<DataEntry *>(pmem_record);
    pmem_allocator_->Free(SizedSpaceEntry(
        pmem_allocator_->addr2offset_checked(pmem_record),
        data_entry->header.record_size, data_entry->meta.timestamp));
  }

  Array<ThreadCache> thread_cache_;

  // restored kvs in reopen
  std::atomic<uint64_t> restored_{0};
  std::atomic<CollectionIDType> list_id_{0};

  std::shared_ptr<HashTable> hash_table_;

  std::vector<std::shared_ptr<Skiplist>> skiplists_;
  std::vector<std::shared_ptr<UnorderedCollection>>
      vec_sp_unordered_collections_;
  std::vector<std::unique_ptr<Queue>> queue_uptr_vec_;
  std::mutex list_mu_;

  std::string dir_;
  std::string pending_batch_dir_;
  std::string db_file_;
  std::shared_ptr<ThreadManager> thread_manager_;
  std::shared_ptr<PMEMAllocator> pmem_allocator_;
  Configs configs_;
  bool closing_{false};
  std::vector<std::thread> bg_threads_;
  SortedCollectionRebuilder sorted_rebuilder_;
  VersionController version_controller_;

  // background free space
  std::vector<std::deque<PendingFreeDataRecord>>
      pending_free_data_records_pool_;
  std::vector<std::deque<PendingFreeDeleteRecord>>
      pending_free_delete_records_pool_;
  bool bg_free_processing_{false};
  bool bg_free_closed_{false};
  SpinMutex bg_free_lock_;
  std::condition_variable_any bg_free_cv_;
};

} // namespace KVDK_NAMESPACE
