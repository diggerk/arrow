// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "arrow/testing/macros.h"
#include "parquet/column_page.h"
#include "parquet/column_reader.h"
#include "parquet/schema.h"
#include "parquet/test_util.h"
#include "parquet/types.h"

namespace parquet {

using schema::NodePtr;

namespace test {

template <typename T>
static inline bool vector_equal_with_def_levels(const std::vector<T>& left,
                                                const std::vector<int16_t>& def_levels,
                                                int16_t max_def_levels,
                                                int16_t max_rep_levels,
                                                const std::vector<T>& right) {
  size_t i_left = 0;
  size_t i_right = 0;
  for (size_t i = 0; i < def_levels.size(); i++) {
    if (def_levels[i] == max_def_levels) {
      // Compare
      if (left[i_left] != right[i_right]) {
        std::cerr << "index " << i << " left was " << left[i_left] << " right was "
                  << right[i] << std::endl;
        return false;
      }
      i_left++;
      i_right++;
    } else if (def_levels[i] == (max_def_levels - 1)) {
      // Null entry on the lowest nested level
      i_right++;
    } else if (def_levels[i] < (max_def_levels - 1)) {
      // Null entry on a higher nesting level, only supported for non-repeating data
      if (max_rep_levels == 0) {
        i_right++;
      }
    }
  }

  return true;
}

template <typename Type>
class TestPrimitiveReader : public ::testing::Test {
 public:
  using c_type = typename Type::c_type;

  void InitReader(const ColumnDescriptor* d) {
    std::unique_ptr<PageReader> pager_;
    pager_.reset(new test::MockPageReader(pages_));
    reader_ = ColumnReader::Make(d, std::move(pager_));
  }

  void CheckResults() {
    std::vector<c_type> vresult(num_values_, -1);
    std::vector<uint8_t> vresult_bool(num_values_, -1);
    c_type* vresult_ptr = ResultsPointer(vresult, vresult_bool);

    std::vector<int16_t> dresult(num_levels_, -1);
    std::vector<int16_t> rresult(num_levels_, -1);
    int64_t values_read = 0;
    int total_values_read = 0;
    int batch_actual = 0;

    TypedColumnReader<Type>* reader =
        static_cast<TypedColumnReader<Type>*>(reader_.get());
    int32_t batch_size = 8;
    int batch = 0;
    // This will cover both the cases
    // 1) batch_size < page_size (multiple ReadBatch from a single page)
    // 2) batch_size > page_size (BatchRead limits to a single page)
    do {
      batch = static_cast<int>(reader->ReadBatch(
          batch_size, &dresult[0] + batch_actual, &rresult[0] + batch_actual,
          vresult_ptr + total_values_read, &values_read));
      total_values_read += static_cast<int>(values_read);
      batch_actual += batch;
      batch_size = std::min(1 << 24, std::max(batch_size * 2, 4096));
    } while (batch > 0);

    ASSERT_EQ(num_levels_, batch_actual);
    ASSERT_EQ(num_values_, total_values_read);
    ASSERT_TRUE(CompareResults(vresult, vresult_bool));
    if (max_def_level_ > 0) {
      ASSERT_TRUE(vector_equal(def_levels_, dresult));
    }
    if (max_rep_level_ > 0) {
      ASSERT_TRUE(vector_equal(rep_levels_, rresult));
    }
    // catch improper writes at EOS
    batch_actual =
        static_cast<int>(reader->ReadBatch(5, nullptr, nullptr, nullptr, &values_read));
    ASSERT_EQ(0, batch_actual);
    ASSERT_EQ(0, values_read);
  }
  void CheckResultsSpaced() {
    std::vector<typename Type::c_type> vresult(num_levels_, -1);
    std::vector<uint8_t> vresult_bool(num_levels_, -1);
    c_type* vresult_ptr = ResultsPointer(vresult, vresult_bool);

    std::vector<int16_t> dresult(num_levels_, -1);
    std::vector<int16_t> rresult(num_levels_, -1);
    std::vector<uint8_t> valid_bits(num_levels_, 255);
    int total_values_read = 0;
    int batch_actual = 0;
    int levels_actual = 0;
    int64_t null_count = -1;
    int64_t levels_read = 0;
    int64_t values_read;

    TypedColumnReader<Type>* reader =
        static_cast<TypedColumnReader<Type>*>(reader_.get());
    int32_t batch_size = 8;
    int batch = 0;
    // This will cover both the cases
    // 1) batch_size < page_size (multiple ReadBatch from a single page)
    // 2) batch_size > page_size (BatchRead limits to a single page)
    do {
      SUPPRESS_DEPRECATION_WARNING;
      batch = static_cast<int>(reader->ReadBatchSpaced(
          batch_size, dresult.data() + levels_actual, rresult.data() + levels_actual,
          vresult_ptr + batch_actual, valid_bits.data() + batch_actual, 0, &levels_read,
          &values_read, &null_count));
      UNSUPPRESS_DEPRECATION_WARNING;
      total_values_read += batch - static_cast<int>(null_count);
      batch_actual += batch;
      levels_actual += static_cast<int>(levels_read);
      batch_size = std::min(1 << 24, std::max(batch_size * 2, 4096));
    } while ((batch > 0) || (levels_read > 0));

    ASSERT_EQ(num_levels_, levels_actual);
    ASSERT_EQ(num_values_, total_values_read);
    if (max_def_level_ > 0) {
      ASSERT_TRUE(vector_equal(def_levels_, dresult));
      ASSERT_TRUE(vector_equal_with_def_levels(values_, dresult, max_def_level_,
                                               max_rep_level_, vresult));
    } else {
      ASSERT_TRUE(CompareResults(vresult, vresult_bool));
    }
    if (max_rep_level_ > 0) {
      ASSERT_TRUE(vector_equal(rep_levels_, rresult));
    }
    // catch improper writes at EOS
    SUPPRESS_DEPRECATION_WARNING;
    batch_actual = static_cast<int>(
        reader->ReadBatchSpaced(5, nullptr, nullptr, nullptr, valid_bits.data(), 0,
                                &levels_read, &values_read, &null_count));
    UNSUPPRESS_DEPRECATION_WARNING;
    ASSERT_EQ(0, batch_actual);
    ASSERT_EQ(0, null_count);
  }

  c_type* ResultsPointer(std::vector<c_type>& vresult,
                         std::vector<uint8_t>& vresult_bool);

  bool CompareResults(std::vector<c_type>& vresult, std::vector<uint8_t>& vresult_bool);

  void Clear() {
    values_.clear();
    def_levels_.clear();
    rep_levels_.clear();
    pages_.clear();
    reader_.reset();
  }

  void ExecutePlain(int num_pages, int levels_per_page, const ColumnDescriptor* d) {
    num_values_ = MakePages<Type>(d, num_pages, levels_per_page, def_levels_, rep_levels_,
                                  values_, data_buffer_, pages_, Encoding::PLAIN);
    num_levels_ = num_pages * levels_per_page;
    InitReader(d);
    CheckResults();
    Clear();

    num_values_ = MakePages<Type>(d, num_pages, levels_per_page, def_levels_, rep_levels_,
                                  values_, data_buffer_, pages_, Encoding::PLAIN);
    num_levels_ = num_pages * levels_per_page;
    InitReader(d);
    CheckResultsSpaced();
    Clear();
  }

  void ExecuteDict(int num_pages, int levels_per_page, const ColumnDescriptor* d) {
    num_values_ =
        MakePages<Type>(d, num_pages, levels_per_page, def_levels_, rep_levels_, values_,
                        data_buffer_, pages_, Encoding::RLE_DICTIONARY);
    num_levels_ = num_pages * levels_per_page;
    InitReader(d);
    CheckResults();
    Clear();

    num_values_ =
        MakePages<Type>(d, num_pages, levels_per_page, def_levels_, rep_levels_, values_,
                        data_buffer_, pages_, Encoding::RLE_DICTIONARY);
    num_levels_ = num_pages * levels_per_page;
    InitReader(d);
    CheckResultsSpaced();
    Clear();
  }

 protected:
  int num_levels_;
  int num_values_;
  int16_t max_def_level_;
  int16_t max_rep_level_;
  std::vector<std::shared_ptr<Page>> pages_;
  std::shared_ptr<ColumnReader> reader_;
  std::vector<c_type> values_;
  std::vector<int16_t> def_levels_;
  std::vector<int16_t> rep_levels_;
  std::vector<uint8_t> data_buffer_;  // For BA and FLBA
};

template <typename Type>
inline typename Type::c_type* TestPrimitiveReader<Type>::ResultsPointer(
    std::vector<typename Type::c_type>& vresult, std::vector<uint8_t>& vresult_bool) {
  return &vresult[0];
}

template <>
inline bool* TestPrimitiveReader<BooleanType>::ResultsPointer(
    std::vector<bool>& vresult, std::vector<uint8_t>& vresult_bool) {
  return reinterpret_cast<bool*>(vresult_bool.data());
}

template <typename Type>
inline bool TestPrimitiveReader<Type>::CompareResults(
    std::vector<c_type>& vresult, std::vector<uint8_t>& vresult_bool) {
  return vector_equal(values_, vresult);
}

template <>
inline bool TestPrimitiveReader<BooleanType>::CompareResults(
    std::vector<bool>& vresult, std::vector<uint8_t>& vresult_bool) {
  vresult.assign(vresult_bool.begin(), vresult_bool.end());
  return vector_equal(values_, vresult);
}

using TestInt32Reader = TestPrimitiveReader<Int32Type>;

TEST_F(TestInt32Reader, TestInt32FlatRequired) {
  int levels_per_page = 100;
  int num_pages = 50;
  max_def_level_ = 0;
  max_rep_level_ = 0;
  NodePtr type = schema::Int32("a", Repetition::REQUIRED);
  const ColumnDescriptor descr(type, max_def_level_, max_rep_level_);
  ASSERT_NO_FATAL_FAILURE(ExecutePlain(num_pages, levels_per_page, &descr));
  ASSERT_NO_FATAL_FAILURE(ExecuteDict(num_pages, levels_per_page, &descr));
}

TEST_F(TestInt32Reader, TestInt32FlatOptional) {
  int levels_per_page = 100;
  int num_pages = 50;
  max_def_level_ = 4;
  max_rep_level_ = 0;
  NodePtr type = schema::Int32("b", Repetition::OPTIONAL);
  const ColumnDescriptor descr(type, max_def_level_, max_rep_level_);
  ASSERT_NO_FATAL_FAILURE(ExecutePlain(num_pages, levels_per_page, &descr));
  ASSERT_NO_FATAL_FAILURE(ExecuteDict(num_pages, levels_per_page, &descr));
}

TEST_F(TestInt32Reader, TestInt32FlatRepeated) {
  int levels_per_page = 100;
  int num_pages = 50;
  max_def_level_ = 4;
  max_rep_level_ = 2;
  NodePtr type = schema::Int32("c", Repetition::REPEATED);
  const ColumnDescriptor descr(type, max_def_level_, max_rep_level_);
  ASSERT_NO_FATAL_FAILURE(ExecutePlain(num_pages, levels_per_page, &descr));
  ASSERT_NO_FATAL_FAILURE(ExecuteDict(num_pages, levels_per_page, &descr));
}

TEST_F(TestInt32Reader, TestInt32FlatRequiredSkip) {
  int levels_per_page = 100;
  int num_pages = 5;
  max_def_level_ = 0;
  max_rep_level_ = 0;
  NodePtr type = schema::Int32("b", Repetition::REQUIRED);
  const ColumnDescriptor descr(type, max_def_level_, max_rep_level_);
  MakePages<Int32Type>(&descr, num_pages, levels_per_page, def_levels_, rep_levels_,
                       values_, data_buffer_, pages_, Encoding::PLAIN);
  InitReader(&descr);
  std::vector<int32_t> vresult(levels_per_page / 2, -1);
  std::vector<int16_t> dresult(levels_per_page / 2, -1);
  std::vector<int16_t> rresult(levels_per_page / 2, -1);

  Int32Reader* reader = static_cast<Int32Reader*>(reader_.get());
  int64_t values_read = 0;

  // 1) skip_size > page_size (multiple pages skipped)
  // Skip first 2 pages
  int64_t levels_skipped = reader->Skip(2 * levels_per_page);
  ASSERT_EQ(2 * levels_per_page, levels_skipped);
  // Read half a page
  reader->ReadBatch(levels_per_page / 2, dresult.data(), rresult.data(), vresult.data(),
                    &values_read);
  std::vector<int32_t> sub_values(
      values_.begin() + 2 * levels_per_page,
      values_.begin() + static_cast<int>(2.5 * static_cast<double>(levels_per_page)));
  ASSERT_TRUE(vector_equal(sub_values, vresult));

  // 2) skip_size == page_size (skip across two pages)
  levels_skipped = reader->Skip(levels_per_page);
  ASSERT_EQ(levels_per_page, levels_skipped);
  // Read half a page
  reader->ReadBatch(levels_per_page / 2, dresult.data(), rresult.data(), vresult.data(),
                    &values_read);
  sub_values.clear();
  sub_values.insert(
      sub_values.end(),
      values_.begin() + static_cast<int>(3.5 * static_cast<double>(levels_per_page)),
      values_.begin() + 4 * levels_per_page);
  ASSERT_TRUE(vector_equal(sub_values, vresult));

  // 3) skip_size < page_size (skip limited to a single page)
  // Skip half a page
  levels_skipped = reader->Skip(levels_per_page / 2);
  ASSERT_EQ(0.5 * levels_per_page, levels_skipped);
  // Read half a page
  reader->ReadBatch(levels_per_page / 2, dresult.data(), rresult.data(), vresult.data(),
                    &values_read);
  sub_values.clear();
  sub_values.insert(
      sub_values.end(),
      values_.begin() + static_cast<int>(4.5 * static_cast<double>(levels_per_page)),
      values_.end());
  ASSERT_TRUE(vector_equal(sub_values, vresult));

  values_.clear();
  def_levels_.clear();
  rep_levels_.clear();
  pages_.clear();
  reader_.reset();
}

TEST_F(TestInt32Reader, TestDictionaryEncodedPages) {
  max_def_level_ = 0;
  max_rep_level_ = 0;
  NodePtr type = schema::Int32("a", Repetition::REQUIRED);
  const ColumnDescriptor descr(type, max_def_level_, max_rep_level_);
  std::shared_ptr<ResizableBuffer> dummy = AllocateBuffer();

  std::shared_ptr<DictionaryPage> dict_page =
      std::make_shared<DictionaryPage>(dummy, 0, Encoding::PLAIN);
  std::shared_ptr<DataPageV1> data_page = MakeDataPage<Int32Type>(
      &descr, {}, 0, Encoding::RLE_DICTIONARY, {}, 0, {}, 0, {}, 0);
  pages_.push_back(dict_page);
  pages_.push_back(data_page);
  InitReader(&descr);
  // Tests Dict : PLAIN, Data : RLE_DICTIONARY
  ASSERT_NO_THROW(reader_->HasNext());
  pages_.clear();

  dict_page = std::make_shared<DictionaryPage>(dummy, 0, Encoding::PLAIN_DICTIONARY);
  data_page = MakeDataPage<Int32Type>(&descr, {}, 0, Encoding::PLAIN_DICTIONARY, {}, 0,
                                      {}, 0, {}, 0);
  pages_.push_back(dict_page);
  pages_.push_back(data_page);
  InitReader(&descr);
  // Tests Dict : PLAIN_DICTIONARY, Data : PLAIN_DICTIONARY
  ASSERT_NO_THROW(reader_->HasNext());
  pages_.clear();

  data_page = MakeDataPage<Int32Type>(&descr, {}, 0, Encoding::RLE_DICTIONARY, {}, 0, {},
                                      0, {}, 0);
  pages_.push_back(data_page);
  InitReader(&descr);
  // Tests dictionary page must occur before data page
  ASSERT_THROW(reader_->HasNext(), ParquetException);
  pages_.clear();

  dict_page = std::make_shared<DictionaryPage>(dummy, 0, Encoding::DELTA_BYTE_ARRAY);
  pages_.push_back(dict_page);
  InitReader(&descr);
  // Tests only RLE_DICTIONARY is supported
  ASSERT_THROW(reader_->HasNext(), ParquetException);
  pages_.clear();

  std::shared_ptr<DictionaryPage> dict_page1 =
      std::make_shared<DictionaryPage>(dummy, 0, Encoding::PLAIN_DICTIONARY);
  std::shared_ptr<DictionaryPage> dict_page2 =
      std::make_shared<DictionaryPage>(dummy, 0, Encoding::PLAIN);
  pages_.push_back(dict_page1);
  pages_.push_back(dict_page2);
  InitReader(&descr);
  // Column cannot have more than one dictionary
  ASSERT_THROW(reader_->HasNext(), ParquetException);
  pages_.clear();

  data_page = MakeDataPage<Int32Type>(&descr, {}, 0, Encoding::DELTA_BYTE_ARRAY, {}, 0,
                                      {}, 0, {}, 0);
  pages_.push_back(data_page);
  InitReader(&descr);
  // unsupported encoding
  ASSERT_THROW(reader_->HasNext(), ParquetException);
  pages_.clear();
}

using TestBooleanReader = TestPrimitiveReader<BooleanType>;

TEST_F(TestBooleanReader, TestBooleanNestedOptionalSkip) {
  // Use large page size to make TypedColumnReader::Skip max out its batch size.
  int levels_per_page = 4000;
  int num_pages = 5;
  max_def_level_ = 1;
  max_rep_level_ = 0;
  NodePtr type = schema::Boolean("a", Repetition::OPTIONAL);
  const ColumnDescriptor descr(type, max_def_level_, max_rep_level_);

  MakePages<BooleanType>(&descr, num_pages, levels_per_page, def_levels_, rep_levels_,
                         values_, data_buffer_, pages_, Encoding::PLAIN);
  InitReader(&descr);
  std::vector<uint8_t> vresult(levels_per_page / 2, -1);
  bool* vresult_ptr = reinterpret_cast<bool*>(vresult.data());
  std::vector<int16_t> dresult(levels_per_page / 2, -1);
  std::vector<int16_t> rresult(levels_per_page / 2, -1);

  BoolReader* reader = static_cast<BoolReader*>(reader_.get());
  int64_t values_read = 0;
  int64_t levels_processed = 0;
  std::vector<bool>::iterator values_it = values_.begin();

  // Skip full page
  int64_t levels_skipped = reader->Skip(levels_per_page);
  ASSERT_EQ(levels_per_page, levels_skipped);
  for (int i = 0; i < levels_skipped; i++) {
    if (def_levels_[i] == max_def_level_) {
      values_it++;
    }
  }
  levels_processed += levels_skipped;
  // Read half a page
  int64_t levels_read = reader->ReadBatch(levels_per_page / 2, dresult.data(),
                                          rresult.data(), vresult_ptr, &values_read);
  std::vector<bool> vresult_bool(vresult.begin(), vresult.end());
  for (int i = levels_processed, j = 0; i < levels_processed + levels_read; i++) {
    if (def_levels_[i] == max_def_level_) {
      ASSERT_EQ(*(values_it++), vresult[j++]);
    }
  }
  levels_processed += levels_read;

  // Skip across two pages
  levels_skipped = reader->Skip(levels_per_page * 5 / 4);
  ASSERT_EQ(levels_per_page * 5 / 4, levels_skipped);
  for (int i = levels_processed; i < levels_processed + levels_skipped; i++) {
    if (def_levels_[i] == max_def_level_) {
      values_it++;
    }
  }
  levels_processed += levels_skipped;
  // Read half a page
  levels_read = reader->ReadBatch(levels_per_page / 2, dresult.data(), rresult.data(),
                                  vresult_ptr, &values_read);
  vresult_bool.assign(vresult.begin(), vresult.end());
  for (int i = levels_processed, j = 0; i < levels_processed + levels_read; i++) {
    if (def_levels_[i] == max_def_level_) {
      ASSERT_EQ(*(values_it++), vresult[j++]);
    }
  }
  levels_processed += levels_read;

  // Skip within one page
  levels_skipped = reader->Skip(levels_per_page / 8);
  ASSERT_EQ(levels_per_page / 8, levels_skipped);
  for (int i = levels_processed; i < levels_processed + levels_skipped; i++) {
    if (def_levels_[i] == max_def_level_) {
      values_it++;
    }
  }
  levels_processed += levels_skipped;
  // Read half a page
  levels_read = reader->ReadBatch(levels_per_page / 2, dresult.data(), rresult.data(),
                                  vresult_ptr, &values_read);
  vresult_bool.assign(vresult.begin(), vresult.end());
  for (int i = levels_processed, j = 0; i < levels_processed + levels_read; i++) {
    if (def_levels_[i] == max_def_level_) {
      ASSERT_EQ(*(values_it++), vresult[j++]);
    }
  }
}

}  // namespace test
}  // namespace parquet
