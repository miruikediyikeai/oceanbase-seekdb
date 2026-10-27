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

#include "src/storage/fts/ob_ik_ft_parser.h"

#include "lib/allocator/page_arena.h"
#include "lib/charset/ob_charset.h"
#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/utility/ob_macro_utils.h"
#include "lib/utility/utility.h"

#include <new>
#include "data_plane/fts/ob_fts_struct.h"
#include "data_plane/fts/ob_fts_parser_helper.h"
#include "storage/fts/dict/ob_ft_dict.h"
#include "storage/fts/dict/ob_ft_dict_def.h"
#include "storage/fts/dict/ob_ft_dict_hub.h"
#include "storage/fts/dict/ob_ft_range_dict.h"
#include "storage/fts/ik/ob_ik_arbitrator.h"
#include "storage/fts/ik/ob_ik_cjk_processor.h"
#include "storage/fts/ik/ob_ik_letter_processor.h"
#include "storage/fts/ik/ob_ik_processor.h"
#include "storage/fts/ik/ob_ik_quantifier_processor.h"
#include "storage/fts/ik/ob_ik_surrogate_processor.h"

namespace oceanbase
{
namespace storage
{

struct ObIKParserTLS
{
  common::ObArenaAllocator meta_alloc;
  common::ObArenaAllocator doc_alloc;
  ObIKFTParser *parser;
  bool in_use;

  ObIKParserTLS()
      : meta_alloc(lib::ObMemAttr("IkFtMeta")),
        doc_alloc(lib::ObMemAttr("IkFtDoc")),
        parser(nullptr),
        in_use(false)
  {
  }
};

ObIKParserTLS *ik_parser_tls()
{
  static thread_local ObIKParserTLS *tls = nullptr;
  if (OB_ISNULL(tls)) {
    tls = new (std::nothrow) ObIKParserTLS();
  }
  return tls;
}

int ObIKFTParser::init(const ObFTParserParam &param)
{
  int ret = OB_SUCCESS;

  if (IS_INIT) {
    ret = OB_INIT_TWICE;
  } else {
    coll_type_ = ObCollationType::CS_TYPE_INVALID;
    if (OB_ISNULL(param.cs_) || OB_ISNULL(param.cs_->name)) {
      ret = OB_INVALID_ARGUMENT;
    } else if (CS_TYPE_INVALID == (coll_type_ = ObCharset::collation_type(param.cs_->name))) {
      ret = OB_INVALID_ARGUMENT;
    } else if (OB_FAIL(init_dict(param))) {
    } else if (OB_FAIL(init_ctx(param))) {
    } else if (OB_FAIL(init_segmenter(param))) {
    }

    if (OB_FAIL(ret)) {
      reset();
    } else {
      is_inited_ = true;
    }
  }

  return ret;
}

int ObIKFTParser::get_next_token(const char *&word,
                                 int64_t &word_len,
                                 int64_t &char_cnt,
                                 int64_t &word_freq)
{
  int ret = OB_SUCCESS;
  const char *output_word;
  int64_t len;
  int64_t offset;
  int64_t cnt;

  if (!IS_INIT) {
    ret = OB_NOT_INIT;
  } else {
    bool accept_token = false;
    while (OB_SUCC(ret) && !accept_token) {
      if (OB_FAIL(produce())) {
      } else if (OB_FAIL(ctx_->get_next_token(output_word, len, offset, cnt))) {
        if (OB_ITER_END == ret) {
          // ok, end this iter
        } else {
        }
      } else {
        bool is_stop = false;
        // if (!OB_ISNULL(dict_stop_)
        //     && OB_FAIL(dict_stop_->match(ObString(len, output_word + offset), is_stop))) {
        //   LOG_WARN("Failed to match stopwords", K(ret));
        // } else
        if (!is_stop) {
          word = output_word + offset;
          word_len = len;
          char_cnt = cnt;
          word_freq = 1;
          accept_token = true;
        } else {
        }
      }
    }
  }

  return ret;
}

int ObIKFTParser::produce()
{
  int ret = OB_SUCCESS;
  // Loop until end or has data to output
  while (OB_SUCC(ret) && ctx_->result_list().empty() && !ctx_->iter_end()) {
    if (OB_FAIL(process_next_batch())) {
      if (OB_ITER_END == ret) {
        // ok
      } else {
      }
    }
  }
  if (OB_ITER_END == ret) {
    ret = OB_SUCCESS;
  }
  return ret;
}

int ObIKFTParser::process_one_char(TokenizeContext &ctx,
                                   const char *ch,
                                   const uint8_t char_len,
                                   const ObFTCharUtil::CharType type)
{
  int ret = OB_SUCCESS;
  // proces by char with all segmenters
  if (doc_seg_cnt_ > 0) {
    for (int64_t i = 0; OB_SUCC(ret) && i < doc_seg_cnt_; ++i) {
      if (OB_ISNULL(doc_segs_[i])) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(doc_segs_[i]->process(ctx))) {
      }
    }
  } else {
    for (ObList<ObIIKProcessor *, ObIAllocator>::iterator iter = segmenters_.begin();
         OB_SUCC(ret) && iter != segmenters_.end();
         iter++) {
      if (OB_FAIL((*iter)->process(ctx))) {
      }
    }
  }
  return ret;
}

int ObIKFTParser::process_next_batch()
{
  int ret = OB_SUCCESS;
  ctx_->reset_resource();

  // handle next segmenter
  bool do_seg = false;

  if (ctx_->iter_end()) {
    ret = OB_ITER_END;
  } else {
    while (OB_SUCC(ret) && !do_seg && !ctx_->iter_end()) {
      const char *ch;
      uint8_t char_len = 0;
      ObFTCharUtil::CharType type = ObFTCharUtil::CharType::USELESS;
      if (OB_FAIL(ctx_->current_char(ch, char_len))) {
      } else if (OB_FAIL(ctx_->current_char_type(type))) {
      } else if (OB_FAIL(process_one_char(*ctx_, ch, char_len, type))) {
      } else {
        // 1. check segmention
        if (ctx_->handle_size() > SEGMENT_LIMIT && type == ObFTCharUtil::CharType::USELESS) {
          do_seg = true;
        }

        // 2. move to next;
        if (OB_FAIL(ctx_->step_next())) {
          if (OB_ITER_END == ret) {
          } else {
          }
        }
      } // end of one batch
    }

    if (OB_SUCC(ret) || OB_ITER_END == ret) {
      ObIKArbitrator arb;
      if (OB_FAIL(arb.process(*ctx_))) {
      } else if (OB_FAIL(arb.output_result(*ctx_))) {
      }
    } else {
      // Already logged.
    }
  }

  return ret;
}

int ObIKFTParserDesc::segment(ObFTParserParam *param, ObITokenIterator *&iter) const
{
  int ret = OB_SUCCESS;
  ObIKFTParser *parser = nullptr;
  ObFTDictHub *hub = nullptr;
  if (OB_ISNULL(param) || OB_UNLIKELY(!param->is_valid())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObFTParseData::instance().get_dict_hub(hub))) {
  } else if (OB_ISNULL(ik_parser_tls())) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (!ik_parser_tls()->in_use) {
    ObIKParserTLS &tls = *ik_parser_tls();
    if (OB_ISNULL(tls.parser)
        && OB_ISNULL(tls.parser = OB_NEWx(ObIKFTParser, &tls.meta_alloc, tls.meta_alloc, hub))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (FALSE_IT(tls.in_use = true)) {
    } else if (OB_FAIL(tls.parser->open_document(*param, tls.doc_alloc))) {
      tls.parser->finish_document();
      tls.doc_alloc.reuse();
      tls.in_use = false;
    } else {
      iter = tls.parser;
    }
  } else if (OB_ISNULL(parser = OB_NEWx(ObIKFTParser, param->allocator_, *(param->allocator_), hub))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(parser->init(*param))) {
  } else {
    iter = parser;
  }

  if (OB_FAIL(ret) && OB_NOT_NULL(parser)) {
    OB_DELETEx(ObIKFTParser, param->allocator_, parser);
  }

  return ret;
}

void ObIKFTParserDesc::free_token_iter(ObFTParserParam *param,
                                       ObITokenIterator *&iter) const
{
  if (OB_NOT_NULL(iter) && OB_NOT_NULL(ik_parser_tls()) && iter == ik_parser_tls()->parser
      && ik_parser_tls()->in_use) {
    static_cast<ObIKFTParser *>(iter)->finish_document();
    ik_parser_tls()->doc_alloc.reuse();
    ik_parser_tls()->in_use = false;
    iter = nullptr;
  } else if (OB_NOT_NULL(iter)) {
    iter->~ObITokenIterator();
    param->allocator_->free(iter);
    iter = nullptr;
  }
}


int ObIKFTParserDesc::get_add_word_flag(ObAddWordFlag &flag) const
{
  int ret = OB_SUCCESS;
  flag.set_casedown();
  flag.set_groupby_word();
  return ret;
}

int ObIKFTParser::init_dict(const ObFTParserParam &param)
{
  int ret = OB_SUCCESS;
  ObIFTDict *tmp_dict = nullptr;

  if (OB_ISNULL(hub_)) {
    ret = OB_ERR_UNEXPECTED;
  }

  ObFTRangeDict *dict = nullptr;
  ObFTDictDesc main_dict_desc("main_dict",
                              ObFTDictType::DICT_IK_MAIN,
                              ObCharsetType::CHARSET_UTF8MB4,
                              ObCollationType::CS_TYPE_UTF8MB4_BIN);

  ObFTDictDesc quan_dict_desc("quan_dict",
                              ObFTDictType::DICT_IK_QUAN,
                              ObCharsetType::CHARSET_UTF8MB4,
                              ObCollationType::CS_TYPE_UTF8MB4_BIN);

  ObFTDictDesc stopword_dict_desc("stopword",
                                  ObFTDictType::DICT_IK_STOP,
                                  ObCharsetType::CHARSET_UTF8MB4,
                                  ObCollationType::CS_TYPE_UTF8MB4_BIN);

  if (should_read_newest_table()) {
    // clear dict cache, always false now
  } else {
    if (OB_FAIL(init_single_dict(main_dict_desc, cache_main_))) {
    } else if (OB_FAIL(init_single_dict(quan_dict_desc, cache_quan_))) {
    } else if (OB_FAIL(init_single_dict(stopword_dict_desc, cache_stop_))) {
    }
  }

  if (OB_FAIL(ret)) {
    // already logged.
  } else if (OB_FAIL(build_dict_from_cache(main_dict_desc, cache_main_, dict_main_))) {
  } else if (OB_FAIL(build_dict_from_cache(quan_dict_desc, cache_quan_, dict_quan_))) {
  } else if (OB_FAIL(build_dict_from_cache(stopword_dict_desc, cache_stop_, dict_stop_))) {
  }

  return ret;
}

int ObIKFTParser::init_single_dict(ObFTDictDesc desc, ObFTCacheRangeContainer &container)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(hub_->load_cache(desc, container))) {
    if (OB_ENTRY_NOT_EXIST == ret) {
      if (OB_FAIL(hub_->build_cache(desc, container))) {
      }
    } else {
    }
  }
  return ret;
}

int ObIKFTParser::init_ctx(const ObFTParserParam &param)
{
  int ret = OB_SUCCESS;

  ObIAllocator &alloc = active_alloc();
  if (coll_type_ == common::CS_TYPE_INVALID) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(ctx_ = OB_NEWx(TokenizeContext,
                                      &alloc,
                                      coll_type_,
                                      alloc,
                                      param.fulltext_,
                                      param.ft_length_,
                                      param.ik_param_.mode_ == ObFTIKParam::Mode::SMART))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(ctx_->init())) {
  }
  if (OB_FAIL(ret)) {
    OB_DELETEx(TokenizeContext, &alloc, ctx_);
  }
  return ret;
}

int ObIKFTParser::init_segmenter(const ObFTParserParam &param)
{
  int ret = OB_SUCCESS;
  UNUSED(param);
  ObIAllocator &alloc = active_alloc();
  // do have an order
  ObIKLetterProcessor *letter_seg = nullptr;
  ObIKQuantifierProcessor *cnqsg = nullptr;
  ObIKCJKProcessor *cjksg = nullptr;
  ObIKSurrogateProcessor *surrogate_seg = nullptr;
  if (OB_ISNULL(letter_seg = OB_NEWx(ObIKLetterProcessor, &alloc))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_ISNULL(dict_quan_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(cnqsg = OB_NEWx(ObIKQuantifierProcessor, &alloc, *dict_quan_, alloc))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_ISNULL(dict_main_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(cjksg = OB_NEWx(ObIKCJKProcessor, &alloc, *dict_main_, alloc))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_ISNULL(surrogate_seg = OB_NEWx(ObIKSurrogateProcessor, &alloc))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    doc_segs_[0] = letter_seg;
    doc_segs_[1] = cnqsg;
    doc_segs_[2] = cjksg;
    doc_segs_[3] = surrogate_seg;
    doc_seg_cnt_ = 4;
    letter_seg = nullptr;
    cnqsg = nullptr;
    cjksg = nullptr;
    surrogate_seg = nullptr;
  }
  // quantifier is before cjk

  if (OB_FAIL(ret)) {
    OB_DELETEx(ObIKLetterProcessor, &alloc, letter_seg);
    OB_DELETEx(ObIKQuantifierProcessor, &alloc, cnqsg);
    OB_DELETEx(ObIKCJKProcessor, &alloc, cjksg);
    OB_DELETEx(ObIKSurrogateProcessor, &alloc, surrogate_seg);
  }
  return ret;
}

common::ObIAllocator &ObIKFTParser::active_alloc()
{
  return doc_allocator_ != nullptr ? *doc_allocator_ : allocator_;
}

void ObIKFTParser::free_doc_state()
{
  ObIAllocator &alloc = active_alloc();
  if (!OB_ISNULL(ctx_)) {
    ctx_->~TokenizeContext();
    alloc.free(ctx_);
    ctx_ = nullptr;
  }
  for (int64_t i = 0; i < doc_seg_cnt_; ++i) {
    if (!OB_ISNULL(doc_segs_[i])) {
      doc_segs_[i]->~ObIIKProcessor();
      alloc.free(doc_segs_[i]);
      doc_segs_[i] = nullptr;
    }
  }
  doc_seg_cnt_ = 0;
}

void ObIKFTParser::finish_document()
{
  free_doc_state();
  is_inited_ = false;
}

int ObIKFTParser::open_document(const ObFTParserParam &param, common::ObIAllocator &doc_alloc)
{
  int ret = OB_SUCCESS;
  doc_allocator_ = &doc_alloc;
  if (OB_ISNULL(param.cs_) || OB_ISNULL(param.cs_->name)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (CS_TYPE_INVALID == (coll_type_ = ObCharset::collation_type(param.cs_->name))) {
    ret = OB_INVALID_ARGUMENT;
  } else if (!dict_ready_) {
    if (OB_FAIL(init_dict(param))) {
      reset();
    } else {
      dict_ready_ = true;
    }
  }
  if (OB_FAIL(ret)) {
  } else if (FALSE_IT(free_doc_state())) {
  } else if (FALSE_IT(doc_allocator_ = &doc_alloc)) {
  } else if (OB_FAIL(init_ctx(param))) {
  } else if (OB_FAIL(init_segmenter(param))) {
  } else {
    is_inited_ = true;
  }
  if (OB_FAIL(ret)) {
    free_doc_state();
    is_inited_ = false;
  }
  return ret;
}

void ObIKFTParser::reset()
{
  free_doc_state();

  for (ObIIKProcessor *segmenter : segmenters_) {
    if (!OB_ISNULL(segmenter)) {
      segmenter->~ObIIKProcessor();
      allocator_.free(segmenter);
    }
  }
  segmenters_.clear();

  cache_main_.reset();
  cache_quan_.reset();
  cache_stop_.reset();

  if (!OB_ISNULL(dict_main_)) {
    dict_main_->~ObIFTDict();
    allocator_.free(dict_main_);
    dict_main_ = nullptr;
  }
  if (!OB_ISNULL(dict_quan_)) {
    dict_quan_->~ObIFTDict();
    allocator_.free(dict_quan_);
    dict_quan_ = nullptr;
  }
  if (!OB_ISNULL(dict_stop_)) {
    dict_stop_->~ObIFTDict();
    allocator_.free(dict_stop_);
    dict_stop_ = nullptr;
  }

  dict_ready_ = false;
  doc_allocator_ = nullptr;
  is_inited_ = false;
}

bool ObIKFTParser::should_read_newest_table() const { return false; }
int ObIKFTParser::build_dict_from_cache(const ObFTDictDesc &desc,
                                        ObFTCacheRangeContainer &container,
                                        ObIFTDict *&dict)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(hub_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(dict = OB_NEWx(ObFTRangeDict, &allocator_, allocator_, &container, desc))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(dict->init())) {
  }
  if (OB_FAIL(ret)) {
    OB_DELETEx(ObIFTDict, &allocator_, dict);
  }
  return ret;
}

} //  namespace storage
} //  namespace oceanbase
