#ifndef SQL_BASIC_ROW_ITERATORS_H_
#define SQL_BASIC_ROW_ITERATORS_H_

/* Copyright (c) 2018, 2021, Oracle and/or its affiliates. All rights reserved.
   Copyright (c) 2021, Huawei Technologies Co., Ltd.
   Copyright (c) 2021, GreatDB Software Co., Ltd

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
  @file
  Row iterators that scan a single table without reference to other tables
  or iterators.
 */

#include <sys/types.h>

#include <memory>

#include "filesort.h"
#include "map_helpers.h"
#include "mem_root_deque.h"
#include "my_alloc.h"
#include "my_base.h"
#include "my_inttypes.h"
#include "sql/mem_root_array.h"
#include "sql/row_iterator.h"
#include "sql/sql_executor.h"
#include "sql/sql_list.h"

class Filesort_info;
class Item;
class QUICK_SELECT_I;
class Sort_result;
class THD;
class handler;
struct IO_CACHE;
struct TABLE;
class Gather_operator;

class ORDER;
class MQ_record_gather;
class QEP_TAB;

/**
 * Parallel scan iterator, which is used in parallel leader
 */
class ParallelScanIterator final : public TableRowIterator {
 public:
  ParallelScanIterator(THD *thd, QEP_TAB *tab, TABLE *table,
                       ha_rows *examined_rows, JOIN *join,
                       Gather_operator *gather, bool stab_output = false,
                       uint ref_length = 0);

  ~ParallelScanIterator() override;

  bool Init() override;
  int Read() override;
  int End() override;

 public:
  void UnlockRow() override {}
  void SetNullRowFlag(bool) override {}
  void StartPSIBatchMode() override {}
  void EndPSIBatchModeIfStarted() override {}

 private:
  uchar *const m_record;
  ha_rows *const m_examined_rows;
  uint m_dop;
  JOIN *m_join;
  Gather_operator *m_gather;
  MQ_record_gather *m_record_gather;
  ORDER *m_order; /** use for records merge sort */
  QEP_TAB *m_tab;

  bool m_stable_sort; /** determine whether using stable sort */
  uint m_ref_length;

 private:
  /** construct filesort on leader when needing stab_output or merge_sort */
  bool pq_make_filesort(Filesort **sort);
  /** init m_record_gather */
  bool pq_init_record_gather();
  /** launch worker threads to execute parallel query */
  bool pq_launch_worker();
  /** wait all workers finished */
  void pq_wait_workers_finished();
  /** outoput parallel query error code */
  int pq_error_code();
};

class PQ_worker_manager;

/**
 * block scan iterator, which is used is in parallel worker.
 * a whole talbe is cut into many blocks for parallel scan
 */
class PQblockScanIterator final : public TableRowIterator {
 public:
  PQblockScanIterator(THD *thd, TABLE *table, uchar *record,
                      ha_rows *examined_rows, Gather_operator *gather,
                      bool need_rowid = false);
  ~PQblockScanIterator() override;

  bool Init() override;
  int Read() override;
  int End() override;

 private:
  uchar *const m_record;
  ha_rows *const m_examined_rows;
  void *m_pq_ctx;  // parallel query context
  uint keyno;
  Gather_operator *m_gather;

  bool m_need_rowid;
};

/**
 * pq explain iterator, which is used to store worker explain analyze
 * information.
 */
class PQExplainIterator : public RowIterator {
 public:
  PQExplainIterator() : RowIterator(NULL) {}
  ~PQExplainIterator() {}

  void copy(RowIterator *src_iterator);
  bool Init() override { return 0; }
  int Read() override { return 0; }
  int End() override { return 0; }
  void UnlockRow() override {}
  void SetNullRowFlag(bool) override {}
  std::string TimingString() const override { return time_string; }

 private:
  std::vector<std::string> str;
  std::vector<unique_ptr_destroy_only<PQExplainIterator>> iter;
  std::string time_string;
};

/**
  Scan a table from beginning to end.

  This is the most basic access method of a table using rnd_init,
  ha_rnd_next and rnd_end. No indexes are used.
 */
class TableScanIterator final : public TableRowIterator {
 public:
  // “expected_rows” is used for scaling the record buffer.
  // If zero or less, no record buffer will be set up.
  //
  // "examined_rows", if not nullptr, is incremented for each successful Read().
  TableScanIterator(THD *thd, TABLE *table, double expected_rows,
                    ha_rows *examined_rows);
  ~TableScanIterator() override;

  bool Init() override;
  int Read() override;

 private:
  uchar *const m_record;
  const double m_expected_rows;
  ha_rows *const m_examined_rows;
};

/** Perform a full index scan along an index. */
template <bool Reverse>
class IndexScanIterator final : public TableRowIterator {
 public:
  // use_order must be set to true if you actually need to get the records
  // back in index order. It can be set to false if you wish to scan
  // using the index (e.g. for an index-only scan of the entire table),
  // but do not actually care about the order. In particular, partitioned
  // tables can use this to deliver more efficient scans.
  //
  // “expected_rows” is used for scaling the record buffer.
  // If zero or less, no record buffer will be set up.
  //
  // The pushed condition can be nullptr.
  //
  // "examined_rows", if not nullptr, is incremented for each successful Read().
  IndexScanIterator(THD *thd, TABLE *table, int idx, bool use_order,
                    double expected_rows, ha_rows *examined_rows);
  ~IndexScanIterator() override;

  bool Init() override;
  int Read() override;

 private:
  uchar *const m_record;
  const int m_idx;
  const bool m_use_order;
  const double m_expected_rows;
  ha_rows *const m_examined_rows;
  bool m_first = true;
};

/**
  Scan a given range of the table (a “quick”), using an index.

  IndexRangeScanIterator uses one of the QUICK_SELECT classes in opt_range.cc
  to perform an index scan. There are loads of functionality hidden
  in these quick classes. It handles all index scans of various kinds.

  TODO: Convert the QUICK_SELECT framework to RowIterator, so that
  we do not need this adapter.
 */
class IndexRangeScanIterator final : public TableRowIterator {
 public:
  // Does _not_ take ownership of "quick" (but maybe it should).
  //
  // “expected_rows” is used for scaling the record buffer.
  // If zero or less, no record buffer will be set up.
  //
  // The pushed condition can be nullptr.
  //
  // "examined_rows", if not nullptr, is incremented for each successful Read().
  IndexRangeScanIterator(THD *thd, TABLE *table, QUICK_SELECT_I *quick,
                         double expected_rows, ha_rows *examined_rows);

  bool Init() override;
  int Read() override;

 private:
  // NOTE: No destructor; quick_range will call ha_index_or_rnd_end() for us.
  QUICK_SELECT_I *const m_quick;
  const double m_expected_rows;
  ha_rows *const m_examined_rows;

  // After m_quick has returned EOF, some of its members are destroyed, making
  // subsequent requests for new rows undefined. We flag EOF so that the
  // iterator does not request a new row.
  bool m_seen_eof{false};
};

// Readers relating to reading sorted data (from filesort).
//
// Filesort will produce references to the records sorted; these
// references can be stored in memory or in a temporary file.
//
// The temporary file is normally used when the references doesn't fit into
// a properly sized memory buffer. For most small queries the references
// are stored in the memory buffer.
//
// The temporary file is also used when performing an update where a key is
// modified.

/**
  Fetch the records from a memory buffer.

  This method is used when table->sort.addon_field is allocated.
  This is allocated for most SELECT queries not involving any BLOB's.
  In this case the records are fetched from a memory buffer.
 */
template <bool Packed_addon_fields>
class SortBufferIterator final : public RowIterator {
 public:
  // "examined_rows", if not nullptr, is incremented for each successful Read().
  // The table is used solely for NULL row flags.
  SortBufferIterator(THD *thd, Mem_root_array<TABLE *> tables,
                     Filesort_info *sort, Sort_result *sort_result,
                     ha_rows *examined_rows);
  ~SortBufferIterator() override;

  bool Init() override;
  int Read() override;
  void UnlockRow() override {}
  void SetNullRowFlag(bool is_null_row) override;

 private:
  // NOTE: No m_record -- unpacks directly into each Field's field->ptr.
  Filesort_info *const m_sort;
  Sort_result *const m_sort_result;
  unsigned m_unpack_counter;
  ha_rows *const m_examined_rows;
  Mem_root_array<TABLE *> m_tables;
};

/**
  Fetch the record IDs from a memory buffer, but the records themselves from
  the table on disk.

  Used when the above (comment on SortBufferIterator) is not true, UPDATE,
  DELETE and so forth and SELECT's involving large BLOBs. It is also used for
  the result of Unique, which returns row IDs in the same format as filesort.
  In this case the record data is fetched from the handler using the saved
  reference using the rnd_pos handler call.
 */
class SortBufferIndirectIterator final : public RowIterator {
 public:
  // Ownership here is suboptimal: Takes only partial ownership of
  // "sort_result", so it must be alive for as long as the RowIterator is.
  // However, it _does_ free the buffers within on destruction.
  //
  // The pushed condition can be nullptr.
  //
  // "examined_rows", if not nullptr, is incremented for each successful Read().
  SortBufferIndirectIterator(THD *thd, Mem_root_array<TABLE *> tables,
                             Sort_result *sort_result,
                             bool ignore_not_found_rows, bool has_null_flags,
                             ha_rows *examined_rows);
  ~SortBufferIndirectIterator() override;
  bool Init() override;
  int Read() override;
  void SetNullRowFlag(bool is_null_row) override;
  void UnlockRow() override {}

 private:
  Sort_result *const m_sort_result;
  Mem_root_array<TABLE *> m_tables;
  uint m_sum_ref_length;
  ha_rows *const m_examined_rows;
  uchar *m_cache_pos = nullptr, *m_cache_end = nullptr;
  bool m_ignore_not_found_rows;
  bool m_has_null_flags;
};

/**
  Fetch the records from a tempoary file.

  There used to be a comment here saying “should obviously not really happen
  other than in strange configurations”, but especially with packed addons
  and InnoDB (where fetching rows needs a primary key lookup), it's not
  necessarily suboptimal compared to e.g. SortBufferIndirectIterator.
 */
template <bool Packed_addon_fields>
class SortFileIterator final : public RowIterator {
 public:
  // Takes ownership of tempfile.
  // The table is used solely for NULL row flags.
  SortFileIterator(THD *thd, Mem_root_array<TABLE *> tables, IO_CACHE *tempfile,
                   Filesort_info *sort, ha_rows *examined_rows);
  ~SortFileIterator() override;

  bool Init() override { return false; }
  int Read() override;
  void UnlockRow() override {}
  void SetNullRowFlag(bool is_null_row) override;

 private:
  uchar *const m_rec_buf;
  const uint m_buf_length;
  Mem_root_array<TABLE *> m_tables;
  IO_CACHE *const m_io_cache;
  Filesort_info *const m_sort;
  ha_rows *const m_examined_rows;
};

/**
  Fetch the record IDs from a temporary file, then the records themselves from
  the table on disk.

  Same as SortBufferIndirectIterator except that references are fetched
  from temporary file instead of from a memory buffer. So first the record IDs
  are read from file, then those record IDs are used to look up rows in the
  table.
 */
class SortFileIndirectIterator final : public RowIterator {
 public:
  // Takes ownership of tempfile.
  //
  // The pushed condition can be nullptr.
  //
  // "examined_rows", if not nullptr, is incremented for each successful Read().
  SortFileIndirectIterator(THD *thd, Mem_root_array<TABLE *> tables,
                           IO_CACHE *tempfile, bool ignore_not_found_rows,
                           bool has_null_flags, ha_rows *examined_rows);
  ~SortFileIndirectIterator() override;

  bool Init() override;
  int Read() override;
  void SetNullRowFlag(bool is_null_row) override;
  void UnlockRow() override {}

 private:
  IO_CACHE *m_io_cache = nullptr;
  ha_rows *const m_examined_rows;
  Mem_root_array<TABLE *> m_tables;
  uchar *m_ref_pos = nullptr;
  bool m_ignore_not_found_rows;
  bool m_has_null_flags;

  uint m_sum_ref_length;
};

// Used when the plan is const, ie. is known to contain a single row
// (and all values have been read in advance, so we don't need to read
// a single table).
class FakeSingleRowIterator final : public RowIterator {
 public:
  // "examined_rows", if not nullptr, is incremented for each successful Read().
  FakeSingleRowIterator(THD *thd, ha_rows *examined_rows)
      : RowIterator(thd), m_examined_rows(examined_rows) {}

  bool Init() override {
    m_has_row = true;
    return false;
  }

  int Read() override {
    if (m_has_row) {
      m_has_row = false;
      if (m_examined_rows != nullptr) {
        ++*m_examined_rows;
      }
      return 0;
    } else {
      return -1;
    }
  }

  void SetNullRowFlag(bool is_null_row MY_ATTRIBUTE((unused))) override {
    assert(!is_null_row);
  }

  void UnlockRow() override {}

 private:
  bool m_has_row;
  ha_rows *const m_examined_rows;
};

/**
  An iterator for unqualified COUNT(*) (ie., no WHERE, no join conditions,
  etc.), taking a special fast path in the handler. It returns a single row,
  much like FakeSingleRowIterator; however, unlike said iterator, it actually
  does the counting in Read() instead of expecting all fields to already be
  filled out.
 */
class UnqualifiedCountIterator final : public RowIterator {
 public:
  UnqualifiedCountIterator(THD *thd, JOIN *join)
      : RowIterator(thd), m_join(join) {}

  bool Init() override {
    m_has_row = true;
    return false;
  }

  int Read() override;

  void SetNullRowFlag(bool) override { assert(false); }

  void UnlockRow() override {}

 private:
  bool m_has_row;
  JOIN *const m_join;
};

/**
  A simple iterator that takes no input and produces zero output rows.
  Used when the optimizer has figured out ahead of time that a given table
  can produce no output (e.g. SELECT ... WHERE 2+2 = 5).

  The child iterator is optional (can be nullptr) if SetNullRowFlag() is
  not to be called. It is used when a subtree used on the inner side of an
  outer join is found to be never executable, and replaced with a
  ZeroRowsIterator; in that case, we need to forward the SetNullRowFlag call
  to it. This child is not printed as part of the iterator tree.
 */
class ZeroRowsIterator final : public RowIterator {
 public:
  ZeroRowsIterator(THD *thd,
                   unique_ptr_destroy_only<RowIterator> child_iterator)
      : RowIterator(thd), m_child_iterator(std::move(child_iterator)) {}

  bool Init() override { return false; }

  int Read() override { return -1; }

  void SetNullRowFlag(bool is_null_row) override {
    assert(m_child_iterator != nullptr);
    m_child_iterator->SetNullRowFlag(is_null_row);
  }

  void UnlockRow() override {}

 private:
  unique_ptr_destroy_only<RowIterator> m_child_iterator;
};

class Query_block;

/**
  Like ZeroRowsIterator, but produces a single output row, since there are
  aggregation functions present and no GROUP BY. E.g.,

    SELECT SUM(f1) FROM t1 WHERE 2+2 = 5;

  should produce a single row, containing only the value NULL.
 */
class ZeroRowsAggregatedIterator final : public RowIterator {
 public:
  // "examined_rows", if not nullptr, is incremented for each successful Read().
  ZeroRowsAggregatedIterator(THD *thd, JOIN *join, ha_rows *examined_rows)
      : RowIterator(thd), m_join(join), m_examined_rows(examined_rows) {}

  bool Init() override {
    m_has_row = true;
    return false;
  }

  int Read() override;

  void SetNullRowFlag(bool) override { assert(false); }

  void UnlockRow() override {}

 private:
  bool m_has_row;
  JOIN *const m_join;
  ha_rows *const m_examined_rows;
};

/**
  FollowTailIterator is a special version of TableScanIterator that is used
  as part of WITH RECURSIVE queries. It is designed to read from a temporary
  table at the same time as MaterializeIterator writes to the same table,
  picking up new records in the order they come in -- it follows the tail,
  much like the UNIX tool “tail -f”.

  Furthermore, when materializing a recursive query expression consisting of
  multiple query blocks, MaterializeIterator needs to run each block several
  times until convergence. (For a single query block, one iteration suffices,
  since the iterator sees new records as they come in.) Each such run, the
  recursive references should see only rows that were added since the last
  iteration, even though Init() is called anew. FollowTailIterator is thus
  different from TableScanIterator in that subsequent calls to Init() do not
  move the cursor back to the start.

  In addition, FollowTailIterator implements the WITH RECURSIVE iteration limit.
  This is not specified in terms of Init() calls, since one run can encompass
  many iterations. Instead, it keeps track of the number of records in the table
  at the start of iteration, and when it has read all of those records, the next
  iteration is deemed to have begun. If the iteration counter is above the
  user-set limit, it raises an error to stop runaway queries with infinite
  recursion.
 */
class FollowTailIterator final : public TableRowIterator {
 public:
  // "examined_rows", if not nullptr, is incremented for each successful Read().
  FollowTailIterator(THD *thd, TABLE *table, double expected_rows,
                     ha_rows *examined_rows);
  ~FollowTailIterator() override;

  bool Init() override;
  int Read() override;

  /**
    Signal where we can expect to find the number of generated rows for this
    materialization (this points into the MaterializeIterator's data).

    This must be called when we start materializing the CTE,
    before Init() runs.
   */
  void set_stored_rows_pointer(ha_rows *stored_rows) {
    m_stored_rows = stored_rows;
  }

  /**
    Signal to the iterator that the underlying table was closed and replaced
    with an InnoDB table with the same data, due to a spill-to-disk
    (e.g. the table used to be MEMORY and now is InnoDB). This is
    required so that Read() can continue scanning from the right place.
    Called by MaterializeIterator::MaterializeRecursive().
   */
  bool RepositionCursorAfterSpillToDisk();

 private:
  bool m_inited = false;
  uchar *const m_record;
  const double m_expected_rows;
  ha_rows *const m_examined_rows;
  ha_rows m_read_rows;
  ha_rows m_end_of_current_iteration;
  unsigned m_recursive_iteration_count;

  // Points into MaterializeIterator's data; set by BeginMaterialization() only.
  ha_rows *m_stored_rows = nullptr;
};

/**
  TableValueConstructor is the iterator for the table value constructor case of
  a query_primary (i.e. queries of the form VALUES row_list; e.g. VALUES ROW(1,
  10), ROW(2, 20)).

  The iterator is passed the field list of its parent JOIN object, which may
  contain Item_values_column objects that are created during
  Query_block::prepare_values(). This is required so that Read() can replace the
  currently selected row by simply changing the references of Item_values_column
  objects to the next row.

  The iterator must output multiple rows without being materialized, and does
  not scan any tables. The indirection of Item_values_column is required, as the
  executor outputs what is contained in join->fields (either directly, or
  indirectly through ConvertItemsToCopy), and is thus responsible for ensuring
  that join->fields contains the correct next row.
 */
class TableValueConstructorIterator final : public RowIterator {
 public:
  TableValueConstructorIterator(
      THD *thd, ha_rows *examined_rows,
      const mem_root_deque<mem_root_deque<Item *> *> &row_value_list,
      mem_root_deque<Item *> *join_fields);

  bool Init() override;
  int Read() override;

  void SetNullRowFlag(bool) override { assert(false); }

  void UnlockRow() override {}

 private:
  ha_rows *const m_examined_rows{nullptr};

  /// Contains the row values that are part of a VALUES clause. Read() will
  /// modify contained Item objects during execution by calls to is_null() and
  /// the required val function to extract its value.
  const mem_root_deque<mem_root_deque<Item *> *> &m_row_value_list;
  mem_root_deque<mem_root_deque<Item *> *>::const_iterator m_row_it;

  /// References to the row we currently want to output. When multiple rows must
  /// be output, this contains Item_values_column objects. In this case, each
  /// call to Read() will replace its current reference with the next row.
  mem_root_deque<Item *> *const m_output_refs;
};

#endif  // SQL_BASIC_ROW_ITERATORS_H_
