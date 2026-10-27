/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef OCEANBASE_OBSERVER_VIRTUAL_TABLE_OB_ALL_AI_COLUMN_H_
#define OCEANBASE_OBSERVER_VIRTUAL_TABLE_OB_ALL_AI_COLUMN_H_

#include "observer/virtual_table/ob_virtual_table_scanner_iterator.h"
#include "sql/engine/dml/ob_ai_column_runtime.h"

namespace oceanbase
{
namespace observer
{

class ObAllAIColumn : public common::ObVirtualTableScannerIterator
{
public:
  ObAllAIColumn();
  virtual ~ObAllAIColumn();
  virtual int inner_get_next_row(common::ObNewRow *&row) override;
  virtual void reset() override;

private:
  enum COLUMN_ID_LIST
  {
    COLUMN_NAME = common::OB_APP_MIN_COLUMN_ID,
    MAX_RETRIES,
    COLUMN_ID,
    STATE_COLUMN_ID,
    REQUEST_COLUMN_ID,
    RESULT_SPEC_COLUMN_ID,
    NORMALIZED_EXPR,
    TABLE_ID
  };

  bool loaded_;
  int64_t row_idx_;
  common::ObSEArray<sql::ObAIColumnMetaRow, 4> rows_;

  DISALLOW_COPY_AND_ASSIGN(ObAllAIColumn);
};

}
}

#endif
