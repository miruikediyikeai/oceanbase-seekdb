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

#define USING_LOG_PREFIX SERVER

#include "observer/virtual_table/ob_all_ai_column.h"
#include "lib/charset/ob_charset.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase
{
namespace observer
{

ObAllAIColumn::ObAllAIColumn()
  : ObVirtualTableScannerIterator(),
    loaded_(false),
    row_idx_(0)
{
}

ObAllAIColumn::~ObAllAIColumn()
{
  reset();
}

void ObAllAIColumn::reset()
{
  loaded_ = false;
  row_idx_ = 0;
  rows_.reset();
  ObVirtualTableScannerIterator::reset();
}

int ObAllAIColumn::inner_get_next_row(ObNewRow *&row)
{
  int ret = OB_SUCCESS;
  if (!loaded_) {
    if (OB_FAIL(ObAIColumnRuntime::collect_meta(schema_guard_, rows_))) {
      SERVER_LOG(WARN, "collect ai column metadata failed", K(ret));
    } else {
      loaded_ = true;
      row_idx_ = 0;
    }
  }
  if (OB_FAIL(ret)) {
  } else if (row_idx_ >= rows_.count()) {
    ret = OB_ITER_END;
  } else if (OB_ISNULL(cur_row_.cells_)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    const ObAIColumnMetaRow &meta = rows_.at(row_idx_);
    const int64_t col_count = output_column_ids_.count();
    ObObj *cells = cur_row_.cells_;
    const ObCollationType collation = ObCharset::get_default_collation(ObCharset::get_default_charset());
    for (int64_t i = 0; OB_SUCC(ret) && i < col_count; ++i) {
      switch (output_column_ids_.at(i)) {
        case COLUMN_NAME: {
          cells[i].set_varchar(meta.column_name_);
          cells[i].set_collation_type(collation);
          break;
        }
        case MAX_RETRIES: {
          cells[i].set_int(meta.max_retries_);
          break;
        }
        case COLUMN_ID: {
          cells[i].set_int(meta.column_id_);
          break;
        }
        case STATE_COLUMN_ID: {
          cells[i].set_int(meta.state_column_id_);
          break;
        }
        case REQUEST_COLUMN_ID: {
          cells[i].set_int(meta.request_column_id_);
          break;
        }
        case RESULT_SPEC_COLUMN_ID: {
          cells[i].set_int(meta.spec_column_id_);
          break;
        }
        case NORMALIZED_EXPR: {
          cells[i].set_varchar(meta.normalized_expr_);
          cells[i].set_collation_type(collation);
          break;
        }
        case TABLE_ID: {
          cells[i].set_int(meta.table_id_);
          break;
        }
        default: {
          ret = OB_ERR_UNEXPECTED;
          SERVER_LOG(WARN, "unexpected ai column metadata id", K(ret), K(output_column_ids_.at(i)));
          break;
        }
      }
    }
    if (OB_SUCC(ret)) {
      row = &cur_row_;
      ++row_idx_;
    }
  }
  return ret;
}

}
}
