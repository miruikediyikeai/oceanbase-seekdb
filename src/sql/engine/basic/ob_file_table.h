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

#ifndef OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_TABLE_H_
#define OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_TABLE_H_

#include <string.h>
#include "common/ob_accuracy.h"
#include "common/object/ob_object.h"
#include "lib/container/ob_iarray.h"
#include "lib/string/ob_string.h"

struct ParseNode;

namespace oceanbase
{
namespace sql
{
class ObSQLSessionInfo;

enum ObFileColType
{
  OB_FILE_VARCHAR = 0,
  OB_FILE_BIGINT = 1,
  OB_FILE_DOUBLE = 2,
  OB_FILE_BOOL = 3,
  OB_FILE_DATE = 4,
  OB_FILE_DATETIME = 5
};

struct ObFileTableCol
{
  common::ObString name_;
  int type_;
  bool nullable_;
  ObFileTableCol() : name_(), type_(OB_FILE_VARCHAR), nullable_(true) {}
};

inline bool ob_is_file_table_payload(const common::ObString &s)
{
  return s.length() >= 7 && NULL != s.ptr() && 0 == memcmp(s.ptr(), "OBFILE1", 7);
}

int ob_file_table_resolve_node(common::ObIAllocator &alloc,
                               ObSQLSessionInfo *session,
                               const ParseNode *node,
                               common::ObIArray<ObFileTableCol> &cols,
                               common::ObString &payload);
int ob_file_table_column_names(const common::ObString &payload,
                               common::ObIAllocator &alloc,
                               common::ObIArray<common::ObString> &names);
int ob_file_table_fill_meta(int type, common::ObObjMeta &meta, common::ObAccuracy &accuracy);
int ob_file_table_build_describe_sql(common::ObIAllocator &alloc,
                                     const common::ObIArray<ObFileTableCol> &cols,
                                     common::ObString &sql);

class ObFileTableScan
{
public:
  ObFileTableScan();
  ~ObFileTableScan();
  int open(common::ObIAllocator &alloc, const common::ObString &payload);
  int next();
  int get_cell(int64_t idx, common::ObObj &cell);
  int rewind();
  void close();
private:
  struct Impl;
  Impl *impl_;
};

} // namespace sql
} // namespace oceanbase

#endif
