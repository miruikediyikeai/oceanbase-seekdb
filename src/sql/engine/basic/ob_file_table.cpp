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

#include "sql/engine/basic/ob_file_table.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <glob.h>
#endif

#include "common/timezone/ob_time_convert.h"
#include "lib/container/ob_se_array.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/string/ob_sql_string.h"
#include "lib/utility/ob_macro_utils.h"
#include "sql/parser/parse_node.h"
#include "sql/resolver/ob_resolver_utils.h"
#include "share/system_variable/ob_sys_var_class_type.h"
#include "sql/session/ob_sql_session_info.h"

namespace oceanbase
{
using namespace common;
namespace sql
{
namespace
{
constexpr int KIND_CSV = 1;
constexpr int KIND_JSON = 2;
constexpr int KIND_PARQUET = 3;
constexpr int KIND_GLOB = 4;
constexpr int64_t MAX_COLS = 512;

struct Buf
{
  ObIAllocator *alloc_;
  char *p_;
  int64_t len_;
  int64_t cap_;
  Buf() : alloc_(NULL), p_(NULL), len_(0), cap_(0) {}
  void reset() { len_ = 0; }
  int add(char c)
  {
    return add_mem(&c, 1);
  }
  int add_mem(const char *s, int64_t n)
  {
    int ret = OB_SUCCESS;
    if (n <= 0) {
    } else if (len_ + n > cap_) {
      int64_t ncap = cap_ == 0 ? 256 : cap_;
      while (ncap < len_ + n) {
        ncap *= 2;
      }
      char *np = static_cast<char *>(alloc_->alloc(ncap));
      if (OB_ISNULL(np)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        if (len_ > 0) {
          memcpy(np, p_, len_);
        }
        p_ = np;
        cap_ = ncap;
      }
    }
    if (OB_SUCC(ret) && n > 0) {
      memcpy(p_ + len_, s, n);
      len_ += n;
    }
    return ret;
  }
};

struct Field
{
  int64_t off_;
  int64_t len_;
  bool is_null_;
  Field() : off_(0), len_(0), is_null_(true) {}
};

struct Reader
{
  FILE *fp_;
  unsigned char buf_[1 << 16];
  int64_t pos_;
  int64_t len_;
  Reader() : fp_(NULL), pos_(0), len_(0) {}
  void reset_buf() { pos_ = 0; len_ = 0; }
  int fill()
  {
    if (pos_ < len_) {
      return OB_SUCCESS;
    }
    len_ = static_cast<int64_t>(fread(buf_, 1, sizeof(buf_), fp_));
    pos_ = 0;
    return len_ > 0 ? OB_SUCCESS : OB_ITER_END;
  }
  int get(int &c)
  {
    int ret = fill();
    if (OB_ITER_END == ret) {
      c = EOF;
      ret = OB_SUCCESS;
    } else if (OB_SUCC(ret)) {
      c = buf_[pos_++];
    }
    return ret;
  }
  int peek(int &c)
  {
    int ret = fill();
    if (OB_ITER_END == ret) {
      c = EOF;
      ret = OB_SUCCESS;
    } else if (OB_SUCC(ret)) {
      c = buf_[pos_];
    }
    return ret;
  }
};

static bool ascii_eq(const ObString &a, const char *b)
{
  int64_t n = static_cast<int64_t>(strlen(b));
  if (a.length() != n || OB_ISNULL(a.ptr())) {
    return false;
  }
  for (int64_t i = 0; i < n; ++i) {
    unsigned char ca = static_cast<unsigned char>(a.ptr()[i]);
    unsigned char cb = static_cast<unsigned char>(b[i]);
    if (tolower(ca) != tolower(cb)) {
      return false;
    }
  }
  return true;
}

static bool name_eq(const ObString &a, const ObString &b)
{
  if (a.length() != b.length()) {
    return false;
  }
  for (int64_t i = 0; i < a.length(); ++i) {
    unsigned char ca = static_cast<unsigned char>(a.ptr()[i]);
    unsigned char cb = static_cast<unsigned char>(b.ptr()[i]);
    if (tolower(ca) != tolower(cb)) {
      return false;
    }
  }
  return true;
}

static int copy_str(ObIAllocator &alloc, const ObString &src, ObString &dst)
{
  return ob_write_string(alloc, src, dst);
}

static bool name_used(const ObIArray<ObFileTableCol> &cols, const ObString &n)
{
  for (int64_t i = 0; i < cols.count(); ++i) {
    if (name_eq(cols.at(i).name_, n)) {
      return true;
    }
  }
  return false;
}

static int uniquify(ObIAllocator &alloc, ObIArray<ObFileTableCol> &cols,
                    const ObString &raw, ObString &out)
{
  int ret = OB_SUCCESS;
  if (!name_used(cols, raw)) {
    ret = copy_str(alloc, raw, out);
  } else {
    bool done = false;
    for (int n = 2; OB_SUCC(ret) && !done; ++n) {
      char tmp[512];
      int nw = snprintf(tmp, sizeof(tmp), "%.*s__%d", raw.length(),
                        NULL == raw.ptr() ? "" : raw.ptr(), n);
      if (nw < 0 || nw >= static_cast<int>(sizeof(tmp))) {
        ret = OB_SIZE_OVERFLOW;
      } else {
        ObString cand(nw, tmp);
        if (!name_used(cols, cand)) {
          ret = copy_str(alloc, cand, out);
          done = true;
        }
      }
    }
  }
  return ret;
}

static int classify_text(const ObString &s, int &type)
{
  int ret = OB_SUCCESS;
  type = OB_FILE_VARCHAR;
  if (s.length() <= 0 || OB_ISNULL(s.ptr())) {
    type = OB_FILE_VARCHAR;
  } else if (ascii_eq(s, "true") || ascii_eq(s, "false")) {
    type = OB_FILE_BOOL;
  } else if (10 == s.length() && '-' == s.ptr()[4] && '-' == s.ptr()[7]) {
    int32_t dv = 0;
    bool digits = true;
    for (int i = 0; i < 10; ++i) {
      if (4 == i || 7 == i) {
      } else if (!isdigit(static_cast<unsigned char>(s.ptr()[i]))) {
        digits = false;
      }
    }
    if (digits && OB_SUCCESS == ObTimeConverter::str_to_date(s, dv)) {
      type = OB_FILE_DATE;
    }
  } else if (19 == s.length()) {
    char buf[20];
    memcpy(buf, s.ptr(), 19);
    buf[19] = '\0';
    bool ok = true;
    for (int i = 0; i < 19 && ok; ++i) {
      if (4 == i || 7 == i) {
        ok = ('-' == buf[i]);
      } else if (10 == i) {
        ok = (' ' == buf[i] || 'T' == buf[i] || 't' == buf[i]);
      } else if (13 == i || 16 == i) {
        ok = (':' == buf[i]);
      } else {
        ok = isdigit(static_cast<unsigned char>(buf[i]));
      }
    }
    if (ok) {
      if ('T' == buf[10] || 't' == buf[10]) {
        buf[10] = ' ';
      }
      ObString ds(19, buf);
      ObTimeConvertCtx ctx(NULL, false);
      int64_t dv = 0;
      if (OB_SUCCESS == ObTimeConverter::str_to_datetime(ds, ctx, dv)) {
        type = OB_FILE_DATETIME;
      }
    }
  }
  if (OB_FILE_VARCHAR == type && s.length() > 0 && NULL != s.ptr()) {
    int64_t i = 0;
    if ('+' == s.ptr()[0] || '-' == s.ptr()[0]) {
      i = 1;
    }
    bool digit = false;
    bool dot = false;
    bool exp = false;
    bool bad = false;
    for (; i < s.length() && !bad; ++i) {
      unsigned char c = static_cast<unsigned char>(s.ptr()[i]);
      if (isdigit(c)) {
        digit = true;
      } else if ('.' == c && !dot && !exp) {
        dot = true;
      } else if (('e' == c || 'E' == c) && digit && !exp) {
        exp = true;
        digit = false;
        if (i + 1 < s.length() && ('+' == s.ptr()[i + 1] || '-' == s.ptr()[i + 1])) {
          ++i;
        }
      } else {
        bad = true;
      }
    }
    if (!bad && digit) {
      type = (dot || exp) ? OB_FILE_DOUBLE : OB_FILE_BIGINT;
    }
  }
  return ret;
}

static int promote(int a, int b)
{
  if (a == b) {
    return a;
  } else if ((OB_FILE_BIGINT == a && OB_FILE_DOUBLE == b) ||
             (OB_FILE_DOUBLE == a && OB_FILE_BIGINT == b)) {
    return OB_FILE_DOUBLE;
  } else if ((OB_FILE_DATE == a && OB_FILE_DATETIME == b) ||
             (OB_FILE_DATETIME == a && OB_FILE_DATE == b)) {
    return OB_FILE_DATETIME;
  } else {
    return OB_FILE_VARCHAR;
  }
}

static int cast_text(int col_type, const ObString &s, bool is_null, ObObj &cell)
{
  int ret = OB_SUCCESS;
  if (is_null) {
    cell.set_null();
  } else if (OB_FILE_VARCHAR == col_type) {
    cell.set_varchar(s);
    cell.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    cell.set_collation_level(CS_LEVEL_IMPLICIT);
  } else if (OB_FILE_BOOL == col_type) {
    if (ascii_eq(s, "true")) {
      cell.set_tinyint(1);
    } else if (ascii_eq(s, "false")) {
      cell.set_tinyint(0);
    } else {
      ret = OB_INVALID_DATA;
    }
  } else if (OB_FILE_BIGINT == col_type) {
    char tmp[128];
    if (s.length() <= 0 || s.length() >= static_cast<int64_t>(sizeof(tmp))) {
      ret = OB_INVALID_DATA;
    } else {
      memcpy(tmp, s.ptr(), s.length());
      tmp[s.length()] = '\0';
      char *end = NULL;
      errno = 0;
      int64_t v = strtoll(tmp, &end, 10);
      if (0 != errno || end != tmp + s.length()) {
        ret = OB_INVALID_DATA;
      } else {
        cell.set_int(v);
      }
    }
  } else if (OB_FILE_DOUBLE == col_type) {
    char tmp[256];
    if (s.length() <= 0 || s.length() >= static_cast<int64_t>(sizeof(tmp))) {
      ret = OB_INVALID_DATA;
    } else {
      memcpy(tmp, s.ptr(), s.length());
      tmp[s.length()] = '\0';
      char *end = NULL;
      errno = 0;
      double v = strtod(tmp, &end);
      if (0 != errno || end != tmp + s.length()) {
        ret = OB_INVALID_DATA;
      } else {
        cell.set_double(v);
      }
    }
  } else if (OB_FILE_DATE == col_type) {
    int32_t dv = 0;
    if (OB_FAIL(ObTimeConverter::str_to_date(s, dv))) {
      ret = OB_INVALID_DATA;
    } else {
      cell.set_date(dv);
    }
  } else if (OB_FILE_DATETIME == col_type) {
    char buf[40];
    int64_t n = s.length();
    if (n == 10) {
      if (OB_ISNULL(s.ptr())) {
        ret = OB_INVALID_DATA;
      } else {
        memcpy(buf, s.ptr(), 10);
        memcpy(buf + 10, " 00:00:00", 9);
        n = 19;
      }
    } else if (n == 19 && NULL != s.ptr()) {
      memcpy(buf, s.ptr(), 19);
      if ('T' == buf[10] || 't' == buf[10]) {
        buf[10] = ' ';
      }
    } else {
      ret = OB_INVALID_DATA;
    }
    if (OB_SUCC(ret)) {
      buf[n] = '\0';
      ObString ds(static_cast<int32_t>(n), buf);
      ObTimeConvertCtx ctx(NULL, false);
      int64_t dv = 0;
      if (OB_FAIL(ObTimeConverter::str_to_datetime(ds, ctx, dv))) {
        ret = OB_INVALID_DATA;
      } else {
        cell.set_datetime(dv);
      }
    }
  } else {
    ret = OB_ERR_UNEXPECTED;
  }
  return ret;
}

static int read_csv_row(Reader &in, Buf &row, Field *fields, int64_t cap, int64_t &nfield, bool &got)
{
  int ret = OB_SUCCESS;
  got = false;
  nfield = 0;
  row.reset();
  bool any = false;
  bool in_q = false;
  int64_t off = 0;
  int64_t flen = 0;
  int c = 0;
  while (OB_SUCC(ret)) {
    if (OB_FAIL(in.get(c))) {
    } else if (EOF == c) {
      if (in_q) {
        ret = OB_INVALID_DATA;
      } else if (any) {
        if (nfield >= cap) {
          ret = OB_SIZE_OVERFLOW;
        } else {
          fields[nfield].off_ = off;
          fields[nfield].len_ = flen;
          fields[nfield].is_null_ = (0 == flen);
          ++nfield;
          got = true;
        }
      }
      break;
    } else if (in_q) {
      any = true;
      if ('"' == c) {
        int nch = 0;
        if (OB_FAIL(in.peek(nch))) {
        } else if ('"' == nch) {
          if (OB_FAIL(in.get(nch))) {
          } else if (OB_FAIL(row.add('"'))) {
          } else {
            ++flen;
          }
        } else {
          in_q = false;
        }
      } else if (OB_FAIL(row.add(static_cast<char>(c)))) {
      } else {
        ++flen;
      }
    } else if ('"' == c && 0 == flen) {
      any = true;
      in_q = true;
    } else if (',' == c) {
      any = true;
      if (nfield >= cap) {
        ret = OB_SIZE_OVERFLOW;
      } else {
        fields[nfield].off_ = off;
        fields[nfield].len_ = flen;
        fields[nfield].is_null_ = (0 == flen);
        ++nfield;
        off = row.len_;
        flen = 0;
      }
    } else if ('\n' == c || '\r' == c) {
      any = true;
      if ('\r' == c) {
        int nch = 0;
        if (OB_FAIL(in.peek(nch))) {
        } else if ('\n' == nch) {
          int drop = 0;
          ret = in.get(drop);
        }
      }
      if (OB_SUCC(ret)) {
        if (nfield >= cap) {
          ret = OB_SIZE_OVERFLOW;
        } else {
          fields[nfield].off_ = off;
          fields[nfield].len_ = flen;
          fields[nfield].is_null_ = (0 == flen);
          ++nfield;
          got = true;
        }
      }
      break;
    } else if (OB_FAIL(row.add(static_cast<char>(c)))) {
    } else {
      any = true;
      ++flen;
    }
  }
  return ret;
}

static int observe(ObIArray<int> &types, ObIArray<bool> &seen, ObIArray<bool> &nulls,
                   int64_t idx, const ObString &text, bool is_null)
{
  int ret = OB_SUCCESS;
  if (is_null) {
    nulls.at(idx) = true;
  } else {
    int t = OB_FILE_VARCHAR;
    if (OB_FAIL(classify_text(text, t))) {
    } else if (!seen.at(idx)) {
      seen.at(idx) = true;
      types.at(idx) = t;
    } else {
      types.at(idx) = promote(types.at(idx), t);
    }
  }
  return ret;
}

static int add_col(ObIAllocator &alloc, ObIArray<ObFileTableCol> &cols, const ObString &raw,
                   int type, bool nullable)
{
  int ret = OB_SUCCESS;
  ObFileTableCol col;
  col.type_ = type;
  col.nullable_ = nullable;
  if (OB_FAIL(uniquify(alloc, cols, raw, col.name_))) {
  } else if (OB_FAIL(cols.push_back(col))) {
  }
  return ret;
}

static int infer_csv(ObIAllocator &alloc, Reader &in, bool header, ObIArray<ObFileTableCol> &cols)
{
  int ret = OB_SUCCESS;
  Field fields[MAX_COLS];
  Buf row;
  row.alloc_ = &alloc;
  bool got = false;
  int64_t nfield = 0;
  ObSEArray<int, 16> types;
  ObSEArray<bool, 16> seen;
  ObSEArray<bool, 16> nulls;
  bool first = true;
  while (OB_SUCC(ret)) {
    if (OB_FAIL(read_csv_row(in, row, fields, MAX_COLS, nfield, got))) {
    } else if (!got) {
      break;
    } else if (first && header) {
      first = false;
      for (int64_t i = 0; OB_SUCC(ret) && i < nfield; ++i) {
        ObString raw;
        if (fields[i].len_ > 0) {
          raw.assign_ptr(row.p_ + fields[i].off_, static_cast<int32_t>(fields[i].len_));
        } else {
          char tmp[32];
          int nw = snprintf(tmp, sizeof(tmp), "column%ld", i);
          raw.assign_ptr(tmp, nw);
        }
        if (OB_FAIL(add_col(alloc, cols, raw, OB_FILE_VARCHAR, true))) {
        } else if (OB_FAIL(types.push_back(OB_FILE_VARCHAR)) ||
                   OB_FAIL(seen.push_back(false)) ||
                   OB_FAIL(nulls.push_back(false))) {
        }
      }
    } else {
      first = false;
      if (0 == cols.count()) {
        for (int64_t i = 0; OB_SUCC(ret) && i < nfield; ++i) {
          char tmp[32];
          int nw = snprintf(tmp, sizeof(tmp), "column%ld", i);
          ObString raw(nw, tmp);
          if (OB_FAIL(add_col(alloc, cols, raw, OB_FILE_VARCHAR, true))) {
          } else if (OB_FAIL(types.push_back(OB_FILE_VARCHAR)) ||
                     OB_FAIL(seen.push_back(false)) ||
                     OB_FAIL(nulls.push_back(true))) {
          }
        }
      }
      while (OB_SUCC(ret) && cols.count() < nfield) {
        char tmp[32];
        int nw = snprintf(tmp, sizeof(tmp), "column%ld", cols.count());
        ObString raw(nw, tmp);
        if (OB_FAIL(add_col(alloc, cols, raw, OB_FILE_VARCHAR, true))) {
        } else if (OB_FAIL(types.push_back(OB_FILE_VARCHAR)) ||
                   OB_FAIL(seen.push_back(false)) ||
                   OB_FAIL(nulls.push_back(true))) {
        }
      }
      int64_t n = nfield < cols.count() ? nfield : cols.count();
      for (int64_t i = 0; OB_SUCC(ret) && i < n; ++i) {
        ObString text;
        if (!fields[i].is_null_ && NULL != row.p_) {
          text.assign_ptr(row.p_ + fields[i].off_, static_cast<int32_t>(fields[i].len_));
        }
        if (OB_FAIL(observe(types, seen, nulls, i, text, fields[i].is_null_))) {
        }
      }
      for (int64_t i = n; OB_SUCC(ret) && i < cols.count(); ++i) {
        nulls.at(i) = true;
      }
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < cols.count(); ++i) {
    if (!seen.at(i)) {
      cols.at(i).type_ = OB_FILE_VARCHAR;
      cols.at(i).nullable_ = true;
    } else {
      cols.at(i).type_ = types.at(i);
      cols.at(i).nullable_ = nulls.at(i);
    }
  }
  return ret;
}

static int json_skip_ws(Reader &in, int &c)
{
  int ret = OB_SUCCESS;
  do {
    if (OB_FAIL(in.get(c))) {
    }
  } while (OB_SUCC(ret) && (' ' == c || '\t' == c || '\r' == c || '\n' == c));
  return ret;
}

static int json_string(Reader &in, Buf &row, int64_t &off, int64_t &len)
{
  int ret = OB_SUCCESS;
  off = row.len_;
  len = 0;
  int c = 0;
  while (OB_SUCC(ret)) {
    if (OB_FAIL(in.get(c))) {
    } else if (EOF == c) {
      ret = OB_ERR_INVALID_JSON_TEXT;
    } else if ('"' == c) {
      break;
    } else if ('\\' == c) {
      int e = 0;
      if (OB_FAIL(in.get(e)) || EOF == e) {
        ret = OB_ERR_INVALID_JSON_TEXT;
      } else {
        char out = static_cast<char>(e);
        if ('n' == e) out = '\n';
        else if ('r' == e) out = '\r';
        else if ('t' == e) out = '\t';
        else if ('b' == e) out = '\b';
        else if ('f' == e) out = '\f';
        else if ('u' == e) {
          int cp = 0;
          for (int k = 0; OB_SUCC(ret) && k < 4; ++k) {
            int h = 0;
            if (OB_FAIL(in.get(h))) {
            } else if (!isxdigit(static_cast<unsigned char>(h))) {
              ret = OB_ERR_INVALID_JSON_TEXT;
            } else {
              cp <<= 4;
              if (h >= '0' && h <= '9') cp += h - '0';
              else if (h >= 'a' && h <= 'f') cp += h - 'a' + 10;
              else cp += h - 'A' + 10;
            }
          }
          if (OB_SUCC(ret)) {
            if (cp < 0x80) {
              out = static_cast<char>(cp);
            } else if (cp < 0x800) {
              char u[2];
              u[0] = static_cast<char>(0xC0 | (cp >> 6));
              u[1] = static_cast<char>(0x80 | (cp & 0x3F));
              if (OB_FAIL(row.add_mem(u, 2))) {
              } else {
                len += 2;
              }
              continue;
            } else {
              char u[3];
              u[0] = static_cast<char>(0xE0 | (cp >> 12));
              u[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
              u[2] = static_cast<char>(0x80 | (cp & 0x3F));
              if (OB_FAIL(row.add_mem(u, 3))) {
              } else {
                len += 3;
              }
              continue;
            }
          }
        }
        if (OB_SUCC(ret) && OB_FAIL(row.add(out))) {
        } else if (OB_SUCC(ret)) {
          ++len;
        }
      }
    } else if (OB_FAIL(row.add(static_cast<char>(c)))) {
    } else {
      ++len;
    }
  }
  return ret;
}

struct Kv
{
  ObString key_;
  ObString val_;
  bool is_null_;
};

static int parse_json_object(Reader &in, Buf &row, ObIArray<Kv> &kvs)
{
  int ret = OB_SUCCESS;
  int c = 0;
  if (OB_FAIL(json_skip_ws(in, c))) {
  } else if (EOF == c) {
    ret = OB_ITER_END;
  } else if ('{' != c) {
    ret = OB_ERR_INVALID_JSON_TEXT;
  }
  bool first = true;
  while (OB_SUCC(ret)) {
    if (OB_FAIL(json_skip_ws(in, c))) {
    } else if ('}' == c) {
      break;
    } else if (!first && ',' == c) {
      if (OB_FAIL(json_skip_ws(in, c))) {
      }
    } else if (!first) {
      ret = OB_ERR_INVALID_JSON_TEXT;
    }
    first = false;
    if (OB_FAIL(ret)) {
    } else if ('}' == c) {
      break;
    } else if ('"' != c) {
      ret = OB_ERR_INVALID_JSON_TEXT;
    } else {
      int64_t koff = 0;
      int64_t klen = 0;
      Kv kv;
      kv.is_null_ = false;
      if (OB_FAIL(json_string(in, row, koff, klen))) {
      } else if (OB_FAIL(json_skip_ws(in, c)) || ':' != c) {
        ret = OB_ERR_INVALID_JSON_TEXT;
      } else if (OB_FAIL(json_skip_ws(in, c))) {
      } else if ('{' == c || '[' == c) {
        ret = OB_ERR_INVALID_JSON_TEXT;
      } else if ('"' == c) {
        int64_t voff = 0;
        int64_t vlen = 0;
        if (OB_FAIL(json_string(in, row, voff, vlen))) {
        } else if (NULL != row.p_) {
          kv.val_.assign_ptr(row.p_ + voff, static_cast<int32_t>(vlen));
        }
      } else if (EOF == c) {
        ret = OB_ERR_INVALID_JSON_TEXT;
      } else {
        int64_t voff = row.len_;
        int64_t vlen = 0;
        if (OB_FAIL(row.add(static_cast<char>(c)))) {
        } else {
          ++vlen;
        }
        while (OB_SUCC(ret)) {
          int pk = 0;
          if (OB_FAIL(in.peek(pk))) {
          } else if (EOF == pk || '}' == pk || ',' == pk || ' ' == pk || '\n' == pk ||
                     '\r' == pk || '\t' == pk) {
            break;
          } else {
            int gc = 0;
            if (OB_FAIL(in.get(gc))) {
            } else if (OB_FAIL(row.add(static_cast<char>(gc)))) {
            } else {
              ++vlen;
            }
          }
        }
        if (OB_SUCC(ret) && NULL != row.p_) {
          kv.val_.assign_ptr(row.p_ + voff, static_cast<int32_t>(vlen));
          if (ascii_eq(kv.val_, "null")) {
            kv.is_null_ = true;
            kv.val_.reset();
          }
        }
      }
      if (OB_SUCC(ret)) {
        if (NULL != row.p_) {
          kv.key_.assign_ptr(row.p_ + koff, static_cast<int32_t>(klen));
        }
        if (OB_FAIL(kvs.push_back(kv))) {
        }
      }
    }
  }
  return ret;
}

static int find_col(ObIArray<ObFileTableCol> &cols, const ObString &name, int64_t &idx)
{
  idx = -1;
  for (int64_t i = 0; i < cols.count(); ++i) {
    if (cols.at(i).name_.length() == name.length() &&
        NULL != name.ptr() && NULL != cols.at(i).name_.ptr() &&
        0 == memcmp(cols.at(i).name_.ptr(), name.ptr(), name.length())) {
      idx = i;
      break;
    }
  }
  return OB_SUCCESS;
}

static int infer_json(ObIAllocator &alloc, Reader &in, ObIArray<ObFileTableCol> &cols)
{
  int ret = OB_SUCCESS;
  Buf row;
  row.alloc_ = &alloc;
  ObSEArray<int, 16> types;
  ObSEArray<bool, 16> seen;
  ObSEArray<bool, 16> nulls;
  int64_t row_index = 0;
  while (OB_SUCC(ret)) {
    row.reset();
    ObSEArray<Kv, 16> kvs;
    if (OB_FAIL(parse_json_object(in, row, kvs))) {
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
        break;
      }
    } else {
      ObSEArray<bool, 16> hit;
      for (int64_t i = 0; OB_SUCC(ret) && i < cols.count(); ++i) {
        if (OB_FAIL(hit.push_back(false))) {
        }
      }
      for (int64_t k = 0; OB_SUCC(ret) && k < kvs.count(); ++k) {
        int64_t idx = -1;
        if (OB_FAIL(find_col(cols, kvs.at(k).key_, idx))) {
        } else if (idx < 0) {
          bool nullable = row_index > 0 || kvs.at(k).is_null_;
          if (OB_FAIL(add_col(alloc, cols, kvs.at(k).key_, OB_FILE_VARCHAR, nullable))) {
          } else if (OB_FAIL(types.push_back(OB_FILE_VARCHAR)) ||
                     OB_FAIL(seen.push_back(false)) ||
                     OB_FAIL(nulls.push_back(nullable)) ||
                     OB_FAIL(hit.push_back(true))) {
          } else {
            idx = cols.count() - 1;
          }
        } else {
          hit.at(idx) = true;
        }
        if (OB_SUCC(ret)) {
          if (OB_FAIL(observe(types, seen, nulls, idx, kvs.at(k).val_, kvs.at(k).is_null_))) {
          }
        }
      }
      for (int64_t i = 0; i < hit.count() && i < cols.count(); ++i) {
        if (!hit.at(i)) {
          nulls.at(i) = true;
        }
      }
      ++row_index;
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < cols.count(); ++i) {
    if (!seen.at(i)) {
      cols.at(i).type_ = OB_FILE_VARCHAR;
      cols.at(i).nullable_ = true;
    } else {
      cols.at(i).type_ = types.at(i);
      cols.at(i).nullable_ = nulls.at(i);
    }
  }
  return ret;
}

struct Tr
{
  const unsigned char *b_;
  int64_t n_;
  int64_t i_;
  int64_t last_;
  Tr() : b_(NULL), n_(0), i_(0), last_(0) {}
  int varint(uint64_t &v)
  {
    int ret = OB_SUCCESS;
    v = 0;
    int shift = 0;
    while (OB_SUCC(ret)) {
      if (i_ >= n_) {
        ret = OB_INVALID_DATA;
      } else {
        unsigned char c = b_[i_++];
        v |= static_cast<uint64_t>(c & 0x7F) << shift;
        if (0 == (c & 0x80)) {
          break;
        }
        shift += 7;
        if (shift > 63) {
          ret = OB_INVALID_DATA;
        }
      }
    }
    return ret;
  }
  int zz(int64_t &v)
  {
    uint64_t n = 0;
    int ret = varint(n);
    if (OB_SUCC(ret)) {
      v = static_cast<int64_t>((n >> 1) ^ (~(n & 1) + 1));
    }
    return ret;
  }
  int hdr(int &ftype, int64_t &fid, bool &stop)
  {
    int ret = OB_SUCCESS;
    stop = false;
    if (i_ >= n_) {
      ret = OB_INVALID_DATA;
    } else if (0 == b_[i_]) {
      ++i_;
      stop = true;
    } else {
      unsigned char h = b_[i_++];
      ftype = h & 0x0F;
      int delta = h >> 4;
      if (0 == delta) {
        uint64_t id = 0;
        if (OB_FAIL(varint(id))) {
        } else {
          fid = static_cast<int64_t>(id);
        }
      } else {
        fid = last_ + delta;
      }
      if (OB_SUCC(ret)) {
        last_ = fid;
      }
    }
    return ret;
  }
  int skip(int ftype);
  int skip_struct()
  {
    int ret = OB_SUCCESS;
    int64_t saved = last_;
    last_ = 0;
    while (OB_SUCC(ret)) {
      int ftype = 0;
      int64_t fid = 0;
      bool stop = false;
      if (OB_FAIL(hdr(ftype, fid, stop))) {
      } else if (stop) {
        break;
      } else if (OB_FAIL(skip(ftype))) {
      }
    }
    last_ = saved;
    return ret;
  }
};

int Tr::skip(int ftype)
{
  int ret = OB_SUCCESS;
  if (1 == ftype || 2 == ftype) {
  } else if (3 == ftype) {
    if (i_ >= n_) ret = OB_INVALID_DATA;
    else ++i_;
  } else if (4 == ftype || 5 == ftype || 6 == ftype) {
    int64_t v = 0;
    ret = zz(v);
  } else if (7 == ftype) {
    if (i_ + 8 > n_) ret = OB_INVALID_DATA;
    else i_ += 8;
  } else if (8 == ftype) {
    uint64_t len = 0;
    if (OB_FAIL(varint(len))) {
    } else if (i_ + static_cast<int64_t>(len) > n_) {
      ret = OB_INVALID_DATA;
    } else {
      i_ += static_cast<int64_t>(len);
    }
  } else if (9 == ftype || 10 == ftype) {
    if (i_ >= n_) {
      ret = OB_INVALID_DATA;
    } else {
      unsigned char eh = b_[i_++];
      int et = eh & 0x0F;
      int sz = eh >> 4;
      if (15 == sz) {
        uint64_t nsz = 0;
        if (OB_FAIL(varint(nsz))) {
        } else {
          sz = static_cast<int>(nsz);
        }
      }
      for (int k = 0; OB_SUCC(ret) && k < sz; ++k) {
        ret = skip(et);
      }
    }
  } else if (11 == ftype) {
    if (i_ >= n_) {
      ret = OB_INVALID_DATA;
    } else {
      unsigned char eh = b_[i_++];
      int kty = eh & 0x0F;
      int vty = eh >> 4;
      uint64_t sz = 0;
      if (OB_FAIL(varint(sz))) {
      }
      for (uint64_t k = 0; OB_SUCC(ret) && k < sz; ++k) {
        if (OB_FAIL(skip(kty))) {
        } else if (OB_FAIL(skip(vty))) {
        }
      }
    }
  } else if (12 == ftype) {
    ret = skip_struct();
  } else {
    ret = OB_INVALID_DATA;
  }
  return ret;
}

struct PqCol
{
  ObString name_;
  int physical_;
  int codec_;
  int64_t data_off_;
  int64_t compressed_size_;
  bool nullable_;
  int64_t page_off_;
  int64_t chunk_end_;
  int64_t cur_;
  int64_t cnt_;
  int64_t cap_;
  ObObj *vals_;
  char *bytes_;
  int64_t bytes_len_;
  int64_t bytes_cap_;
  PqCol() : name_(), physical_(1), codec_(0), data_off_(0), compressed_size_(0),
            nullable_(true), page_off_(0), chunk_end_(0), cur_(0), cnt_(0), cap_(0),
            vals_(NULL), bytes_(NULL), bytes_len_(0), bytes_cap_(0) {}
};

static int read_binary(Tr &tr, ObString &s)
{
  int ret = OB_SUCCESS;
  uint64_t len = 0;
  if (OB_FAIL(tr.varint(len))) {
  } else if (tr.i_ + static_cast<int64_t>(len) > tr.n_) {
    ret = OB_INVALID_DATA;
  } else {
    s.assign_ptr(reinterpret_cast<const char *>(tr.b_ + tr.i_), static_cast<int32_t>(len));
    tr.i_ += static_cast<int64_t>(len);
  }
  return ret;
}

static int parse_schema_el(Tr &tr, int &physical, int &rep, ObString &name, int &nchild, bool &has_type)
{
  int ret = OB_SUCCESS;
  physical = -1;
  rep = 0;
  nchild = 0;
  has_type = false;
  name.reset();
  int64_t saved = tr.last_;
  tr.last_ = 0;
  while (OB_SUCC(ret)) {
    int ftype = 0;
    int64_t fid = 0;
    bool stop = false;
    if (OB_FAIL(tr.hdr(ftype, fid, stop))) {
    } else if (stop) {
      break;
    } else if (1 == fid && 5 == ftype) {
      int64_t v = 0;
      if (OB_FAIL(tr.zz(v))) {
      } else {
        physical = static_cast<int>(v);
        has_type = true;
      }
    } else if (3 == fid && 5 == ftype) {
      int64_t v = 0;
      if (OB_FAIL(tr.zz(v))) {
      } else {
        rep = static_cast<int>(v);
      }
    } else if (4 == fid && 8 == ftype) {
      ret = read_binary(tr, name);
    } else if (5 == fid && 5 == ftype) {
      int64_t v = 0;
      if (OB_FAIL(tr.zz(v))) {
      } else {
        nchild = static_cast<int>(v);
      }
    } else if (OB_FAIL(tr.skip(ftype))) {
    }
  }
  tr.last_ = saved;
  return ret;
}

static int parse_col_meta(Tr &tr, PqCol &col)
{
  int ret = OB_SUCCESS;
  int64_t saved = tr.last_;
  tr.last_ = 0;
  while (OB_SUCC(ret)) {
    int ftype = 0;
    int64_t fid = 0;
    bool stop = false;
    if (OB_FAIL(tr.hdr(ftype, fid, stop))) {
    } else if (stop) {
      break;
    } else if (1 == fid && 5 == ftype) {
      int64_t v = 0;
      if (OB_FAIL(tr.zz(v))) {
      } else {
        col.physical_ = static_cast<int>(v);
      }
    } else if (4 == fid && 5 == ftype) {
      int64_t v = 0;
      if (OB_FAIL(tr.zz(v))) {
      } else {
        col.codec_ = static_cast<int>(v);
      }
    } else if (9 == fid && 6 == ftype) {
      ret = tr.zz(col.data_off_);
    } else if (7 == fid && 6 == ftype) {
      ret = tr.zz(col.compressed_size_);
    } else if (OB_FAIL(tr.skip(ftype))) {
    }
  }
  tr.last_ = saved;
  return ret;
}

static int parse_col_chunk(Tr &tr, PqCol &col)
{
  int ret = OB_SUCCESS;
  int64_t saved = tr.last_;
  tr.last_ = 0;
  while (OB_SUCC(ret)) {
    int ftype = 0;
    int64_t fid = 0;
    bool stop = false;
    if (OB_FAIL(tr.hdr(ftype, fid, stop))) {
    } else if (stop) {
      break;
    } else if (3 == fid && 12 == ftype) {
      ret = parse_col_meta(tr, col);
    } else if (OB_FAIL(tr.skip(ftype))) {
    }
  }
  tr.last_ = saved;
  return ret;
}

static int parse_footer(const unsigned char *b, int64_t n, ObIAllocator &alloc,
                        ObIArray<PqCol> &cols, int64_t &num_rows)
{
  int ret = OB_SUCCESS;
  num_rows = 0;
  struct El { int physical; int rep; int nchild; bool has_type; ObString name; };
  ObSEArray<El, 8> schema;
  ObSEArray<PqCol, 4> chunks;
  Tr tr;
  tr.b_ = b;
  tr.n_ = n;
  tr.last_ = 0;
  while (OB_SUCC(ret) && tr.i_ < tr.n_) {
    int ftype = 0;
    int64_t fid = 0;
    bool stop = false;
    if (OB_FAIL(tr.hdr(ftype, fid, stop))) {
    } else if (stop) {
      break;
    } else if (2 == fid && 9 == ftype) {
      if (tr.i_ >= tr.n_) {
        ret = OB_INVALID_DATA;
      } else {
        unsigned char eh = tr.b_[tr.i_++];
        int et = eh & 0x0F;
        int sz = eh >> 4;
        if (15 == sz) {
          uint64_t nsz = 0;
          if (OB_FAIL(tr.varint(nsz))) {
          } else {
            sz = static_cast<int>(nsz);
          }
        }
        for (int k = 0; OB_SUCC(ret) && k < sz; ++k) {
          if (12 != et) {
            ret = tr.skip(et);
          } else {
            El el;
            el.physical = -1;
            el.rep = 0;
            el.nchild = 0;
            el.has_type = false;
            if (OB_FAIL(parse_schema_el(tr, el.physical, el.rep, el.name, el.nchild, el.has_type))) {
            } else if (OB_FAIL(schema.push_back(el))) {
            }
          }
        }
      }
    } else if (3 == fid && 6 == ftype) {
      ret = tr.zz(num_rows);
    } else if (4 == fid && 9 == ftype) {
      if (tr.i_ >= tr.n_) {
        ret = OB_INVALID_DATA;
      } else {
        unsigned char eh = tr.b_[tr.i_++];
        int et = eh & 0x0F;
        int sz = eh >> 4;
        if (15 == sz) {
          uint64_t nsz = 0;
          if (OB_FAIL(tr.varint(nsz))) {
          } else {
            sz = static_cast<int>(nsz);
          }
        }
        for (int g = 0; OB_SUCC(ret) && g < sz; ++g) {
          if (12 != et) {
            ret = tr.skip(et);
          } else {
            int64_t saved = tr.last_;
            tr.last_ = 0;
            while (OB_SUCC(ret)) {
              int ft = 0;
              int64_t id = 0;
              bool st = false;
              if (OB_FAIL(tr.hdr(ft, id, st))) {
              } else if (st) {
                break;
              } else if (1 == id && 9 == ft) {
                if (tr.i_ >= tr.n_) {
                  ret = OB_INVALID_DATA;
                } else {
                  unsigned char eh2 = tr.b_[tr.i_++];
                  int et2 = eh2 & 0x0F;
                  int sz2 = eh2 >> 4;
                  if (15 == sz2) {
                    uint64_t nsz = 0;
                    if (OB_FAIL(tr.varint(nsz))) {
                    } else {
                      sz2 = static_cast<int>(nsz);
                    }
                  }
                  for (int c = 0; OB_SUCC(ret) && c < sz2; ++c) {
                    PqCol col;
                    if (12 != et2) {
                      ret = tr.skip(et2);
                    } else if (OB_FAIL(parse_col_chunk(tr, col))) {
                    } else if (OB_FAIL(chunks.push_back(col))) {
                    }
                  }
                }
              } else if (OB_FAIL(tr.skip(ft))) {
              }
            }
            tr.last_ = saved;
          }
        }
      }
    } else if (OB_FAIL(tr.skip(ftype))) {
    }
  }
  int64_t idx = 0;
  int64_t leaf = 0;
  while (OB_SUCC(ret) && idx < schema.count()) {
    El el = schema.at(idx++);
    if (el.nchild > 0) {
      continue;
    }
    if (leaf >= chunks.count()) {
      ret = OB_INVALID_DATA;
    } else {
      PqCol col = chunks.at(leaf++);
      ObString raw = el.name;
      if (OB_FAIL(copy_str(alloc, raw, col.name_))) {
      } else {
        col.physical_ = el.has_type ? el.physical : col.physical_;
        col.nullable_ = (0 != el.rep);
        col.page_off_ = col.data_off_;
        col.chunk_end_ = col.data_off_ + col.compressed_size_;
        if (OB_FAIL(cols.push_back(col))) {
        }
      }
    }
  }
  return ret;
}

static int file_size(FILE *fp, int64_t &sz)
{
  int ret = OB_SUCCESS;
  if (0 != fseeko(fp, 0, SEEK_END)) {
    ret = OB_IO_ERROR;
  } else {
    off_t n = ftello(fp);
    if (n < 0) {
      ret = OB_IO_ERROR;
    } else {
      sz = static_cast<int64_t>(n);
      if (0 != fseeko(fp, 0, SEEK_SET)) {
        ret = OB_IO_ERROR;
      }
    }
  }
  return ret;
}

static int read_at(FILE *fp, int64_t off, void *buf, int64_t n)
{
  int ret = OB_SUCCESS;
  if (0 != fseeko(fp, off, SEEK_SET)) {
    ret = OB_IO_ERROR;
  } else {
    int64_t got = 0;
    while (got < n && OB_SUCC(ret)) {
      size_t r = fread(static_cast<char *>(buf) + got, 1, static_cast<size_t>(n - got), fp);
      if (0 == r) {
        ret = OB_INVALID_DATA;
      } else {
        got += static_cast<int64_t>(r);
      }
    }
  }
  return ret;
}

static int load_footer(FILE *fp, ObIAllocator &alloc, unsigned char *&footer, int64_t &flen)
{
  int ret = OB_SUCCESS;
  int64_t sz = 0;
  footer = NULL;
  flen = 0;
  unsigned char tail[8];
  if (OB_FAIL(file_size(fp, sz))) {
  } else if (sz < 12) {
    ret = OB_INVALID_DATA;
  } else if (OB_FAIL(read_at(fp, sz - 8, tail, 8))) {
  } else if (0 != memcmp(tail + 4, "PAR1", 4)) {
    ret = OB_INVALID_DATA;
  } else {
    uint32_t n = static_cast<uint32_t>(tail[0]) |
                 (static_cast<uint32_t>(tail[1]) << 8) |
                 (static_cast<uint32_t>(tail[2]) << 16) |
                 (static_cast<uint32_t>(tail[3]) << 24);
    if (n > 8 * 1024 * 1024 || static_cast<int64_t>(n) + 8 > sz) {
      ret = OB_INVALID_DATA;
    } else if (OB_ISNULL(footer = static_cast<unsigned char *>(alloc.alloc(n + 1)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_FAIL(read_at(fp, sz - 8 - n, footer, n))) {
    } else {
      flen = n;
    }
  }
  return ret;
}

static int physical_to_file_type(int physical, int &type)
{
  int ret = OB_SUCCESS;
  switch (physical) {
    case 0: type = OB_FILE_BOOL; break;
    case 1:
    case 2: type = OB_FILE_BIGINT; break;
    case 4:
    case 5: type = OB_FILE_DOUBLE; break;
    case 6: type = OB_FILE_VARCHAR; break;
    default:
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "parquet physical type");
      break;
  }
  return ret;
}

static int infer_parquet(ObIAllocator &alloc, FILE *fp, ObIArray<ObFileTableCol> &cols)
{
  int ret = OB_SUCCESS;
  unsigned char *footer = NULL;
  int64_t flen = 0;
  ObSEArray<PqCol, 4> pcols;
  int64_t nrows = 0;
  if (OB_FAIL(load_footer(fp, alloc, footer, flen))) {
  } else if (OB_FAIL(parse_footer(footer, flen, alloc, pcols, nrows))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < pcols.count(); ++i) {
    int ft = OB_FILE_VARCHAR;
    if (OB_FAIL(physical_to_file_type(pcols.at(i).physical_, ft))) {
    } else if (OB_FAIL(add_col(alloc, cols, pcols.at(i).name_, ft, pcols.at(i).nullable_))) {
    }
  }
  return ret;
}

static int snappy_uncompress(const unsigned char *src, int64_t src_len, Buf &out)
{
  int ret = OB_SUCCESS;
  int64_t i = 0;
  uint64_t expect = 0;
  int shift = 0;
  if (src_len <= 0) {
    ret = OB_INVALID_DATA;
  }
  while (OB_SUCC(ret)) {
    if (i >= src_len) {
      ret = OB_INVALID_DATA;
    } else {
      unsigned char c = src[i++];
      expect |= static_cast<uint64_t>(c & 0x7F) << shift;
      if (0 == (c & 0x80)) {
        break;
      }
      shift += 7;
    }
  }
  int64_t produced = 0;
  while (OB_SUCC(ret) && produced < static_cast<int64_t>(expect)) {
    if (i >= src_len) {
      ret = OB_INVALID_DATA;
    } else {
      unsigned char tag = src[i++];
      int kind = tag & 3;
      if (0 == kind) {
        int64_t len = tag >> 2;
        if (len >= 60) {
          int extra = static_cast<int>(len - 59);
          len = 0;
          if (i + extra > src_len) {
            ret = OB_INVALID_DATA;
          } else {
            for (int k = 0; k < extra; ++k) {
              len |= static_cast<int64_t>(src[i++]) << (8 * k);
            }
          }
        }
        ++len;
        if (OB_FAIL(ret)) {
        } else if (i + len > src_len || produced + len > static_cast<int64_t>(expect)) {
          ret = OB_INVALID_DATA;
        } else if (OB_FAIL(out.add_mem(reinterpret_cast<const char *>(src + i), len))) {
        } else {
          i += len;
          produced += len;
        }
      } else {
        int64_t len = 0;
        int64_t offset = 0;
        if (1 == kind) {
          if (i >= src_len) {
            ret = OB_INVALID_DATA;
          } else {
            len = ((tag >> 2) & 7) + 4;
            offset = ((tag & 0xE0) << 3) | src[i++];
          }
        } else if (2 == kind) {
          if (i + 2 > src_len) {
            ret = OB_INVALID_DATA;
          } else {
            len = (tag >> 2) + 1;
            offset = src[i] | (src[i + 1] << 8);
            i += 2;
          }
        } else if (i + 4 > src_len) {
          ret = OB_INVALID_DATA;
        } else {
          len = (tag >> 2) + 1;
          offset = src[i] | (src[i + 1] << 8) | (src[i + 2] << 16) | (src[i + 3] << 24);
          i += 4;
        }
        if (OB_FAIL(ret)) {
        } else if (offset <= 0 || offset > produced || produced + len > static_cast<int64_t>(expect)) {
          ret = OB_INVALID_DATA;
        } else {
          for (int64_t k = 0; OB_SUCC(ret) && k < len; ++k) {
            ret = out.add(out.p_[produced - offset]);
            if (OB_SUCC(ret)) {
              ++produced;
            }
          }
        }
      }
    }
  }
  return ret;
}

static int parse_page_header(Tr &tr, int &page_type, int64_t &uncomp, int64_t &comp,
                             int64_t &num_values, int64_t &num_nulls, int64_t &def_len,
                             int64_t &rep_len, int &encoding)
{
  int ret = OB_SUCCESS;
  page_type = -1;
  uncomp = 0;
  comp = 0;
  num_values = 0;
  num_nulls = 0;
  def_len = 0;
  rep_len = 0;
  encoding = 0;
  int64_t saved = tr.last_;
  tr.last_ = 0;
  while (OB_SUCC(ret)) {
    int ftype = 0;
    int64_t fid = 0;
    bool stop = false;
    if (OB_FAIL(tr.hdr(ftype, fid, stop))) {
    } else if (stop) {
      break;
    } else if (1 == fid && 5 == ftype) {
      ret = tr.zz(page_type);
    } else if (2 == fid && 5 == ftype) {
      ret = tr.zz(uncomp);
    } else if (3 == fid && 5 == ftype) {
      ret = tr.zz(comp);
    } else if (8 == fid && 12 == ftype) {
      int64_t saved2 = tr.last_;
      tr.last_ = 0;
      while (OB_SUCC(ret)) {
        int ft = 0;
        int64_t id = 0;
        bool st = false;
        if (OB_FAIL(tr.hdr(ft, id, st))) {
        } else if (st) {
          break;
        } else if (1 == id && 5 == ft) {
          ret = tr.zz(num_values);
        } else if (2 == id && 5 == ft) {
          ret = tr.zz(num_nulls);
        } else if (4 == id && 5 == ft) {
          ret = tr.zz(encoding);
        } else if (5 == id && 5 == ft) {
          ret = tr.zz(def_len);
        } else if (6 == id && 5 == ft) {
          ret = tr.zz(rep_len);
        } else if (OB_FAIL(tr.skip(ft))) {
        }
      }
      tr.last_ = saved2;
    } else if (OB_FAIL(tr.skip(ftype))) {
    }
  }
  tr.last_ = saved;
  return ret;
}

static int decode_plain(const unsigned char *p, int64_t n, int physical, int64_t nval,
                        ObIAllocator &alloc, Buf &store, ObObj *vals)
{
  int ret = OB_SUCCESS;
  int64_t i = 0;
  for (int64_t k = 0; OB_SUCC(ret) && k < nval; ++k) {
    if (0 == physical) {
      if (i >= n) {
        ret = OB_INVALID_DATA;
      } else {
        vals[k].set_tinyint(0 == p[i++] ? 0 : 1);
      }
    } else if (1 == physical) {
      if (i + 4 > n) {
        ret = OB_INVALID_DATA;
      } else {
        int32_t v = 0;
        memcpy(&v, p + i, 4);
        i += 4;
        vals[k].set_int(v);
      }
    } else if (2 == physical) {
      if (i + 8 > n) {
        ret = OB_INVALID_DATA;
      } else {
        int64_t v = 0;
        memcpy(&v, p + i, 8);
        i += 8;
        vals[k].set_int(v);
      }
    } else if (4 == physical) {
      if (i + 4 > n) {
        ret = OB_INVALID_DATA;
      } else {
        float v = 0;
        memcpy(&v, p + i, 4);
        i += 4;
        vals[k].set_double(static_cast<double>(v));
      }
    } else if (5 == physical) {
      if (i + 8 > n) {
        ret = OB_INVALID_DATA;
      } else {
        double v = 0;
        memcpy(&v, p + i, 8);
        i += 8;
        vals[k].set_double(v);
      }
    } else if (6 == physical) {
      if (i + 4 > n) {
        ret = OB_INVALID_DATA;
      } else {
        uint32_t ln = 0;
        memcpy(&ln, p + i, 4);
        i += 4;
        if (i + static_cast<int64_t>(ln) > n) {
          ret = OB_INVALID_DATA;
        } else {
          int64_t off = store.len_;
          if (OB_FAIL(store.add_mem(reinterpret_cast<const char *>(p + i), ln))) {
          } else {
            i += ln;
            vals[k].set_varchar(ObString(static_cast<int32_t>(ln), store.p_ + off));
            vals[k].set_collation_type(CS_TYPE_UTF8MB4_BIN);
            vals[k].set_collation_level(CS_LEVEL_IMPLICIT);
          }
        }
      }
    } else {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "parquet encoding");
    }
    UNUSED(alloc);
  }
  return ret;
}

static int open_rb(ObIAllocator &alloc, const ObString &path, FILE *&fp)
{
  int ret = OB_SUCCESS;
  fp = NULL;
  ObString z;
  if (path.empty()) {
    ret = OB_FILE_NOT_EXIST;
  } else if (OB_FAIL(ob_write_string(alloc, path, z, true))) {
  } else if (OB_ISNULL(z.ptr()) || OB_ISNULL(fp = fopen(z.ptr(), "rb"))) {
    ret = OB_FILE_NOT_EXIST;
    LOG_WARN("file sql open failed", K(ret), K(path));
  }
  return ret;
}

struct Pay
{
  int kind_;
  bool header_;
  ObString path_;
  ObFileTableCol *cols_;
  int64_t ncol_;
  Pay() : kind_(0), header_(true), path_(), cols_(NULL), ncol_(0) {}
};

static int read_i64_line(const char *p, int64_t n, int64_t &i, int64_t &v)
{
  int ret = OB_SUCCESS;
  char tmp[64];
  int tn = 0;
  while (i < n && '\n' != p[i]) {
    if (tn < 63) {
      tmp[tn++] = p[i];
    }
    ++i;
  }
  if (i >= n || '\n' != p[i]) {
    ret = OB_INVALID_DATA;
  } else {
    ++i;
    tmp[tn] = '\0';
    char *end = NULL;
    errno = 0;
    v = strtoll(tmp, &end, 10);
    if (0 != errno || end == tmp) {
      ret = OB_INVALID_DATA;
    }
  }
  return ret;
}

static int read_block(const char *p, int64_t n, int64_t &i, ObString &s)
{
  int ret = OB_SUCCESS;
  int64_t len = 0;
  if (OB_FAIL(read_i64_line(p, n, i, len))) {
  } else if (len < 0 || i + len >= n || '\n' != p[i + len]) {
    ret = OB_INVALID_DATA;
  } else {
    s.assign_ptr(p + i, static_cast<int32_t>(len));
    i += len + 1;
  }
  return ret;
}

static int parse_payload(ObIAllocator &alloc, const ObString &payload, Pay &pay)
{
  int ret = OB_SUCCESS;
  const char *p = payload.ptr();
  int64_t n = payload.length();
  int64_t i = 0;
  if (!ob_is_file_table_payload(payload) || n < 8 || '\n' != p[7]) {
    ret = OB_INVALID_DATA;
  } else {
    i = 8;
  }
  int64_t kind = 0;
  int64_t header = 1;
  int64_t sample = -1;
  int64_t ncol = 0;
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(read_i64_line(p, n, i, kind)) ||
             OB_FAIL(read_i64_line(p, n, i, header)) ||
             OB_FAIL(read_i64_line(p, n, i, sample)) ||
             OB_FAIL(read_block(p, n, i, pay.path_)) ||
             OB_FAIL(read_i64_line(p, n, i, ncol))) {
  } else if (ncol < 0 || ncol > MAX_COLS) {
    ret = OB_INVALID_DATA;
  } else if (ncol > 0 &&
             OB_ISNULL(pay.cols_ = static_cast<ObFileTableCol *>(alloc.alloc(sizeof(ObFileTableCol) * ncol)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    pay.kind_ = static_cast<int>(kind);
    pay.header_ = (0 != header);
    pay.ncol_ = ncol;
    for (int64_t c = 0; OB_SUCC(ret) && c < ncol; ++c) {
      int64_t t = 0;
      int64_t nu = 1;
      new (pay.cols_ + c) ObFileTableCol();
      if (OB_FAIL(read_block(p, n, i, pay.cols_[c].name_))) {
      } else if (OB_FAIL(read_i64_line(p, n, i, t)) || OB_FAIL(read_i64_line(p, n, i, nu))) {
      } else {
        pay.cols_[c].type_ = static_cast<int>(t);
        pay.cols_[c].nullable_ = (0 != nu);
      }
    }
  }
  return ret;
}

static int build_payload(ObIAllocator &alloc, int kind, bool header, int64_t sample,
                         const ObString &path, const ObIArray<ObFileTableCol> &cols,
                         ObString &payload)
{
  int ret = OB_SUCCESS;
  ObSqlString s;
  if (OB_FAIL(s.append("OBFILE1\n"))) {
  } else if (OB_FAIL(s.append_fmt("%d\n%d\n%ld\n%ld\n", kind, header ? 1 : 0, sample,
                                  static_cast<long>(path.length())))) {
  } else if (path.length() > 0 && OB_FAIL(s.append_fmt("%.*s", path.length(), path.ptr()))) {
  } else if (OB_FAIL(s.append_fmt("\n%ld\n", static_cast<long>(cols.count())))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < cols.count(); ++i) {
    const ObFileTableCol &c = cols.at(i);
    if (OB_FAIL(s.append_fmt("%ld\n", static_cast<long>(c.name_.length())))) {
    } else if (c.name_.length() > 0 &&
               OB_FAIL(s.append_fmt("%.*s", c.name_.length(), c.name_.ptr()))) {
    } else if (OB_FAIL(s.append_fmt("\n%d\n%d\n", c.type_, c.nullable_ ? 1 : 0))) {
    }
  }
  if (OB_SUCC(ret)) {
    ret = ob_write_string(alloc, ObString(s.length(), s.ptr()), payload);
  }
  return ret;
}

static const char *file_type_name(int t)
{
  switch (t) {
    case OB_FILE_BIGINT: return "BIGINT";
    case OB_FILE_DOUBLE: return "DOUBLE";
    case OB_FILE_BOOL: return "BOOLEAN";
    case OB_FILE_DATE: return "DATE";
    case OB_FILE_DATETIME: return "DATETIME";
    default: return "VARCHAR";
  }
}

static int append_sql_lit(ObSqlString &s, const ObString &v)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(s.append("'"))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < v.length(); ++i) {
    char c = v.ptr()[i];
    if ('\'' == c) {
      if (OB_FAIL(s.append("''"))) {
      }
    } else if (OB_FAIL(s.append_fmt("%c", c))) {
    }
  }
  if (OB_SUCC(ret)) {
    ret = s.append("'");
  }
  return ret;
}

} // namespace

int ob_file_table_fill_meta(int type, ObObjMeta &meta, ObAccuracy &accuracy)
{
  int ret = OB_SUCCESS;
  accuracy.reset();
  switch (type) {
    case OB_FILE_BIGINT:
      meta.set_int();
      break;
    case OB_FILE_DOUBLE:
      meta.set_double();
      break;
    case OB_FILE_BOOL:
      meta.set_tinyint();
      break;
    case OB_FILE_DATE:
      meta.set_date();
      break;
    case OB_FILE_DATETIME:
      meta.set_datetime();
      accuracy.set_scale(0);
      break;
    default:
      meta.set_varchar();
      meta.set_collation_type(CS_TYPE_UTF8MB4_BIN);
      meta.set_collation_level(CS_LEVEL_IMPLICIT);
      accuracy.set_length(65535);
      break;
  }
  return ret;
}

int ob_file_table_column_names(const ObString &payload, ObIAllocator &alloc, ObIArray<ObString> &names)
{
  int ret = OB_SUCCESS;
  Pay pay;
  if (OB_FAIL(parse_payload(alloc, payload, pay))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < pay.ncol_; ++i) {
    ObString name;
    if (OB_FAIL(copy_str(alloc, pay.cols_[i].name_, name))) {
    } else if (OB_FAIL(names.push_back(name))) {
    }
  }
  return ret;
}

int ob_file_table_build_describe_sql(ObIAllocator &alloc, const ObIArray<ObFileTableCol> &cols, ObString &sql)
{
  int ret = OB_SUCCESS;
  ObSqlString s;
  if (0 == cols.count()) {
    ret = s.append("SELECT '' AS `column_name`, '' AS `column_type`, '' AS `null`, "
                   "NULL AS `key`, NULL AS `default`, NULL AS `extra` WHERE 0");
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < cols.count(); ++i) {
    if (i > 0 && OB_FAIL(s.append(" UNION ALL "))) {
    } else if (OB_FAIL(s.append("SELECT "))) {
    } else if (OB_FAIL(append_sql_lit(s, cols.at(i).name_))) {
    } else if (0 == i && OB_FAIL(s.append(" AS `column_name`, "))) {
    } else if (0 != i && OB_FAIL(s.append(", "))) {
    } else if (OB_FAIL(append_sql_lit(s, ObString(file_type_name(cols.at(i).type_))))) {
    } else if (0 == i && OB_FAIL(s.append(" AS `column_type`, "))) {
    } else if (0 != i && OB_FAIL(s.append(", "))) {
    } else if (OB_FAIL(s.append(cols.at(i).nullable_ ? "'YES'" : "'NO'"))) {
    } else if (0 == i && OB_FAIL(s.append(" AS `null`, NULL AS `key`, NULL AS `default`, NULL AS `extra`"))) {
    } else if (0 != i && OB_FAIL(s.append(", NULL, NULL, NULL"))) {
    }
  }
  if (OB_SUCC(ret)) {
    ret = ob_write_string(alloc, ObString(s.length(), s.ptr()), sql);
  }
  return ret;
}

static int node_text(const ParseNode *node, ObString &s)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(node) || OB_ISNULL(node->str_value_)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    s.assign_ptr(node->str_value_, static_cast<int32_t>(node->str_len_));
  }
  return ret;
}

int ob_file_table_resolve_node(ObIAllocator &alloc, ObSQLSessionInfo *session, const ParseNode *node,
                               ObIArray<ObFileTableCol> &cols, ObString &payload)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(session) || OB_ISNULL(node) || T_FILE_TABLE_EXPRESSION != node->type_ ||
      node->num_child_ < 2 || OB_ISNULL(node->children_[0]) || OB_ISNULL(node->children_[1])) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    int kind = static_cast<int>(node->children_[0]->value_);
    ObString path;
    bool header = true;
    int64_t sample = -1;
    ObString format;
    if (OB_FAIL(node_text(node->children_[1], path))) {
    } else if (NULL != node->children_[2]) {
      ParseNode *opts = node->children_[2];
      for (int64_t i = 0; OB_SUCC(ret) && i < opts->num_child_; ++i) {
        ParseNode *pair = opts->children_[i];
        ObString name;
        if (OB_ISNULL(pair) || pair->num_child_ < 2) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(node_text(pair->children_[0], name))) {
        } else if (0 == name.case_compare("header")) {
          header = (0 != pair->children_[1]->value_);
        } else if (0 == name.case_compare("sample_size")) {
          sample = pair->children_[1]->value_;
        } else if (0 == name.case_compare("format")) {
          ret = node_text(pair->children_[1], format);
        } else {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "file sql option");
        }
      }
    }
    bool enabled = false;
    ObString secure;
    if (OB_FAIL(ret)) {
    } else if (-1 != sample) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "sample_size other than -1");
    } else if (KIND_JSON == kind && format.length() > 0 &&
               0 != format.case_compare("newline_delimited")) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "json format");
    } else if (OB_FAIL(session->get_bool_sys_var(share::SYS_VAR_ENABLE_FILE_SQL, enabled))) {
    } else if (!enabled) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "file sql");
    } else if (OB_FAIL(session->get_secure_file_priv(secure))) {
    } else if (OB_FAIL(ObResolverUtils::check_secure_path(secure, path))) {
    } else if (KIND_GLOB == kind) {
      ObString fname(4, "file");
      ret = add_col(alloc, cols, fname, OB_FILE_VARCHAR, false);
    } else {
      FILE *fp = NULL;
      if (OB_FAIL(open_rb(alloc, path, fp))) {
      } else if (KIND_CSV == kind) {
        Reader in;
        in.fp_ = fp;
        ret = infer_csv(alloc, in, header, cols);
      } else if (KIND_JSON == kind) {
        Reader in;
        in.fp_ = fp;
        ret = infer_json(alloc, in, cols);
      } else if (KIND_PARQUET == kind) {
        ret = infer_parquet(alloc, fp, cols);
      } else {
        ret = OB_ERR_UNEXPECTED;
      }
      if (NULL != fp) {
        fclose(fp);
      }
    }
    if (OB_SUCC(ret)) {
      ret = build_payload(alloc, kind, header, sample, path, cols, payload);
    }
  }
  return ret;
}

struct ObFileTableScan::Impl
{
  ObIAllocator *alloc_;
  FILE *fp_;
  Reader in_;
  Pay pay_;
  Buf row_;
  ObObj *cells_;
  bool opened_;
  char **paths_;
  int64_t npath_;
  int64_t path_idx_;
  PqCol *pcols_;
  int64_t npq_;
  bool pq_ready_;
  Impl() : alloc_(NULL), fp_(NULL), in_(), pay_(), row_(), cells_(NULL), opened_(false),
           paths_(NULL), npath_(0), path_idx_(0), pcols_(NULL), npq_(0), pq_ready_(false) {}
};

static int fill_cells_from_fields(Pay &pay, Buf &row, Field *fields, int64_t nfield, ObObj *cells)
{
  int ret = OB_SUCCESS;
  int64_t n = nfield < pay.ncol_ ? nfield : pay.ncol_;
  for (int64_t i = 0; OB_SUCC(ret) && i < pay.ncol_; ++i) {
    if (i >= n || fields[i].is_null_) {
      cells[i].set_null();
    } else {
      ObString text;
      if (fields[i].len_ > 0 && NULL != row.p_) {
        text.assign_ptr(row.p_ + fields[i].off_, static_cast<int32_t>(fields[i].len_));
      }
      ret = cast_text(pay.cols_[i].type_, text, false, cells[i]);
    }
  }
  if (nfield > pay.ncol_) {
    ret = OB_INVALID_DATA;
  }
  return ret;
}

static int load_pq_page(ObFileTableScan::Impl &im, PqCol &col)
{
  int ret = OB_SUCCESS;
  if (col.page_off_ >= col.chunk_end_) {
    ret = OB_ITER_END;
  } else {
    int64_t remain = col.chunk_end_ - col.page_off_;
    int64_t probe = remain > 4096 ? 4096 : remain;
    unsigned char *buf = static_cast<unsigned char *>(im.alloc_->alloc(probe));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_FAIL(read_at(im.fp_, col.page_off_, buf, probe))) {
    } else {
      Tr tr;
      tr.b_ = buf;
      tr.n_ = probe;
      int page_type = 0;
      int64_t uncomp = 0;
      int64_t comp = 0;
      int64_t nval = 0;
      int64_t nnull = 0;
      int64_t def_len = 0;
      int64_t rep_len = 0;
      int encoding = 0;
      if (OB_FAIL(parse_page_header(tr, page_type, uncomp, comp, nval, nnull, def_len, rep_len, encoding))) {
      } else if (3 != page_type) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "parquet page type");
      } else if (0 != encoding) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "parquet data encoding");
      } else {
        int64_t header_len = tr.i_;
        int64_t levels = def_len + rep_len;
        int64_t body = comp;
        if (levels > body) {
          body = levels + comp;
        }
        int64_t total = header_len + body;
        if (total > remain) {
          ret = OB_INVALID_DATA;
        } else {
          unsigned char *page = buf;
          if (total > probe) {
            page = static_cast<unsigned char *>(im.alloc_->alloc(total));
            if (OB_ISNULL(page)) {
              ret = OB_ALLOCATE_MEMORY_FAILED;
            } else if (OB_FAIL(read_at(im.fp_, col.page_off_, page, total))) {
            }
          }
          if (OB_SUCC(ret)) {
            const unsigned char *raw = page + header_len;
            int64_t raw_len = body;
            if (levels > 0 && levels <= raw_len) {
              raw += levels;
              raw_len -= levels;
            }
            Buf decoded;
            decoded.alloc_ = im.alloc_;
            const unsigned char *plain = raw;
            int64_t plain_len = raw_len;
            if (1 == col.codec_) {
              if (OB_FAIL(snappy_uncompress(raw, raw_len, decoded))) {
              } else {
                plain = reinterpret_cast<const unsigned char *>(decoded.p_);
                plain_len = decoded.len_;
              }
            } else if (0 != col.codec_) {
              ret = OB_NOT_SUPPORTED;
              LOG_USER_ERROR(OB_NOT_SUPPORTED, "parquet codec");
            }
            if (OB_SUCC(ret)) {
              if (nval > col.cap_) {
                col.vals_ = static_cast<ObObj *>(im.alloc_->alloc(sizeof(ObObj) * nval));
                if (OB_ISNULL(col.vals_)) {
                  ret = OB_ALLOCATE_MEMORY_FAILED;
                } else {
                  col.cap_ = nval;
                }
              }
              if (OB_SUCC(ret)) {
                col.bytes_len_ = 0;
                Buf store;
                store.alloc_ = im.alloc_;
                store.p_ = col.bytes_;
                store.len_ = 0;
                store.cap_ = col.bytes_cap_;
                for (int64_t i = 0; i < nval; ++i) {
                  new (col.vals_ + i) ObObj();
                  col.vals_[i].set_null();
                }
                if (0 == nnull) {
                  ret = decode_plain(plain, plain_len, col.physical_, nval, *im.alloc_, store, col.vals_);
                } else {
                  ret = OB_NOT_SUPPORTED;
                  LOG_USER_ERROR(OB_NOT_SUPPORTED, "parquet nulls");
                }
                col.bytes_ = store.p_;
                col.bytes_len_ = store.len_;
                col.bytes_cap_ = store.cap_;
                col.cnt_ = nval;
                col.cur_ = 0;
                col.page_off_ += total;
              }
            }
          }
        }
      }
    }
  }
  return ret;
}

ObFileTableScan::ObFileTableScan() : impl_(NULL) {}

ObFileTableScan::~ObFileTableScan()
{
  close();
  impl_ = NULL;
}

void ObFileTableScan::close()
{
  if (NULL != impl_) {
    if (NULL != impl_->fp_) {
      fclose(impl_->fp_);
      impl_->fp_ = NULL;
    }
    impl_->in_.fp_ = NULL;
    impl_->opened_ = false;
  }
}

int ObFileTableScan::open(ObIAllocator &alloc, const ObString &payload)
{
  int ret = OB_SUCCESS;
  close();
  if (NULL == impl_) {
    void *m = alloc.alloc(sizeof(Impl));
    if (OB_ISNULL(m)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      impl_ = new (m) Impl();
    }
  }
  if (OB_FAIL(ret)) {
  } else {
    impl_->alloc_ = &alloc;
    impl_->row_.alloc_ = &alloc;
    ObString owned;
    if (OB_FAIL(ob_write_string(alloc, payload, owned))) {
    } else if (OB_FAIL(parse_payload(alloc, owned, impl_->pay_))) {
    } else if (impl_->pay_.ncol_ > 0 &&
               OB_ISNULL(impl_->cells_ = static_cast<ObObj *>(alloc.alloc(sizeof(ObObj) * impl_->pay_.ncol_)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (FALSE_IT(({
                 for (int64_t ci = 0; ci < impl_->pay_.ncol_; ++ci) {
                   new (impl_->cells_ + ci) ObObj();
                 }
               }))) {
    } else if (KIND_GLOB == impl_->pay_.kind_) {
#ifndef _WIN32
      ObString z;
      glob_t g;
      memset(&g, 0, sizeof(g));
      if (OB_FAIL(ob_write_string(alloc, impl_->pay_.path_, z, true))) {
      } else {
        int rc = glob(z.ptr(), 0, NULL, &g);
        if (GLOB_NOMATCH == rc) {
          impl_->npath_ = 0;
          globfree(&g);
        } else if (0 != rc) {
          globfree(&g);
          ret = OB_FILE_NOT_EXIST;
        } else {
          impl_->paths_ = static_cast<char **>(alloc.alloc(sizeof(char *) * (g.gl_pathc + 1)));
          if (OB_ISNULL(impl_->paths_)) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          }
          int64_t n = 0;
          for (size_t i = 0; OB_SUCC(ret) && i < g.gl_pathc; ++i) {
            struct stat st;
            if (0 == stat(g.gl_pathv[i], &st) && S_ISDIR(st.st_mode)) {
            } else {
              ObString src(static_cast<int32_t>(strlen(g.gl_pathv[i])), g.gl_pathv[i]);
              ObString dst;
              if (OB_FAIL(ob_write_string(alloc, src, dst, true))) {
              } else {
                impl_->paths_[n++] = dst.ptr();
              }
            }
          }
          impl_->npath_ = n;
          globfree(&g);
        }
      }
#else
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "glob on windows");
#endif
    } else if (OB_FAIL(open_rb(alloc, impl_->pay_.path_, impl_->fp_))) {
    } else {
      impl_->in_.fp_ = impl_->fp_;
      impl_->in_.reset_buf();
      if (KIND_PARQUET == impl_->pay_.kind_) {
        unsigned char *footer = NULL;
        int64_t flen = 0;
        ObSEArray<PqCol, 4> pcols;
        int64_t nrows = 0;
        if (OB_FAIL(load_footer(impl_->fp_, alloc, footer, flen))) {
        } else if (OB_FAIL(parse_footer(footer, flen, alloc, pcols, nrows))) {
        } else if (pcols.count() != impl_->pay_.ncol_) {
          ret = OB_INVALID_DATA;
        } else if (OB_ISNULL(impl_->pcols_ = static_cast<PqCol *>(alloc.alloc(sizeof(PqCol) * pcols.count())))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          impl_->npq_ = pcols.count();
          for (int64_t i = 0; i < pcols.count(); ++i) {
            impl_->pcols_[i] = pcols.at(i);
          }
          impl_->pq_ready_ = true;
          if (0 != fseeko(impl_->fp_, 0, SEEK_SET)) {
            ret = OB_IO_ERROR;
          }
        }
      } else if (impl_->pay_.header_ && KIND_CSV == impl_->pay_.kind_) {
        Field fields[MAX_COLS];
        int64_t nfield = 0;
        bool got = false;
        ret = read_csv_row(impl_->in_, impl_->row_, fields, MAX_COLS, nfield, got);
      }
    }
    if (OB_SUCC(ret)) {
      impl_->opened_ = true;
    }
  }
  return ret;
}

int ObFileTableScan::rewind()
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(impl_) || !impl_->opened_) {
    ret = OB_ERR_UNEXPECTED;
  } else if (KIND_GLOB == impl_->pay_.kind_) {
    impl_->path_idx_ = 0;
  } else if (NULL == impl_->fp_ || 0 != fseeko(impl_->fp_, 0, SEEK_SET)) {
    ret = OB_IO_ERROR;
  } else {
    impl_->in_.fp_ = impl_->fp_;
    impl_->in_.reset_buf();
    impl_->row_.reset();
    if (KIND_PARQUET == impl_->pay_.kind_) {
      for (int64_t i = 0; i < impl_->npq_; ++i) {
        impl_->pcols_[i].page_off_ = impl_->pcols_[i].data_off_;
        impl_->pcols_[i].cur_ = 0;
        impl_->pcols_[i].cnt_ = 0;
      }
    } else if (impl_->pay_.header_ && KIND_CSV == impl_->pay_.kind_) {
      Field fields[MAX_COLS];
      int64_t nfield = 0;
      bool got = false;
      ret = read_csv_row(impl_->in_, impl_->row_, fields, MAX_COLS, nfield, got);
    }
  }
  return ret;
}

int ObFileTableScan::next()
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(impl_) || !impl_->opened_) {
    ret = OB_ERR_UNEXPECTED;
  } else if (KIND_GLOB == impl_->pay_.kind_) {
    if (impl_->path_idx_ >= impl_->npath_) {
      ret = OB_ITER_END;
    } else {
      ObString s;
      s.assign_ptr(impl_->paths_[impl_->path_idx_],
                   static_cast<int32_t>(strlen(impl_->paths_[impl_->path_idx_])));
      ++impl_->path_idx_;
      if (impl_->pay_.ncol_ < 1) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        impl_->cells_[0].set_varchar(s);
        impl_->cells_[0].set_collation_type(CS_TYPE_UTF8MB4_BIN);
        impl_->cells_[0].set_collation_level(CS_LEVEL_IMPLICIT);
      }
    }
  } else if (KIND_PARQUET == impl_->pay_.kind_) {
    if (!impl_->pq_ready_) {
      ret = OB_ERR_UNEXPECTED;
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < impl_->npq_; ++i) {
      PqCol &col = impl_->pcols_[i];
      if (col.cur_ >= col.cnt_) {
        if (OB_FAIL(load_pq_page(*impl_, col))) {
        }
      }
      if (OB_SUCC(ret)) {
        impl_->cells_[i] = col.vals_[col.cur_++];
      }
    }
  } else if (KIND_CSV == impl_->pay_.kind_) {
    Field fields[MAX_COLS];
    int64_t nfield = 0;
    bool got = false;
    if (OB_FAIL(read_csv_row(impl_->in_, impl_->row_, fields, MAX_COLS, nfield, got))) {
    } else if (!got) {
      ret = OB_ITER_END;
    } else {
      ret = fill_cells_from_fields(impl_->pay_, impl_->row_, fields, nfield, impl_->cells_);
    }
  } else if (KIND_JSON == impl_->pay_.kind_) {
    impl_->row_.reset();
    ObSEArray<Kv, 16> kvs;
    if (OB_FAIL(parse_json_object(impl_->in_, impl_->row_, kvs))) {
    } else {
      for (int64_t i = 0; i < impl_->pay_.ncol_; ++i) {
        impl_->cells_[i].set_null();
      }
      for (int64_t k = 0; OB_SUCC(ret) && k < kvs.count(); ++k) {
        int64_t idx = -1;
        for (int64_t i = 0; i < impl_->pay_.ncol_; ++i) {
          if (impl_->pay_.cols_[i].name_.length() == kvs.at(k).key_.length() &&
              NULL != kvs.at(k).key_.ptr() &&
              0 == memcmp(impl_->pay_.cols_[i].name_.ptr(), kvs.at(k).key_.ptr(), kvs.at(k).key_.length())) {
            idx = i;
            break;
          }
        }
        if (idx >= 0) {
          ret = cast_text(impl_->pay_.cols_[idx].type_, kvs.at(k).val_, kvs.at(k).is_null_, impl_->cells_[idx]);
        }
      }
    }
  } else {
    ret = OB_ERR_UNEXPECTED;
  }
  return ret;
}

int ObFileTableScan::get_cell(int64_t idx, ObObj &cell)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(impl_) || idx < 0 || idx >= impl_->pay_.ncol_ || OB_ISNULL(impl_->cells_)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    cell = impl_->cells_[idx];
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
