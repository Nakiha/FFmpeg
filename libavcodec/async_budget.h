/* Private asynchronous decode dispatch budget. LGPL-2.1-or-later. */
#ifndef AVCODEC_ASYNC_BUDGET_H
#define AVCODEC_ASYNC_BUDGET_H
#include "async.h"

AVCodecAsyncBudget *ff_async_budget_ref(AVCodecAsyncBudget *budget);
int ff_async_budget_default(AVCodecAsyncBudget **out);
int ff_async_budget_take(AVCodecAsyncBudget *budget);
void ff_async_budget_release(AVCodecAsyncBudget *budget, int ticket);
int ff_async_budget_slots(const AVCodecAsyncBudget *budget);
#endif
