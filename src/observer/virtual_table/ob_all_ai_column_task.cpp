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

#include "observer/virtual_table/ob_all_ai_column_task.h"
#include "lib/charset/ob_charset.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase
{
namespace observer
{

ObAllAIColumnTask::ObAllAIColumnTask()
  : ObVirtualTableScannerIterator(),
    loaded_(false),
    row_idx_(0)
{
}

ObAllAIColumnTask::~ObAllAIColumnTask()
{
  reset();
}

void ObAllAIColumnTask::reset()
{
  loaded_ = false;
  row_idx_ = 0;
  rows_.reset();
  ObVirtualTableScannerIterator::reset();
}

int ObAllAIColumnTask::inner_get_next_row(ObNewRow *&row)
{
  int ret = OB_SUCCESS;
  if (!loaded_) {
    uint64_t session_id = 0;
    if (OB_NOT_NULL(session_)) {
      session_id = session_->get_server_sid();
    }
    if (OB_FAIL(ObAIColumnRuntime::collect_tasks(session_id, rows_))) {
      SERVER_LOG(WARN, "collect ai column tasks failed", K(ret));
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
    const ObAIColumnTaskView &task = rows_.at(row_idx_);
    const int64_t col_count = output_column_ids_.count();
    ObObj *cells = cur_row_.cells_;
    const ObCollationType collation = ObCharset::get_default_collation(ObCharset::get_default_charset());
    for (int64_t i = 0; OB_SUCC(ret) && i < col_count; ++i) {
      switch (output_column_ids_.at(i)) {
        case TABLE_ID: {
          cells[i].set_int(task.table_id_);
          break;
        }
        case COLUMN_ID: {
          cells[i].set_int(task.column_id_);
          break;
        }
        case REQUEST_ID: {
          cells[i].set_int(task.request_id_);
          break;
        }
        case ROWKEY_PREDICATE: {
          cells[i].set_varchar(task.predicate_);
          cells[i].set_collation_type(collation);
          break;
        }
        case ELIGIBLE_TS: {
          cells[i].set_timestamp(task.eligible_ts_ > 0 ? task.eligible_ts_ : 1);
          break;
        }
        case STATE: {
          cells[i].set_int(task.state_);
          break;
        }
        case RETRY_COUNT: {
          cells[i].set_int(task.retry_count_);
          break;
        }
        default: {
          ret = OB_ERR_UNEXPECTED;
          SERVER_LOG(WARN, "unexpected ai column task id", K(ret), K(output_column_ids_.at(i)));
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
