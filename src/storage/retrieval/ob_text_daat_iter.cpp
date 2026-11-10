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

#define USING_LOG_PREFIX STORAGE

#include "ob_text_daat_iter.h"

namespace oceanbase
{
namespace storage
{
int ObTextDaaTIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRDaaTIterImpl::init(*param.base_param_, *param.dim_iters_,
                                            *param.allocator_, *param.relevance_collector_))) {
  } else if (OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
  } else {
    mode_flag_ = param.mode_flag_;
    function_lookup_mode_ = param.function_lookup_mode_;
    for (int64_t i = 0; i < iter_domain_ids_.count(); ++i) {
      iter_domain_ids_[i] = nullptr;
    }
  }
  return ret;
}

void ObTextDaaTIter::reset()
{
  bm25_param_estimator_.reset();
  // this class can know the type of dim_iters_, but ObSRDaaTIterImpl maybe not
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRDaaTIterImpl::reset();
}

void ObTextDaaTIter::reuse(const bool switch_tablet)
{
  if ((OB_NOT_NULL(dim_iters_) && 0 == dim_iters_->count())) {
    // do nothing
  } else {
    bm25_param_estimator_.reuse(switch_tablet);
  }
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reuse();
    }
  }
  ObSRDaaTIterImpl::reuse(switch_tablet);
  for (int64_t i = 0; i < iter_domain_ids_.count(); ++i) {
    iter_domain_ids_[i] = nullptr;
  }
}

int ObTextDaaTIter::get_next_rows(const int64_t capacity, int64_t &count)
{
  // A small natural-language union needs only the current posting of each
  // term. Merge them directly, keeping the simple merger's ID/term-index
  // ordering, without pushing and popping an item for every posting.
  const int64_t dim_count = nullptr == dim_iters_ ? 0 : dim_iters_->count();
  if (mode_flag_ != sql::ObMatchAgainstMode::NATURAL_LANGUAGE_MODE
      || dim_count == 0 || dim_count > ObSRSimpleMerger::USE_SIMPLE_MERGER_MAX_TABLE_CNT) {
    return ObSRDaaTIterImpl::get_next_rows(capacity, count);
  }
  int ret = OB_SUCCESS;
  count = 0;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (capacity == 0) {
    // No rows requested.
  } else if (iter_param_->limit_param_->is_valid()
      && output_row_cnt_ >= iter_param_->limit_param_->limit_) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(pre_process())) {
  } else {
    // These fixed buffers cannot grow during a merge batch. Resolve their
    // storage once instead of checking array indices for every posting.
    ObISRDaaTDimIter *const *dims = dim_iters_->get_data();
    const ObDatum **ids = iter_domain_ids_.get_data();
    int64_t *next_dims = next_round_iter_idxes_.get_data();
    const double *weights = iter_param_->dim_weights_ == nullptr
        ? nullptr : iter_param_->dim_weights_->get_data();
    for (int64_t i = 0; OB_SUCC(ret) && i < dim_count; ++i) {
      if (OB_ISNULL(dims[i])) {
        ret = OB_ERR_UNEXPECTED;
      }
    }
    const int64_t real_capacity = MIN(capacity, iter_param_->max_batch_size_);
    while (OB_SUCC(ret) && count < real_capacity) {
      for (int64_t i = 0; OB_SUCC(ret) && i < next_round_cnt_; ++i) {
        const int64_t idx = next_dims[i];
        if (OB_FAIL(dims[idx]->get_next_row())) {
          if (OB_ITER_END == ret) {
            ids[idx] = nullptr;
            ret = OB_SUCCESS;
          }
        } else if (OB_FAIL(dims[idx]->get_curr_id(ids[idx]))) {
        } else if (OB_ISNULL(ids[idx])) {
          ret = OB_ERR_UNEXPECTED;
        }
      }
      next_round_cnt_ = 0;
      ObSRMergeItem minimum;
      uint64_t minimum_dims = 0;
      for (int64_t i = 0; OB_SUCC(ret) && i < dim_count; ++i) {
        ObSRMergeItem candidate;
        candidate.iter_idx_ = i;
        int64_t cmp = 0;
        if (nullptr == ids[i]) {
        } else if (minimum.iter_idx_ < 0) {
          minimum.iter_idx_ = i;
          minimum_dims = 1ULL << i;
        } else if (OB_FAIL(merge_cmp_.cmp(candidate, minimum, cmp))) {
        } else if (cmp < 0) {
          minimum.iter_idx_ = i;
          minimum_dims = 1ULL << i;
        } else if (cmp == 0) {
          minimum_dims |= 1ULL << i;
        }
      }
      if (OB_SUCC(ret) && minimum.iter_idx_ < 0) {
        ret = OB_ITER_END;
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < dim_count; ++i) {
        double score = 0.0;
        if ((minimum_dims & (1ULL << i)) != 0) {
          if (OB_FAIL(dims[i]->get_curr_score(score))) {
          } else {
            if (nullptr != iter_param_->dim_weights_) {
              score = score * iter_param_->field_boost_ * weights[i];
            }
            if (OB_FAIL(relevance_collector_->collect_one_dim(i, score))) {
            } else {
              next_dims[next_round_cnt_++] = i;
            }
          }
        }
      }
      double relevance = 0.0;
      bool need_project = false;
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(relevance_collector_->get_result(relevance, need_project))) {
      } else if (need_project && OB_FAIL(filter_on_demand(count, relevance, need_project))) {
      } else if (need_project && OB_FAIL(cache_result(count, *ids[minimum.iter_idx_], relevance))) {
      }
    }
    if ((OB_SUCC(ret) || OB_ITER_END == ret) && count > 0) {
      const int project_ret = project_results(count);
      if (OB_SUCCESS != project_ret) {
        ret = project_ret;
      }
    }
  }
  return ret;
}

int ObTextDaaTIter::pre_process()
{
  int ret = OB_SUCCESS;
  if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (iter_param_->need_project_relevance()) {
    if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
    }
  }
  return ret;
}

ObTextBMWIter::ObTextBMWIter()
  : ObSRBMWIterImpl(),
    bm25_param_estimator_() {}

void ObTextBMWIter::reuse(const bool switch_tablet)
{
  bm25_param_estimator_.reuse(switch_tablet);
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reuse();
    }
  }
  ObSRBMWIterImpl::reuse(switch_tablet);
}

void ObTextBMWIter::reset()
{
  bm25_param_estimator_.reset();
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRBMWIterImpl::reset();
}

int ObTextBMWIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRBMWIterImpl::init(*param.base_param_, *param.dim_iters_,
                                           *param.allocator_, *param.relevance_collector_))) {
  } else if (OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
  }
  return ret;
}

int ObTextBMWIter::get_next_rows(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
  }

  if (FAILEDx(ObSRBMWIterImpl::get_next_rows(capacity, count))) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
    }
  }
  return ret;
}

int ObTextBMWIter::init_before_wand_process()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!bm25_param_estimator_.is_estimated())) {
    ret = OB_ERR_UNEXPECTED;
  }

  for (int64_t i = 0; OB_SUCC(ret) && i < dim_iters_->count(); ++i) {
    ObTextRetrievalBlockMaxIter *block_max_iter = static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i));
    if (OB_FAIL(block_max_iter->init_block_max_iter(
        bm25_param_estimator_.get_total_doc_cnt(), bm25_param_estimator_.get_avg_doc_token_cnt()))) {
    }
  }
  return ret;
}

} // namespace storage
} // namespace oceanbase
