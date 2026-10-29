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

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/dml/ob_ai_column_runtime.h"
#include "sql/engine/dml/ob_dml_ctx_define.h"
#include "lib/allocator/page_arena.h"
#include "lib/charset/ob_charset.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/time/ob_time_utility.h"
#include "lib/utility/utility.h"
#include "lib/worker.h"
#include "lib/string/ob_sql_string.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_schema_struct.h"
#include "share/schema/ob_schema_service.h"
#include "share/schema/ob_table_schema.h"
#include "share/ob_errno.h"
#include "share/ob_server_struct.h"
#include "share/rc/ob_server_runtime.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/expr/ob_expr.h"
#include "sql/session/ob_sql_session_info.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_client.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include <pthread.h>

namespace oceanbase
{
using namespace common;
namespace sql
{
namespace
{
const char *AI_COMMENT_MAGIC = "OBAC1";
const char *JSON_PROMPT = "Return exactly one valid JSON value";

bool eq_ci(const char *text, int64_t len, const char *literal)
{
  int64_t i = 0;
  if (OB_ISNULL(text) || OB_ISNULL(literal)) {
    return false;
  }
  for (; i < len && literal[i] != '\0'; ++i) {
    char left = text[i];
    char right = literal[i];
    if (left >= 'A' && left <= 'Z') {
      left = static_cast<char>(left - 'A' + 'a');
    }
    if (right >= 'A' && right <= 'Z') {
      right = static_cast<char>(right - 'A' + 'a');
    }
    if (left != right) {
      return false;
    }
  }
  return i == len && literal[i] == '\0';
}

bool node_eq_ci(const ParseNode *node, const char *literal)
{
  return OB_NOT_NULL(node) && OB_NOT_NULL(node->str_value_)
         && eq_ci(node->str_value_, node->str_len_, literal);
}

int nth_line(const char *comment, int nth, const char *&start, int64_t &len)
{
  int ret = OB_SUCCESS;
  const char *cursor = comment;
  int seen = 0;
  start = NULL;
  len = 0;
  if (OB_ISNULL(comment)) {
    ret = OB_INVALID_ARGUMENT;
  }
  while (OB_SUCC(ret) && seen <= nth) {
    const char *end = cursor;
    while (*end != '\0' && *end != '\n') {
      ++end;
    }
    if (seen == nth) {
      start = cursor;
      len = end - cursor;
      break;
    }
    if (*end == '\0') {
      ret = OB_INVALID_ARGUMENT;
    } else {
      cursor = end + 1;
      ++seen;
    }
  }
  return ret;
}

int append_text(char *buf, int64_t cap, int64_t &used, const char *text, int64_t len)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(buf) || used < 0 || len < 0 || used + len >= cap) {
    ret = OB_SIZE_OVERFLOW;
  } else {
    if (len > 0 && OB_NOT_NULL(text)) {
      MEMCPY(buf + used, text, len);
    }
    used += len;
    buf[used] = '\0';
  }
  return ret;
}

int append_dep(char *deps, int64_t cap, int64_t &used, const ObString &name)
{
  int ret = OB_SUCCESS;
  if (used > 0 && OB_FAIL(append_text(deps, cap, used, ",", 1))) {
  } else if (OB_FAIL(append_text(deps, cap, used, name.ptr(), name.length()))) {
  }
  return ret;
}

int walk_refs(const share::schema::ObTableSchema &table,
              const ParseNode *node,
              char *deps,
              int64_t cap,
              int64_t &used)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(node)) {
  } else if (T_COLUMN_REF == node->type_ && node->num_child_ >= 3
             && OB_NOT_NULL(node->children_[2])
             && OB_NOT_NULL(node->children_[2]->str_value_)) {
    ObString name(static_cast<int32_t>(node->children_[2]->str_len_), node->children_[2]->str_value_);
    const share::schema::ObColumnSchemaV2 *col = table.get_column_schema(name);
    bool seen = false;
    const char *exist = deps;
    if (OB_NOT_NULL(col) && ObAIColumnRuntime::is_ai_comment(col->get_comment())) {
      ret = OB_NOT_SUPPORTED;
    }
    while (OB_SUCC(ret) && OB_NOT_NULL(exist) && *exist != '\0' && !seen) {
      const char *comma = exist;
      while (*comma != '\0' && *comma != ',') {
        ++comma;
      }
      if ((comma - exist) == name.length() && 0 == MEMCMP(exist, name.ptr(), name.length())) {
        seen = true;
      }
      exist = *comma == ',' ? comma + 1 : comma;
    }
    if (OB_SUCC(ret) && !seen && OB_FAIL(append_dep(deps, cap, used, name))) {
    }
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < node->num_child_; ++i) {
      if (OB_FAIL(walk_refs(table, node->children_[i], deps, cap, used))) {
      }
    }
  }
  return ret;
}

int extract_call_text(const ParseNode *call, bool strip_dim, char *model, int64_t model_cap,
                      char *expr, int64_t expr_cap)
{
  int ret = OB_SUCCESS;
  const char *text = NULL;
  int64_t len = 0;
  const char *open = NULL;
  const char *close = NULL;
  char quote = 0;
  if (OB_ISNULL(call) || OB_ISNULL(call->str_value_) || call->str_len_ <= 0
      || OB_ISNULL(model) || OB_ISNULL(expr) || model_cap <= 1 || expr_cap <= 1) {
    ret = OB_ERR_PARSER_SYNTAX;
  } else {
    text = call->str_value_;
    len = call->str_len_;
    for (int64_t i = 0; i < len && NULL == open; ++i) {
      if (text[i] == '\'' || text[i] == '"') {
        quote = text[i];
        open = text + i + 1;
      }
    }
    if (NULL == open) {
      ret = OB_ERR_PARSER_SYNTAX;
    }
    for (const char *cursor = open; OB_SUCC(ret) && cursor < text + len && NULL == close; ++cursor) {
      if (*cursor == quote) {
        close = cursor;
      }
    }
    if (OB_SUCC(ret) && NULL == close) {
      ret = OB_ERR_PARSER_SYNTAX;
    }
  }
  if (OB_SUCC(ret)) {
    int64_t model_len = close - open;
    const char *body = close + 1;
    const char *end = text + len;
    while (body < end && (*body == ' ' || *body == '\t')) {
      ++body;
    }
    if (body < end && *body == ',') {
      ++body;
    }
    while (body < end && (*body == ' ' || *body == '\t')) {
      ++body;
    }
    while (end > body && (end[-1] == ' ' || end[-1] == '\n' || end[-1] == '\t')) {
      --end;
    }
    if (end > body && end[-1] == ')') {
      --end;
    }
    while (end > body && (end[-1] == ' ' || end[-1] == '\t')) {
      --end;
    }
    if (strip_dim) {
      const char *comma = NULL;
      for (const char *cursor = body; cursor < end; ++cursor) {
        if (*cursor == ',') {
          comma = cursor;
        }
      }
      if (NULL != comma) {
        const char *num = comma + 1;
        bool digits = false;
        while (num < end && (*num == ' ' || *num == '\t')) {
          ++num;
        }
        const char *scan = num;
        while (scan < end && *scan >= '0' && *scan <= '9') {
          digits = true;
          ++scan;
        }
        if (digits && scan == end) {
          end = comma;
          while (end > body && (end[-1] == ' ' || end[-1] == '\t')) {
            --end;
          }
        }
      }
    }
    if (model_len <= 0 || model_len >= model_cap || end < body || (end - body) >= expr_cap) {
      ret = OB_ERR_PARSER_SYNTAX;
    } else {
      MEMCPY(model, open, model_len);
      model[model_len] = '\0';
      MEMCPY(expr, body, end - body);
      expr[end - body] = '\0';
    }
  }
  return ret;
}

int read_options(const ParseNode *opts, int64_t &max_retries)
{
  int ret = OB_SUCCESS;
  max_retries = 0;
  if (OB_ISNULL(opts)) {
    // WITH (...) is optional: without it, defaults apply.
    return OB_SUCCESS;
  } else if (T_ASSIGN_ITEM == opts->type_) {
    ret = OB_ERR_UNEXPECTED;
  } else if (T_EXPR_LIST != opts->type_) {
    ret = OB_ERR_PARSER_SYNTAX;
  }
  for (int64_t i = 0; OB_SUCC(ret) && OB_NOT_NULL(opts) && i < opts->num_child_; ++i) {
    const ParseNode *item = opts->children_[i];
    if (OB_ISNULL(item) || T_ASSIGN_ITEM != item->type_ || item->num_child_ < 2
        || OB_ISNULL(item->children_[0]) || OB_ISNULL(item->children_[1])) {
      ret = OB_ERR_PARSER_SYNTAX;
    } else if (node_eq_ci(item->children_[0], "max_retries")) {
      max_retries = item->children_[1]->value_;
    } else if (node_eq_ci(item->children_[0], "sync_mode")) {
      if (!node_eq_ci(item->children_[1], "async")) {
        ret = OB_NOT_SUPPORTED;
      }
    }
  }
  return ret;
}

int fill_hidden_common(share::schema::ObColumnSchemaV2 &column, const char *name, uint64_t column_id)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(column.set_column_name(name))) {
  } else {
    column.set_nullable(true);
    column.set_is_hidden(true);
    if (OB_INVALID_ID != column_id) {
      column.set_column_id(column_id);
    }
  }
  return ret;
}

int make_hidden(share::schema::ObColumnSchemaV2 &column, const char *kind, const char *ai_name,
                uint64_t column_id, bool spec)
{
  int ret = OB_SUCCESS;
  char name[128];
  if (OB_FAIL(ObAIColumnRuntime::hidden_name(name, sizeof(name), kind, ai_name))) {
  } else if (OB_FAIL(fill_hidden_common(column, name, column_id))) {
  } else if (spec) {
    column.set_data_type(ObVarcharType);
    column.set_data_length(256);
    column.set_charset_type(CHARSET_UTF8MB4);
    column.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  } else {
    column.set_data_type(ObIntType);
  }
  return ret;
}
}

bool ObAIColumnRuntime::is_ai_comment(const char *comment)
{
  return OB_NOT_NULL(comment) && 0 == STRNCMP(comment, AI_COMMENT_MAGIC, STRLEN(AI_COMMENT_MAGIC))
         && (comment[STRLEN(AI_COMMENT_MAGIC)] == '\n' || comment[STRLEN(AI_COMMENT_MAGIC)] == '\0');
}

bool ObAIColumnRuntime::comment_depends_on(const char *comment, const ObString &column_name)
{
  bool depend = false;
  const char *deps = NULL;
  int64_t len = 0;
  if (is_ai_comment(comment) && column_name.length() > 0
      && OB_SUCCESS == nth_line(comment, 7, deps, len) && OB_NOT_NULL(deps)) {
    const char *cursor = deps;
    const char *end = deps + len;
    while (cursor < end && !depend) {
      const char *comma = cursor;
      while (comma < end && *comma != ',') {
        ++comma;
      }
      if ((comma - cursor) == column_name.length()
          && 0 == MEMCMP(cursor, column_name.ptr(), column_name.length())) {
        depend = true;
      }
      cursor = comma < end ? comma + 1 : end;
    }
  }
  return depend;
}

int ObAIColumnRuntime::hidden_name(char *buf, int64_t cap, const char *kind, const char *base_name)
{
  int ret = OB_SUCCESS;
  int written = 0;
  if (OB_ISNULL(buf) || cap <= 1 || OB_ISNULL(kind) || OB_ISNULL(base_name)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    written = snprintf(buf, cap, "__ob_ai_%s_%s", kind, base_name);
    if (written <= 0 || written >= cap) {
      ret = OB_SIZE_OVERFLOW;
    }
  }
  return ret;
}

int ObAIColumnRuntime::resolve_clause(const share::schema::ObTableSchema &table,
                                      share::schema::ObColumnSchemaV2 &column,
                                      ParseNode *clause,
                                      ParseNode *type_node)
{
  int ret = OB_SUCCESS;
  int64_t max_retries = 0;
  int64_t declared_dim = 0;
  int64_t literal_dim = 0;
  bool is_embed = false;
  bool has_literal_dim = false;
  bool is_vector = false;
  char kind = 'v';
  char model[128];
  char user_expr[2048];
  char deps[1024];
  char comment[4096];
  int64_t dep_used = 0;
  ParseNode *call = NULL;
  deps[0] = '\0';
  model[0] = '\0';
  user_expr[0] = '\0';
  if (OB_ISNULL(clause) || T_AI_COLUMN_CLAUSE != clause->type_ || clause->num_child_ < 2) {
    ret = OB_ERR_PARSER_SYNTAX;
  } else if (FALSE_IT(call = clause->children_[0])) {
  } else if (OB_FAIL(read_options(clause->children_[1], max_retries))) {
  } else if (OB_FAIL(walk_refs(table, call, deps, sizeof(deps), dep_used))) {
  } else if (OB_NOT_NULL(call) && T_FUN_SYS == call->type_ && call->num_child_ >= 1
             && node_eq_ci(call->children_[0], "ai_embed")) {
    is_embed = true;
  }
  if (OB_SUCC(ret) && is_embed && OB_NOT_NULL(call) && call->num_child_ >= 2
      && OB_NOT_NULL(call->children_[1]) && T_EXPR_LIST == call->children_[1]->type_
      && call->children_[1]->num_child_ >= 3
      && OB_NOT_NULL(call->children_[1]->children_[2])
      && T_INT == call->children_[1]->children_[2]->type_) {
    has_literal_dim = true;
    literal_dim = call->children_[1]->children_[2]->value_;
  }
  if (OB_SUCC(ret) && OB_NOT_NULL(type_node) && T_COLLECTION == type_node->type_
      && 1 == type_node->int32_values_[0] && type_node->num_child_ >= 1
      && OB_NOT_NULL(type_node->children_[0])) {
    is_vector = true;
    declared_dim = type_node->children_[0]->value_;
  }
  if (OB_SUCC(ret) && is_vector && has_literal_dim && declared_dim != literal_dim) {
    ret = OB_ERR_AI_COLUMN_DIMENSION;
    LOG_USER_ERROR(OB_ERR_AI_COLUMN_DIMENSION, declared_dim, literal_dim);
  }
  if (OB_SUCC(ret) && OB_FAIL(extract_call_text(call, has_literal_dim, model, sizeof(model),
                                                user_expr, sizeof(user_expr)))) {
  }
  if (OB_SUCC(ret)) {
    if (ObJsonType == column.get_data_type()) {
      kind = 'j';
    } else if (column.is_collection() || is_vector) {
      kind = 'e';
    } else if (ob_is_int_tc(column.get_data_type())) {
      kind = 'i';
    }
    int written = snprintf(comment, sizeof(comment), "OBAC1\n%ld\n%c\n%d\n%ld\n%s\n%s\n%s\n",
                           max_retries, kind, is_embed ? 1 : 0, is_vector ? declared_dim : 0,
                           model, user_expr, deps);
    if (written <= 0 || written >= static_cast<int>(sizeof(comment))) {
      ret = OB_SIZE_OVERFLOW;
    } else if (OB_FAIL(column.set_comment(comment))) {
    }
  }
  return ret;
}

int ObAIColumnRuntime::add_create_hidden(share::schema::ObTableSchema &schema,
                                         const char *ai_name,
                                         uint64_t state_id,
                                         uint64_t request_id,
                                         uint64_t spec_id)
{
  int ret = OB_SUCCESS;
  share::schema::ObColumnSchemaV2 state_col;
  share::schema::ObColumnSchemaV2 request_col;
  share::schema::ObColumnSchemaV2 spec_col;
  if (OB_FAIL(make_hidden(state_col, "st", ai_name, state_id, false))) {
  } else if (OB_FAIL(make_hidden(request_col, "rq", ai_name, request_id, false))) {
  } else if (OB_FAIL(make_hidden(spec_col, "sp", ai_name, spec_id, true))) {
  } else if (OB_FAIL(schema.add_column(state_col))) {
  } else if (OB_FAIL(schema.add_column(request_col))) {
  } else if (OB_FAIL(schema.add_column(spec_col))) {
  }
  return ret;
}

int ObAIColumnRuntime::add_alter_hidden(share::schema::AlterTableSchema &schema, const char *ai_name)
{
  int ret = OB_SUCCESS;
  share::schema::AlterColumnSchema state_col;
  share::schema::AlterColumnSchema request_col;
  share::schema::AlterColumnSchema spec_col;
  state_col.alter_type_ = share::schema::OB_DDL_ADD_COLUMN;
  request_col.alter_type_ = share::schema::OB_DDL_ADD_COLUMN;
  spec_col.alter_type_ = share::schema::OB_DDL_ADD_COLUMN;
  if (OB_FAIL(make_hidden(state_col, "st", ai_name, OB_INVALID_ID, false))) {
  } else if (OB_FAIL(make_hidden(request_col, "rq", ai_name, OB_INVALID_ID, false))) {
  } else if (OB_FAIL(make_hidden(spec_col, "sp", ai_name, OB_INVALID_ID, true))) {
  } else if (OB_FAIL(schema.add_alter_column(state_col, true))) {
  } else if (OB_FAIL(schema.add_alter_column(request_col, true))) {
  } else if (OB_FAIL(schema.add_alter_column(spec_col, true))) {
  }
  return ret;
}

int ObAIColumnRuntime::collect_meta(share::schema::ObSchemaGetterGuard *guard,
                                    common::ObIArray<ObAIColumnMetaRow> &rows)
{
  int ret = OB_SUCCESS;
  ObSEArray<const share::schema::ObTableSchema *, 64> tables;
  rows.reset();
  if (OB_ISNULL(guard)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(guard->get_table_schemas_in_runtime(tables))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < tables.count(); ++i) {
    const share::schema::ObTableSchema *table = tables.at(i);
    if (OB_ISNULL(table) || !table->is_user_table()) {
      continue;
    }
    for (int64_t c = 0; OB_SUCC(ret) && c < table->get_column_count(); ++c) {
      const share::schema::ObColumnSchemaV2 *column = table->get_column_schema_by_idx(c);
      ObAIColumnMetaRow row;
      const char *retries = NULL;
      const char *kind = NULL;
      const char *expr = NULL;
      int64_t retries_len = 0;
      int64_t kind_len = 0;
      int64_t expr_len = 0;
      MEMSET(&row, 0, sizeof(row));
      if (OB_ISNULL(column) || !is_ai_comment(column->get_comment())) {
        continue;
      } else if (OB_FAIL(nth_line(column->get_comment(), 1, retries, retries_len))) {
      } else if (OB_FAIL(nth_line(column->get_comment(), 2, kind, kind_len))) {
      } else if (OB_FAIL(nth_line(column->get_comment(), 6, expr, expr_len))) {
      } else {
        char state_name[128];
        char request_name[128];
        char spec_name[128];
        const share::schema::ObColumnSchemaV2 *state_col = NULL;
        const share::schema::ObColumnSchemaV2 *request_col = NULL;
        const share::schema::ObColumnSchemaV2 *spec_col = NULL;
        int64_t name_len = column->get_column_name_str().length();
        if (name_len >= static_cast<int64_t>(sizeof(row.column_name_))) {
          ret = OB_SIZE_OVERFLOW;
        } else if (OB_FAIL(hidden_name(state_name, sizeof(state_name), "st", column->get_column_name()))) {
        } else if (OB_FAIL(hidden_name(request_name, sizeof(request_name), "rq", column->get_column_name()))) {
        } else if (OB_FAIL(hidden_name(spec_name, sizeof(spec_name), "sp", column->get_column_name()))) {
        } else {
          MEMCPY(row.column_name_, column->get_column_name(), name_len);
          row.column_name_[name_len] = '\0';
          row.max_retries_ = retries_len > 0 ? atoi(retries) : 0;
          row.column_id_ = static_cast<int64_t>(column->get_column_id());
          row.table_id_ = static_cast<int64_t>(table->get_table_id());
          state_col = table->get_column_schema(ObString(state_name));
          request_col = table->get_column_schema(ObString(request_name));
          spec_col = table->get_column_schema(ObString(spec_name));
          row.state_column_id_ = OB_NOT_NULL(state_col) ? static_cast<int64_t>(state_col->get_column_id()) : 0;
          row.request_column_id_ = OB_NOT_NULL(request_col) ? static_cast<int64_t>(request_col->get_column_id()) : 0;
          row.spec_column_id_ = OB_NOT_NULL(spec_col) ? static_cast<int64_t>(spec_col->get_column_id()) : 0;
          if (expr_len >= static_cast<int64_t>(sizeof(row.normalized_expr_))) {
            ret = OB_SIZE_OVERFLOW;
          } else {
            MEMCPY(row.normalized_expr_, expr, expr_len);
            row.normalized_expr_[expr_len] = '\0';
            if (kind_len == 1 && kind[0] == 'j') {
              int written = snprintf(row.normalized_expr_ + expr_len,
                                     sizeof(row.normalized_expr_) - expr_len,
                                     " %s", JSON_PROMPT);
              if (written <= 0 || written >= static_cast<int>(sizeof(row.normalized_expr_) - expr_len)) {
                ret = OB_SIZE_OVERFLOW;
              }
            }
          }
          if (OB_SUCC(ret) && OB_FAIL(rows.push_back(row))) {
          }
        }
      }
    }
  }
  return ret;
}

namespace
{
struct AiTask
{
  bool used_;
  bool published_;
  bool removed_;
  uint64_t session_id_;
  int64_t table_id_;
  int64_t column_id_;
  int64_t request_id_;
  char predicate_[512];
  int64_t eligible_ts_;
  int64_t state_;
  int64_t retry_count_;
  int64_t max_retries_;
  char kind_;
  char model_[128];
  char user_expr_[1024];
  char db_name_[128];
  char table_name_[128];
  char column_name_[128];
  char request_name_[160];
};

const int64_t AI_TASK_CAP = 256;
AiTask g_tasks[AI_TASK_CAP];
pthread_mutex_t g_task_mu = PTHREAD_MUTEX_INITIALIZER;
int64_t g_request_seq = 1;
pthread_t g_worker;
int g_worker_started = 0;

void copy_n(char *dst, int64_t cap, const char *src, int64_t len)
{
  int64_t n = 0;
  if (OB_NOT_NULL(dst) && cap > 0) {
    if (len < 0) {
      len = 0;
    }
    if (len >= cap) {
      len = cap - 1;
    }
    if (len > 0 && OB_NOT_NULL(src)) {
      MEMCPY(dst, src, len);
      n = len;
    }
    dst[n] = '\0';
  }
}

void copy_c(char *dst, int64_t cap, const char *src)
{
  copy_n(dst, cap, src, OB_ISNULL(src) ? 0 : static_cast<int64_t>(STRLEN(src)));
}

ObExpr *find_expr(const ObDMLBaseCtDef &ctdef, uint64_t col_id, bool use_new)
{
  const ExprFixedArray &row = use_new ? ctdef.new_row_ : ctdef.old_row_;
  int64_t n = ctdef.column_ids_.count();
  ObExpr *expr = NULL;
  if (row.count() < n) {
    n = row.count();
  }
  for (int64_t i = 0; NULL == expr && i < n; ++i) {
    if (ctdef.column_ids_.at(i) == col_id) {
      expr = row.at(i);
    }
  }
  return expr;
}

int build_predicate(const share::schema::ObTableSchema *schema, const ObDMLBaseCtDef &ctdef,
                    ObEvalCtx &eval_ctx, bool use_new, char *buf, int64_t cap)
{
  int ret = OB_SUCCESS;
  int64_t used = 0;
  const common::ObRowkeyInfo &rk = schema->get_rowkey_info();
  buf[0] = '\0';
  for (int64_t i = 0; OB_SUCC(ret) && i < rk.get_size(); ++i) {
    uint64_t col_id = OB_INVALID_ID;
    const share::schema::ObColumnSchemaV2 *col = NULL;
    ObExpr *expr = NULL;
    char piece[192];
    int n = 0;
    if (OB_FAIL(rk.get_column_id(i, col_id))) {
    } else if (OB_ISNULL(col = schema->get_column_schema(col_id))
               || OB_ISNULL(expr = find_expr(ctdef, col_id, use_new))) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      ObDatum &datum = expr->locate_expr_datum(eval_ctx);
      if (datum.is_null()) {
        n = snprintf(piece, sizeof(piece), "%s`%s` IS NULL", used > 0 ? " AND " : "", col->get_column_name());
      } else {
        n = snprintf(piece, sizeof(piece), "%s`%s` = %ld", used > 0 ? " AND " : "",
                     col->get_column_name(), static_cast<long>(datum.get_int()));
      }
      if (n <= 0 || used + n >= cap) {
        ret = OB_SIZE_OVERFLOW;
      } else {
        MEMCPY(buf + used, piece, n);
        used += n;
        buf[used] = '\0';
      }
    }
  }
  return ret;
}

void erase_match(int64_t table_id, int64_t column_id, const char *predicate, bool any_column)
{
  for (int64_t i = 0; i < AI_TASK_CAP; ++i) {
    AiTask &task = g_tasks[i];
    if (task.used_ && task.table_id_ == table_id
        && (any_column || task.column_id_ == column_id)
        && 0 == STRCMP(task.predicate_, predicate)) {
      task.used_ = false;
      task.removed_ = true;
    }
  }
}

int fill_task(AiTask &task, const share::schema::ObTableSchema *schema,
              const share::schema::ObDatabaseSchema *db, const share::schema::ObColumnSchemaV2 *column,
              uint64_t session_id, int64_t request_id, const char *predicate)
{
  int ret = OB_SUCCESS;
  const char *retries = NULL;
  const char *kind = NULL;
  const char *model = NULL;
  const char *expr = NULL;
  int64_t retries_len = 0;
  int64_t kind_len = 0;
  int64_t model_len = 0;
  int64_t expr_len = 0;
  MEMSET(&task, 0, sizeof(task));
  if (OB_FAIL(nth_line(column->get_comment(), 1, retries, retries_len))
      || OB_FAIL(nth_line(column->get_comment(), 2, kind, kind_len))
      || OB_FAIL(nth_line(column->get_comment(), 5, model, model_len))
      || OB_FAIL(nth_line(column->get_comment(), 6, expr, expr_len))) {
  } else if (OB_FAIL(ObAIColumnRuntime::hidden_name(task.request_name_, sizeof(task.request_name_),
                                                    "rq", column->get_column_name()))) {
  } else {
    task.used_ = true;
    task.published_ = false;
    task.session_id_ = session_id;
    task.table_id_ = static_cast<int64_t>(schema->get_table_id());
    task.column_id_ = static_cast<int64_t>(column->get_column_id());
    task.request_id_ = request_id;
    copy_c(task.predicate_, sizeof(task.predicate_), predicate);
    task.eligible_ts_ = ObTimeUtility::current_time();
    task.state_ = 0;
    task.retry_count_ = 0;
    task.max_retries_ = retries_len > 0 ? atoi(retries) : 0;
    task.kind_ = kind_len > 0 ? kind[0] : 'v';
    copy_n(task.model_, sizeof(task.model_), model, model_len);
    copy_n(task.user_expr_, sizeof(task.user_expr_), expr, expr_len);
    copy_c(task.db_name_, sizeof(task.db_name_), db->get_database_name());
    copy_c(task.table_name_, sizeof(task.table_name_), schema->get_table_name());
    copy_c(task.column_name_, sizeof(task.column_name_), column->get_column_name());
  }
  return ret;
}

int add_task(const AiTask &task)
{
  int ret = OB_SIZE_OVERFLOW;
  for (int64_t i = 0; i < AI_TASK_CAP; ++i) {
    if (!g_tasks[i].used_) {
      g_tasks[i] = task;
      ret = OB_SUCCESS;
      break;
    }
  }
  return ret;
}

void sql_literal(char kind, const char *printed, int64_t printed_len, char *out, int64_t cap)
{
  char stripped[4096];
  const char *text = printed;
  int64_t len = printed_len;
  int64_t w = 0;
  if (len >= 2 && printed[0] == '"' && printed[len - 1] == '"') {
    int64_t s = 0;
    for (int64_t i = 1; i + 1 < len && s + 1 < static_cast<int64_t>(sizeof(stripped)); ++i) {
      if (printed[i] == '\\' && i + 1 < len - 1) {
        stripped[s++] = printed[++i];
      } else {
        stripped[s++] = printed[i];
      }
    }
    stripped[s] = '\0';
    text = stripped;
    len = s;
  }
  if ('e' == kind) {
    // Embedding output is an array of vectors. A single row needs the inner
    // vector, otherwise the stored literal becomes [[..]] and fails vector parse.
    if (len >= 4 && text[0] == '[' && text[1] == '[' && text[len - 1] == ']' && text[len - 2] == ']') {
      bool nested_only = true;
      for (int64_t i = 2; i < len - 2 && nested_only; ++i) {
        if (']' == text[i] && i + 1 < len - 2 && '[' == text[i + 1]) {
          nested_only = false;
        }
      }
      if (nested_only) {
        ++text;
        len -= 2;
      }
    }
    for (int64_t i = 0; i < len && w + 1 < cap; ++i) {
      if (' ' == text[i]) {
        continue;
      }
      if ('.' == text[i]) {
        int64_t j = i + 1;
        bool zeros = true;
        bool saw = false;
        while (j < len && text[j] != ',' && text[j] != ']') {
          saw = true;
          if (text[j] != '0') {
            zeros = false;
          }
          ++j;
        }
        if (saw && zeros) {
          i = j - 1;
          continue;
        }
      }
      out[w++] = text[i];
    }
    out[w] = '\0';
  } else if ('i' == kind) {
    snprintf(out, cap, "%.*s", static_cast<int>(len), text);
  } else if (w + 3 < cap) {
    out[w++] = '\'';
    for (int64_t i = 0; i < len && w + 2 < cap; ++i) {
      if ('\'' == text[i]) {
        out[w++] = '\'';
      }
      out[w++] = text[i];
    }
    out[w++] = '\'';
    out[w] = '\0';
  } else {
    out[0] = '\0';
  }
}

int eval_user_expr(const AiTask &task, char *content, int64_t cap, int64_t &content_len)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  common::ObISQLClient::ReadResult result;
  content_len = 0;
  content[0] = '\0';
  if (OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(sql.append_fmt("SELECT %s FROM `%s`.`%s` WHERE %s",
                                    task.user_expr_, task.db_name_, task.table_name_, task.predicate_))) {
  } else if (OB_FAIL(GCTX.sql_proxy_->read(result, sql.ptr()))) {
  } else if (OB_ISNULL(result.get_result())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(result.get_result()->next())) {
  } else {
    ObString value;
    if (OB_FAIL(result.get_result()->get_varchar(static_cast<int64_t>(0), value))) {
    } else {
      content_len = value.length();
      if (content_len >= cap) {
        content_len = cap - 1;
      }
      if (content_len > 0 && OB_NOT_NULL(value.ptr())) {
        MEMCPY(content, value.ptr(), content_len);
      }
      content[content_len] = '\0';
    }
  }
  return ret;
}

int call_model(const AiTask &task, const char *content, int64_t content_len, char *sql_value, int64_t cap)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator arena;
  ObAIFuncExprInfo *info = NULL;
  query::ObIAiEndpointResolver *resolver = share::server_service<query::ObIAiEndpointResolver>();
  share::ObAiModelEndpointInfo endpoint;
  ObString model(task.model_);
  ObArray<ObString> headers;
  ObJsonObject *body = NULL;
  ObJsonObject *response = NULL;
  ObIJsonBase *parsed = NULL;
  ObString printed;
  char prompt_buf[80];
  ObString prompt;
  ObString content_str(static_cast<int32_t>(content_len), content);
  prompt_buf[0] = '\0';
  if (content_len <= 0) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(resolver)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(arena, model, info)) || OB_ISNULL(info)) {
  } else if (OB_FAIL(resolver->resolve_by_model_name(model, arena, endpoint))) {
  } else if (OB_FAIL(ObAIFuncUtils::get_header(arena, *info, endpoint, headers))) {
  } else {
    if ('j' == task.kind_) {
      copy_c(prompt_buf, sizeof(prompt_buf), "Return exactly one valid JSON value");
    } else if ('i' == task.kind_) {
      copy_c(prompt_buf, sizeof(prompt_buf), "Return a base-10 integer");
    }
    prompt.assign_ptr(prompt_buf, static_cast<int32_t>(STRLEN(prompt_buf)));
    if ('e' == task.kind_) {
      ObArray<ObString> contents;
      if (OB_FAIL(contents.push_back(content_str))) {
      } else if (OB_FAIL(ObAIFuncUtils::get_embed_body(arena, *info, endpoint, contents, NULL, body))) {
      }
    } else if (OB_FAIL(ObAIFuncUtils::get_complete_body(arena, *info, endpoint, prompt, content_str, NULL, body))) {
    }
  }
  if (OB_SUCC(ret)) {
    ObAIFuncClient client;
    if (OB_FAIL(client.send_post(arena, endpoint.get_url(), headers, body, response))) {
    } else if ('e' == task.kind_) {
      if (OB_FAIL(ObAIFuncUtils::parse_embed_output(arena, endpoint, response, parsed))) {
      }
    } else if (OB_FAIL(ObAIFuncUtils::parse_complete_output(arena, endpoint, response, parsed))) {
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(ObAIFuncJsonUtils::print_json_to_str(arena, parsed, printed))) {
  } else if (OB_SUCC(ret)) {
    sql_literal(task.kind_, printed.ptr(), printed.length(), sql_value, cap);
    if ('\0' == sql_value[0]) {
      ret = OB_ERR_UNEXPECTED;
    }
  }
  return ret;
}

bool finish_task(AiTask &task, int64_t slot, char *sql_value, int64_t cap)
{
  bool ok = false;
  char content[4096];
  int64_t content_len = 0;
  if (OB_SUCCESS != eval_user_expr(task, content, sizeof(content), content_len)) {
    content_len = 0;
  }
  while (true) {
    bool alive = false;
    int64_t retries = 0;
    int64_t max_retries = 0;
    pthread_mutex_lock(&g_task_mu);
    alive = g_tasks[slot].used_ && !g_tasks[slot].removed_ && g_tasks[slot].request_id_ == task.request_id_;
    retries = g_tasks[slot].retry_count_;
    max_retries = g_tasks[slot].max_retries_;
    pthread_mutex_unlock(&g_task_mu);
    if (!alive) {
      break;
    }
    if (OB_SUCCESS == call_model(task, content, content_len, sql_value, cap)) {
      ok = true;
      break;
    }
    if (retries >= max_retries) {
      pthread_mutex_lock(&g_task_mu);
      if (g_tasks[slot].request_id_ == task.request_id_) {
        g_tasks[slot].used_ = false;
        g_tasks[slot].removed_ = true;
      }
      pthread_mutex_unlock(&g_task_mu);
      break;
    }
    pthread_mutex_lock(&g_task_mu);
    if (g_tasks[slot].used_ && g_tasks[slot].request_id_ == task.request_id_) {
      g_tasks[slot].state_ = 3;
      g_tasks[slot].retry_count_ = retries + 1;
    }
    pthread_mutex_unlock(&g_task_mu);
    ob_usleep(350000);
  }
  return ok;
}

void write_group(AiTask *batch, int64_t *slots, bool *ok, char values[][2048], int64_t n)
{
  if (OB_ISNULL(GCTX.sql_proxy_) || n <= 0) {
    return;
  }
  // Each task targets one row. Combining different rows into a single UPDATE
  // makes the request id conditions contradictory, so write them one by one.
  for (int64_t i = 0; i < n; ++i) {
    ObSqlString sql;
    int64_t affected = 0;
    if (!ok[i]) {
      continue;
    }
    if (OB_SUCCESS != sql.append_fmt("UPDATE `%s`.`%s` SET `%s` = %s WHERE %s",
                                     batch[i].db_name_, batch[i].table_name_,
                                     batch[i].column_name_, values[i],
                                     batch[i].predicate_)) {
      continue;
    }
    int ret = GCTX.sql_proxy_->write(sql.ptr(), affected);
    LOG_INFO("ai column worker write back", K(ret), K(affected), K(sql));
    if (OB_SUCC(ret) && affected > 0) {
      pthread_mutex_lock(&g_task_mu);
      if (slots[i] >= 0 && g_tasks[slots[i]].request_id_ == batch[i].request_id_) {
        g_tasks[slots[i]].used_ = false;
        g_tasks[slots[i]].removed_ = true;
      }
      pthread_mutex_unlock(&g_task_mu);
    } else if (OB_SUCC(ret) && affected <= 0) {
      // The row is gone or already recomputed; drop the task either way.
      pthread_mutex_lock(&g_task_mu);
      if (slots[i] >= 0 && g_tasks[slots[i]].request_id_ == batch[i].request_id_) {
        g_tasks[slots[i]].used_ = false;
        g_tasks[slots[i]].removed_ = true;
      }
      pthread_mutex_unlock(&g_task_mu);
    } else {
      // A failed write must not spin forever; give up after one retry.
      bool drop = false;
      pthread_mutex_lock(&g_task_mu);
      if (slots[i] >= 0 && g_tasks[slots[i]].request_id_ == batch[i].request_id_) {
        if (++g_tasks[slots[i]].retry_count_ > 1) {
          g_tasks[slots[i]].used_ = false;
          g_tasks[slots[i]].removed_ = true;
          drop = true;
        }
      }
      pthread_mutex_unlock(&g_task_mu);
      LOG_WARN("ai column write back failed", K(ret), K(drop), K(sql));
    }
  }
}

void *ai_worker(void *)
{
  lib::Worker worker;
  lib::Worker::set_worker_to_thread_local(&worker);
  while (true) {
    AiTask batch[8];
    int64_t slots[8];
    bool ok[8];
    char values[8][2048];
    int64_t n = 0;
    int64_t request_id = 0;
    pthread_mutex_lock(&g_task_mu);
    for (int64_t i = 0; i < AI_TASK_CAP; ++i) {
      if (g_tasks[i].used_ && g_tasks[i].published_ && !g_tasks[i].removed_) {
        request_id = g_tasks[i].request_id_;
        break;
      }
    }
    if (0 != request_id) {
      for (int64_t i = 0; i < AI_TASK_CAP && n < 8; ++i) {
        if (g_tasks[i].used_ && g_tasks[i].published_ && !g_tasks[i].removed_
            && g_tasks[i].request_id_ == request_id) {
          batch[n] = g_tasks[i];
          slots[n] = i;
          ++n;
        }
      }
    }
    pthread_mutex_unlock(&g_task_mu);
    if (0 == n) {
      ob_usleep(20000);
      continue;
    }
    for (int64_t i = 0; i < n; ++i) {
      values[i][0] = '\0';
      ok[i] = finish_task(batch[i], slots[i], values[i], sizeof(values[i]));
    }
    write_group(batch, slots, ok, values, n);
  }
  return NULL;
}

void ensure_worker()
{
  pthread_mutex_lock(&g_task_mu);
  if (0 == g_worker_started) {
    g_worker_started = 1;
    if (0 != pthread_create(&g_worker, NULL, ai_worker, NULL)) {
      g_worker_started = 0;
    } else {
      pthread_detach(g_worker);
    }
  }
  pthread_mutex_unlock(&g_task_mu);
}

int ai_col_touch(ObExecContext &ctx, const ObDMLBaseCtDef &ctdef, ObEvalCtx &eval_ctx, bool use_new, bool is_delete, bool is_update)
{
  int ret = OB_SUCCESS;
  const share::schema::ObTableSchema *schema = NULL;
  const share::schema::ObDatabaseSchema *db = NULL;
  ObSqlCtx *sql_ctx = ctx.get_sql_ctx();
  ObSQLSessionInfo *session = ctx.get_my_session();
  char predicate[512];
  int64_t request_id = 0;
  bool have_request = false;
  if (!ctdef.is_primary_index_) {
  } else if (OB_ISNULL(sql_ctx) || OB_ISNULL(sql_ctx->schema_guard_) || OB_ISNULL(session)) {
    ret = OB_SUCCESS;
  } else if (OB_FAIL(sql_ctx->schema_guard_->get_table_schema(
                 ctdef.das_base_ctdef_.index_tid_, schema))) {
    // Inner tables are not always visible to the statement's schema guard.
    // Tracking AI columns is best effort and must never abort the DML.
    ret = OB_SUCCESS;
  } else if (OB_ISNULL(schema) || !schema->is_user_table()) {
    ret = OB_SUCCESS;
  } else if (OB_FAIL(sql_ctx->schema_guard_->get_database_schema(schema->get_database_id(), db))
             || OB_ISNULL(db)) {
    ret = OB_SUCCESS;
  } else if (OB_FAIL(build_predicate(schema, ctdef, eval_ctx, use_new, predicate, sizeof(predicate)))) {
  } else if (is_delete) {
    pthread_mutex_lock(&g_task_mu);
    erase_match(static_cast<int64_t>(schema->get_table_id()), 0, predicate, true);
    pthread_mutex_unlock(&g_task_mu);
  } else {
    for (int64_t c = 0; OB_SUCC(ret) && c < schema->get_column_count(); ++c) {
      const share::schema::ObColumnSchemaV2 *column = schema->get_column_schema_by_idx(c);
      ObExpr *result_expr = NULL;
      ObExpr *request_expr = NULL;
      char request_name[160];
      const share::schema::ObColumnSchemaV2 *request_col = NULL;
      if (OB_ISNULL(column) || !ObAIColumnRuntime::is_ai_comment(column->get_comment())) {
        continue;
      }
      result_expr = find_expr(ctdef, column->get_column_id(), true);
      if (OB_ISNULL(result_expr) || !result_expr->locate_expr_datum(eval_ctx).is_null()) {
        continue;
      }
      if (!have_request) {
        request_id = __sync_fetch_and_add(&g_request_seq, 1);
        have_request = true;
      }
      // The request id stays in memory only. Writing it into the row would make
      // the hidden column differ between the SQL old row and storage, which
      // fails the DML old row check. The row key predicate identifies the row.
      UNUSED(request_expr);
      UNUSED(request_col);
      if (OB_SUCC(ret)) {
        AiTask task;
        pthread_mutex_lock(&g_task_mu);
        erase_match(static_cast<int64_t>(schema->get_table_id()),
                    static_cast<int64_t>(column->get_column_id()), predicate, false);
        if (OB_FAIL(fill_task(task, schema, db, column, session->get_server_sid(), request_id, predicate))
            || OB_FAIL(add_task(task))) {
        }
        pthread_mutex_unlock(&g_task_mu);
      }
    }
  }
  return ret;
}

void ai_col_end_trans(ObSQLSessionInfo *session, bool commit_ok)
{
  bool wake = false;
  if (OB_ISNULL(session)) {
    return;
  }
  pthread_mutex_lock(&g_task_mu);
  for (int64_t i = 0; i < AI_TASK_CAP; ++i) {
    AiTask &task = g_tasks[i];
    if (task.used_ && !task.published_ && task.session_id_ == session->get_server_sid()) {
      if (commit_ok) {
        task.published_ = true;
        wake = true;
      } else {
        task.used_ = false;
        task.removed_ = true;
      }
    }
  }
  pthread_mutex_unlock(&g_task_mu);
  if (wake) {
    ensure_worker();
  }
}

int ai_col_collect(uint64_t session_id, ObIArray<ObAIColumnTaskView> &rows)
{
  int ret = OB_SUCCESS;
  rows.reset();
  pthread_mutex_lock(&g_task_mu);
  for (int64_t i = 0; OB_SUCC(ret) && i < AI_TASK_CAP; ++i) {
    const AiTask &task = g_tasks[i];
    ObAIColumnTaskView view;
    if (!task.used_ || task.removed_) {
      continue;
    }
    if (!task.published_ && task.session_id_ != session_id) {
      continue;
    }
    MEMSET(&view, 0, sizeof(view));
    view.table_id_ = task.table_id_;
    view.column_id_ = task.column_id_;
    view.request_id_ = task.request_id_;
    copy_c(view.predicate_, sizeof(view.predicate_), task.predicate_);
    view.eligible_ts_ = task.eligible_ts_;
    view.state_ = task.state_;
    view.retry_count_ = task.retry_count_;
    if (OB_FAIL(rows.push_back(view))) {
    }
  }
  pthread_mutex_unlock(&g_task_mu);
  return ret;
}

int ai_col_check_strict(ObExecContext &ctx, uint64_t table_id, const ObIArray<uint64_t> &column_ids)
{
  int ret = OB_SUCCESS;
  static thread_local bool in_check = false;
  ObSQLSessionInfo *session = ctx.get_my_session();
  ObSqlCtx *sql_ctx = ctx.get_sql_ctx();
  const share::schema::ObTableSchema *schema = NULL;
  const share::schema::ObDatabaseSchema *db = NULL;
  bool strict = false;
  if (in_check || OB_ISNULL(session) || OB_INVALID_ID == table_id) {
  } else if (OB_FAIL(session->get_sys_variable(share::SYS_VAR_AI_COLUMN_STRICT_READ, strict))) {
    // Strict read is optional; an unreadable sys var must not break the scan.
    ret = OB_SUCCESS;
  } else if (!strict) {
  } else if (OB_ISNULL(sql_ctx) || OB_ISNULL(sql_ctx->schema_guard_) || OB_ISNULL(GCTX.sql_proxy_)) {
  } else if (OB_FAIL(sql_ctx->schema_guard_->get_table_schema(table_id, schema))) {
    ret = OB_SUCCESS;
  } else if (OB_ISNULL(schema) || !schema->is_user_table()) {
  } else if (OB_FAIL(sql_ctx->schema_guard_->get_database_schema(schema->get_database_id(), db)) || OB_ISNULL(db)) {
    ret = OB_SUCCESS;
  } else {
    in_check = true;
    for (int64_t i = 0; OB_SUCC(ret) && i < column_ids.count(); ++i) {
      const share::schema::ObColumnSchemaV2 *column = schema->get_column_schema(column_ids.at(i));
      ObSqlString sql;
      common::ObISQLClient::ReadResult result;
      if (OB_ISNULL(column) || !ObAIColumnRuntime::is_ai_comment(column->get_comment())) {
        continue;
      } else if (OB_FAIL(sql.append_fmt("SELECT 1 FROM `%s`.`%s` WHERE `%s` IS NULL LIMIT 1",
                                       db->get_database_name(), schema->get_table_name(),
                                       column->get_column_name()))) {
      } else if (OB_FAIL(GCTX.sql_proxy_->read(result, sql.ptr()))) {
      } else if (OB_NOT_NULL(result.get_result()) && OB_SUCCESS == result.get_result()->next()) {
        ret = OB_ERR_AI_COLUMN_NOT_READY;
        LOG_USER_ERROR(OB_ERR_AI_COLUMN_NOT_READY, static_cast<int64_t>(schema->get_table_id()),
                       static_cast<int64_t>(column->get_column_id()));
      } else {
        ret = OB_SUCCESS;
      }
    }
    in_check = false;
  }
  return ret;
}
}

int ObAIColumnRuntime::collect_tasks(uint64_t session_id, common::ObIArray<ObAIColumnTaskView> &rows)
{
  return ai_col_collect(session_id, rows);
}

int ObAIColumnRuntime::on_insert(ObExecContext &ctx, const ObDMLBaseCtDef &ctdef, ObEvalCtx &eval_ctx)
{
  return ai_col_touch(ctx, ctdef, eval_ctx, true, false, false);
}

int ObAIColumnRuntime::on_update(ObExecContext &ctx, const ObDMLBaseCtDef &ctdef, ObEvalCtx &eval_ctx)
{
  return ai_col_touch(ctx, ctdef, eval_ctx, true, false, true);
}

int ObAIColumnRuntime::on_delete(ObExecContext &ctx, const ObDMLBaseCtDef &ctdef, ObEvalCtx &eval_ctx)
{
  return ai_col_touch(ctx, ctdef, eval_ctx, false, true, false);
}

void ObAIColumnRuntime::on_end_trans(ObSQLSessionInfo *session, bool commit_ok)
{
  ai_col_end_trans(session, commit_ok);
}

void ObAIColumnRuntime::on_stmt_rollback(ObSQLSessionInfo *session)
{
  ai_col_end_trans(session, false);
}

int ObAIColumnRuntime::check_strict(ObExecContext &ctx, uint64_t table_id,
                                    const common::ObIArray<uint64_t> &column_ids)
{
  return ai_col_check_strict(ctx, table_id, column_ids);
}

}
}
