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

#ifndef OCEANBASE_SQL_ENGINE_DML_OB_AI_COLUMN_RUNTIME_H_
#define OCEANBASE_SQL_ENGINE_DML_OB_AI_COLUMN_RUNTIME_H_

#include "lib/container/ob_iarray.h"
#include "sql/parser/parse_node.h"

namespace oceanbase
{
namespace share
{
namespace schema
{
class ObTableSchema;
class ObColumnSchemaV2;
class AlterTableSchema;
class ObSchemaGetterGuard;
}
}
namespace sql
{
class ObExecContext;
class ObDMLBaseCtDef;
class ObEvalCtx;
class ObSQLSessionInfo;

struct ObAIColumnMetaRow
{
  char column_name_[128];
  int64_t max_retries_;
  int64_t column_id_;
  int64_t state_column_id_;
  int64_t request_column_id_;
  int64_t spec_column_id_;
  char normalized_expr_[4096];
  int64_t table_id_;
};

struct ObAIColumnTaskView
{
  int64_t table_id_;
  int64_t column_id_;
  int64_t request_id_;
  char predicate_[512];
  int64_t eligible_ts_;
  int64_t state_;
  int64_t retry_count_;
};

class ObAIColumnRuntime
{
public:
  static const int64_t AI_COLUMN_RETRY_MARK = 4922;

  static bool is_ai_comment(const char *comment);
  static bool comment_depends_on(const char *comment, const common::ObString &column_name);
  static int hidden_name(char *buf, int64_t cap, const char *kind, const char *base_name);
  static int resolve_clause(const share::schema::ObTableSchema &table,
                            share::schema::ObColumnSchemaV2 &column,
                            ParseNode *clause,
                            ParseNode *type_node);
  static int add_create_hidden(share::schema::ObTableSchema &schema,
                               const char *ai_name,
                               uint64_t state_id,
                               uint64_t request_id,
                               uint64_t spec_id);
  static int add_alter_hidden(share::schema::AlterTableSchema &schema, const char *ai_name);
  static int on_insert(ObExecContext &ctx, const ObDMLBaseCtDef &ctdef, ObEvalCtx &eval_ctx);
  static int on_update(ObExecContext &ctx, const ObDMLBaseCtDef &ctdef, ObEvalCtx &eval_ctx);
  static int on_delete(ObExecContext &ctx, const ObDMLBaseCtDef &ctdef, ObEvalCtx &eval_ctx);
  static void on_end_trans(ObSQLSessionInfo *session, bool commit_ok);
  static void on_stmt_rollback(ObSQLSessionInfo *session);
  static int check_strict(ObExecContext &ctx,
                          uint64_t table_id,
                          const common::ObIArray<uint64_t> &column_ids);
  static int collect_meta(share::schema::ObSchemaGetterGuard *guard,
                          common::ObIArray<ObAIColumnMetaRow> &rows);
  static int collect_tasks(uint64_t session_id, common::ObIArray<ObAIColumnTaskView> &rows);
};
}
}

#endif
