/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <quic/common/BufUtil.h>

namespace quic {

Buf BufQueue::splitAtMost(size_t len) {
  folly::IOBuf* current = chain_.get();
  // empty queue / requested 0 bytes
  if (current == nullptr || len == 0) {
    return folly::IOBuf::create(0);
  }
  // entire chain requested
  if (len >= chainLength_) {
    return move();
  }

  chainLength_ -= len;
  Buf result;
  /**
   * Find the last IOBuf containing range requested. This will definitively
   * terminate without looping back to chain_ since we know chainLength_ > len.
   */
  while (len != 0) {
    if (current->length() > len) {
      break;
    }
    len -= current->length();
    current = current->next();
  }

  if (len == 0) {
    // edge case if last chunk ended exactly "len" bytes; we know this can't be
    // the last IOBuf in the list since otherwise len >= chainLength_
    result = current->separateChain(chain_.get(), current->prev());
  } else {
    // clone current node and remove overlap b/n result & chain_
    result = current->cloneOne();
    result->trimEnd(current->length() - len);
    current->trimStart(len);

    // if current isn't head node, move all prior nodes into result
    if (current != chain_.get()) {
      result->appendToChain(
          current->separateChain(chain_.get(), current->prev()));
      result = Buf(result.release()->next());
    }
  }
  // update chain_
  chain_.release();
  chain_ = std::unique_ptr<folly::IOBuf>(current);
  DCHECK_EQ(chainLength_, chain_ ? chain_->computeChainDataLength() : 0);
  return result;
}

size_t BufQueue::trimStartAtMost(size_t amount) {
  auto original = amount;
  folly::IOBuf* current = chain_.get();
  if (current == nullptr || amount == 0) {
    return 0;
  }
  while (amount > 0) {
    if (current->length() >= amount) {
      current->trimStart(amount);
      amount = 0;
      break;
    }
    amount -= current->length();
    current = current->next();
    if (current == chain_.get()) {
      break;
    }
  }
  auto prev = current->prev();
  /** We are potentially in 2 states here,
   * 1. we found the entire amount
   * 2. or we did not.
   * If we did not find the entire amount, then current ==
   * chain and we can remove the entire chain.
   * If we did, then we can split from the chain head to the previous buffer and
   * then keep the current buffer.
   */
  if (prev != current && current != chain_.get()) {
    auto chain = chain_.release();
    current->separateChain(chain, prev);
    chain_ = std::unique_ptr<folly::IOBuf>(current);
  } else if (amount > 0) {
    DCHECK_EQ(current, chain_.get());
    chain_ = nullptr;
  }
  size_t trimmed = original - amount;
  DCHECK_GE(chainLength_, trimmed);
  chainLength_ -= trimmed;
  DCHECK(chainLength_ == 0 || !chain_->empty());
  return trimmed;
}

// TODO replace users with trimStartAtMost
void BufQueue::trimStart(size_t amount) {
  auto trimmed = trimStartAtMost(amount);
  if (trimmed != amount) {
    throw std::underflow_error(
        "Attempt to trim more bytes than are present in BufQueue");
  }
}

void BufQueue::append(Buf&& buf) {
  if (!buf || buf->empty()) {
    return;
  }
  chainLength_ += buf->computeChainDataLength();
  appendToChain(chain_, std::move(buf));
}

void BufQueue::appendToChain(Buf& dst, Buf&& src) {
  if (dst == nullptr) {
    dst = std::move(src);
  } else {
    dst->prependChain(std::move(src));
  }
}

BufAppender::BufAppender(folly::IOBuf* data, size_t appendLen)
    : crtBuf_(CHECK_NOTNULL(data)), head_(data), appendLen_(appendLen) {}

void BufAppender::push(const uint8_t* data, size_t len) {
  if (crtBuf_->tailroom() < len || lastBufShared_) {
    auto newBuf = folly::IOBuf::createCombined(std::max(appendLen_, len));
    folly::IOBuf* newBufPtr = newBuf.get();
    head_->prependChain(std::move(newBuf));
    crtBuf_ = newBufPtr;
  }
  memcpy(crtBuf_->writableTail(), data, len);
  crtBuf_->append(len);
  lastBufShared_ = false;
}

void BufAppender::insert(std::unique_ptr<folly::IOBuf> data) {
  // just skip the current buffer and append it to the end of the current
  // buffer.
  folly::IOBuf* dataPtr = data.get();
  // If the buffer is shared we do not want to overrwrite the tail of the
  // buffer.
  lastBufShared_ = data->isShared();
  head_->prependChain(std::move(data));
  crtBuf_ = dataPtr;
}

BufWriter::BufWriter(folly::IOBuf& iobuf, size_t most)
    : iobuf_(iobuf), most_(most) {
  CHECK(iobuf_.tailroom() >= most_)
      << "Buffer room=" << iobuf_.tailroom() << " limit=" << most_;
}

void BufWriter::push(const uint8_t* data, size_t len) {
  sizeCheck(len);
  memcpy(iobuf_.writableTail(), data, len);
  iobuf_.append(len);
  written_ += len;
}

void BufWriter::insert(const folly::IOBuf* data) {
  auto totalLength = data->computeChainDataLength();
  insert(data, totalLength);
}

void BufWriter::insert(const folly::IOBuf* data, size_t limit) {
  copy(data, limit);
}

void BufWriter::append(size_t len) {
  iobuf_.append(len);
  written_ += len;
  appendCount_ += len;
}

void BufWriter::copy(const folly::IOBuf* data, size_t limit) {
  if (!limit) {
    return;
  }
  sizeCheck(limit);
  size_t totalInserted = 0;
  const folly::IOBuf* curBuf = data;
  auto remaining = limit;
  do {
    auto lenToCopy = std::min(curBuf->length(), remaining);
    push(curBuf->data(), lenToCopy);
    totalInserted += lenToCopy;
    remaining -= lenToCopy;
    if (lenToCopy < curBuf->length()) {
      break;
    }
    curBuf = curBuf->next();
  } while (remaining && curBuf != data);
  CHECK_GE(limit, totalInserted);
}

void BufWriter::backFill(const uint8_t* data, size_t len, size_t destOffset) {
  CHECK_GE(appendCount_, len);
  appendCount_ -= len;
  CHECK_LE(destOffset + len, iobuf_.length());
  memcpy(iobuf_.writableData() + destOffset, data, len);
}
} // namespace quic
