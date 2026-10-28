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

#define USING_LOG_PREFIX STORAGE_FTS

#include "lib/allocator/page_arena.h"
#include "storage/fts/ob_beng_ft_parser.h"
#include "data_plane/fts/ob_fts_struct.h"
#include "lib/utility/ob_macro_utils.h"

#include <new>

using namespace oceanbase::common;

namespace oceanbase
{
namespace storage
{

struct ObBEngParserTLS
{
  common::ObArenaAllocator pipeline_alloc;
  share::ObTextAnalysisCtx ctx;
  share::ObEnglishTextAnalyzer analyzer;
  const ObCharsetInfo *cs;
  ObBEngFTParser *parser;
  bool analyzer_ready;
  bool in_use;

  ObBEngParserTLS()
      : pipeline_alloc(lib::ObMemAttr("BengFtPipe")),
        ctx(),
        analyzer(),
        cs(nullptr),
        parser(nullptr),
        analyzer_ready(false),
        in_use(false)
  {
  }
};

ObBEngParserTLS *beng_parser_tls()
{
  static thread_local ObBEngParserTLS *tls = nullptr;
  if (OB_ISNULL(tls)) {
    tls = new (std::nothrow) ObBEngParserTLS();
  }
  return tls;
}

int bind_reused_beng_analyzer(const ObDatum &doc, share::ObITokenStream *&token_stream)
{
  int ret = OB_SUCCESS;
  ObBEngParserTLS *tls = beng_parser_tls();
  if (OB_ISNULL(tls)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (tls->analyzer_ready && tls->cs != tls->ctx.cs_) {
    tls->analyzer.reset();
    tls->analyzer_ready = false;
    tls->cs = nullptr;
  }
  if (OB_FAIL(ret)) {
  } else if (!tls->analyzer_ready) {
    if (OB_FAIL(tls->analyzer.init(tls->ctx, tls->pipeline_alloc))) {
      tls->analyzer_ready = false;
    } else {
      tls->cs = tls->ctx.cs_;
      tls->analyzer_ready = true;
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(tls->analyzer.analyze(doc, token_stream))) {
  }
  return ret;
}

int ObBEngFTParser::get_next_token(
    const char *&word,
    int64_t &word_len,
    int64_t &char_len,
    int64_t &word_freq)
{
  int ret = OB_SUCCESS;
  ObDatum token;
  int64_t token_freq = 0;
  char *buf = nullptr;
  word = nullptr;
  word_len = 0;
  char_len = 0;
  word_freq = 0;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
  } else if (OB_ISNULL(token_stream_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(token_stream_->get_next(token, token_freq))) {
    if (OB_ITER_END != ret) {
    }
  } else if (OB_ISNULL(token.ptr_) || OB_UNLIKELY(0 >= token.len_ || 0 >= token_freq)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(allocator_) || OB_ISNULL(buf = static_cast<char *>(allocator_->alloc(token.len_)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    MEMCPY(buf, token.ptr_, token.len_);
    word = buf;
    word_len = token.len_;
    char_len = token.len_;
    word_freq = token_freq;
    LOG_DEBUG("succeed to add word", K(ObString(word_len, word)), K(word_freq));
  }
  return ret;
}

int ObBEngFTParser::init(ObFTParserParam *param)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
  } else if (OB_ISNULL(param) || OB_UNLIKELY(!param->is_valid())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_UNLIKELY(UINT32_MAX < param->ft_length_)) {
    ret = OB_NOT_SUPPORTED;
  } else {
    doc_.set_string(param->fulltext_, param->ft_length_);
    allocator_ = param->allocator_;
    ObBEngParserTLS *tls = beng_parser_tls();
    if (nullptr != tls && this == tls->parser) {
      analysis_ctx_.cs_ = param->cs_;
      analysis_ctx_.filter_stopword_ = false;
      analysis_ctx_.need_grouping_ = false;
      tls->ctx.cs_ = param->cs_;
      tls->ctx.filter_stopword_ = false;
      tls->ctx.need_grouping_ = false;
      if (OB_FAIL(bind_reused_beng_analyzer(doc_, token_stream_))) {
      } else if (OB_ISNULL(token_stream_)) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        is_inited_ = true;
      }
    } else {
      analysis_ctx_.cs_ = param->cs_;
      analysis_ctx_.filter_stopword_ = false;
      analysis_ctx_.need_grouping_ = false;
      if (OB_FAIL(english_analyzer_.init(analysis_ctx_, *allocator_))) {
      } else if (OB_FAIL(segment(doc_, token_stream_))) {
      } else if (OB_ISNULL(token_stream_)) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        is_inited_ = true;
      }
    }
  }
  if (OB_FAIL(ret) && OB_UNLIKELY(!is_inited_)) {
    reset();
  }
  return ret;
}

int ObBEngFTParser::segment(
    const common::ObDatum &doc,
    share::ObITokenStream *&token_stream)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(doc.ptr_) || OB_UNLIKELY(0 >= doc.len_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_UNLIKELY(UINT32_MAX < doc.len_)) {
    ret = OB_NOT_SUPPORTED;
  } else if (OB_FAIL(english_analyzer_.analyze(doc, token_stream))) {
  }
  return ret;
}

void ObBEngFTParser::reset()
{
  analysis_ctx_.reset();
  english_analyzer_.reset();
  doc_.reset();
  token_stream_ = nullptr;
  is_inited_ = false;
}

ObBasicEnglishFTParserDesc::ObBasicEnglishFTParserDesc() {}

int ObBasicEnglishFTParserDesc::segment(
    ObFTParserParam *param,
    ObITokenIterator *&iter) const
{
  int ret = OB_SUCCESS;
  ObBEngFTParser *parser = nullptr;
  if (OB_ISNULL(param) || OB_ISNULL(param->fulltext_) || OB_UNLIKELY(!param->is_valid())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(beng_parser_tls())) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (!beng_parser_tls()->in_use) {
    ObBEngParserTLS &tls = *beng_parser_tls();
    if (OB_ISNULL(tls.parser)
        && OB_ISNULL(tls.parser = OB_NEWx(ObBEngFTParser, &tls.pipeline_alloc, tls.pipeline_alloc))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (FALSE_IT(tls.in_use = true)) {
    } else if (OB_FAIL(tls.parser->init(param))) {
      tls.parser->reset();
      tls.in_use = false;
    } else {
      iter = tls.parser;
    }
  } else if (OB_ISNULL(parser = OB_NEWx(ObBEngFTParser, param->allocator_, *(param->allocator_)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(parser->init(param))) {
  } else {
    iter = parser;
  }

  if (OB_FAIL(ret) && OB_NOT_NULL(parser)) {
    OB_DELETEx(ObBEngFTParser, param->allocator_, parser);
  }

  return ret;
}

void ObBasicEnglishFTParserDesc::free_token_iter(
    ObFTParserParam *param,
    ObITokenIterator *&iter) const
{
  if (OB_NOT_NULL(iter) && OB_NOT_NULL(beng_parser_tls()) && iter == beng_parser_tls()->parser
      && beng_parser_tls()->in_use) {
    static_cast<ObBEngFTParser *>(iter)->reset();
    beng_parser_tls()->in_use = false;
    iter = nullptr;
  } else if (OB_NOT_NULL(iter)) {
    abort_unless(nullptr != param);
    abort_unless(nullptr != param->allocator_);
    iter->~ObITokenIterator();
    param->allocator_->free(iter);
    iter = nullptr;
  }
}

int ObBasicEnglishFTParserDesc::get_add_word_flag(ObAddWordFlag &flag) const
{
  int ret = OB_SUCCESS;
  flag.set_min_max_word();
  flag.set_stop_word();
  flag.set_groupby_word();
  return ret;
}

} // end namespace storage
} // end namespace oceanbase
