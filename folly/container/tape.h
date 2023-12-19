/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
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

#pragma once

#include <folly/Portability.h>
#include <folly/Range.h>
#include <folly/container/Iterator.h>
#include <folly/container/detail/tape_detail.h>
#include <folly/memory/UninitializedMemoryHacks.h>

#include <algorithm>
#include <cassert>
#include <initializer_list>
#include <iterator>
#include <numeric>
#include <string_view>
#include <type_traits>
#include <vector>

namespace folly {

#if FOLLY_HAS_RANGES
#define FOLLY_TAPE_CONTAINER_REQUIRES std::ranges::random_access_range
#else
#define FOLLY_TAPE_CONTAINER_REQUIRES
#endif

/* # Tape
 *
 * A container adapter, that builds a version of `vector<vector>` on top of a
 * random access underlying container.
 *
 * Instead of having a container of containers it's more efficient to have
 * a single container and store where the separators are.
 *
 *  >     >            >
 * [ word second word  thrid word ]
 *
 * One subrange of internal elements we call a `record`.
 *
 * You can `push` a new `record` or pop one from the back.
 * We also support an `erase` like `std::vector` but `insert` only for one
 * element. (there is no reason for limitation, except it's not implemented).
 *
 * NOTE: for when you don't have the `record` ready, you can use a
 * `record_builder` interface.
 *
 * Existing `records` can be accessed by index.
 *
 * Existing records cannot be mutated.
 *
 * NOTE: in theory there is nothing to prevent `tape<tape>` (and it could be
 * useful) but that doesn't work at the moment.
 *
 * ## PERFORMANCE CHARACTERISTICS (folly/container/test/tape_bench):
 *
 * If you know for a fact that all the elements are fitting into SSO buffer,
 * and you always have complete records (not building) then `tape` does not help
 * you, or can even be a regression.
 *
 * Otherwise tape can give you good speedups, especially if you need to
 * `push_back` on individual records.
 *
 * It is also much more cache friendly than vector of vectors,
 * which can be relevant in prod.
 *
 * ## NAME TAPE
 *
 * Name tape is taken from a lecture by Alexander Stepanov but we are not 100%
 * sure if this is the container he had in mind.
 */
template <FOLLY_TAPE_CONTAINER_REQUIRES Container>
class tape;

// string_tape - a common usecase.
using string_tape = tape<std::vector<char>>;

template <FOLLY_TAPE_CONTAINER_REQUIRES Container>
class tape {
  using ref_traits = detail::tape_reference_traits<Container>;

 public:
  using container_type = Container;

  using const_reference = typename ref_traits::reference;
  using reference = const_reference;

  // value_type for tape does not make much sense.
  // The best we found is to make reference type to be value type.
  // This does not quite make sense but works well enough.
  using value_type = const_reference;
  using scalar_value_type = detail::maybe_range_value_t<container_type>;

  using size_type = typename Container::size_type;
  using difference_type = typename Container::difference_type;

  using iterator = folly::index_iterator<const tape>;
  using const_iterator = iterator;
  using reverse_iterator = std::reverse_iterator<iterator>;
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;

  // concepts ------

  template <typename I>
  static constexpr bool iterator_of_scalars =
      std::is_convertible_v<iterator_value_type_t<I>, scalar_value_type>;

  template <typename I>
  static constexpr bool range_of_scalars =
      iterator_of_scalars<detail::maybe_range_const_iterator_t<I>>;

  template <typename I>
  static constexpr bool iterator_of_records =
      range_of_scalars<iterator_value_type_t<I>>;

  template <typename I>
  static constexpr bool range_of_records =
      iterator_of_records<detail::maybe_range_const_iterator_t<I>>;

  // constructors -----

  tape() = default;

  template <
      typename I,
      typename S,
      typename = std::enable_if_t<iterator_of_records<I>>>
  explicit tape(I f, S l) {
    range_constructor(f, l);
  }

  template <
      typename R,
      typename = std::enable_if_t<
          std::is_convertible_v<R, const_reference> || range_of_records<R>>>
  explicit tape(std::initializer_list<R> il) {
    range_constructor(il.begin(), il.end());
  }

  // access ------

  [[nodiscard]] const_reference operator[](size_type i) const {
    return ref_traits::make(
        data_.begin() + markers_[i], data_.begin() + markers_[i + 1]);
  }

  [[nodiscard]] bool empty() const { return size() == 0; }
  [[nodiscard]] size_type size() const { return markers_.size() - 1; }
  [[nodiscard]] size_type size_flat() const { return data_.size(); }

  [[nodiscard]] const_reference front() const { return operator[](0); }
  [[nodiscard]] const_reference back() const { return operator[](size() - 1); }

  // iterators ----

  [[nodiscard]] const_iterator begin() const { return {*this, 0}; }
  [[nodiscard]] const_iterator cbegin() const { return begin(); }

  [[nodiscard]] const_iterator end() const { return {*this, size()}; }
  [[nodiscard]] const_iterator cend() const { return end(); }

  [[nodiscard]] auto rbegin() const { return const_reverse_iterator{end()}; }
  [[nodiscard]] auto crbegin() const { return rbegin(); }

  [[nodiscard]] auto rend() const { return const_reverse_iterator{begin()}; }
  [[nodiscard]] auto crend() const { return rend(); }

  // push / emplace_back --------

  template <typename I, typename S>
  auto push_back(I f, S l) -> std::enable_if_t<iterator_of_scalars<I>> {
    data_.insert(data_.end(), f, l);
    markers_.push_back(static_cast<difference_type>(data_.size()));
  }

  template <typename R>
  auto push_back(R&& r) -> std::enable_if_t<
      range_of_scalars<R> &&
      !std::is_convertible_v<R, const_reference>> // !convertible to handle \0
  {
    push_back(std::begin(r), std::end(r));
  }

  void push_back(const_reference r) { push_back(r.begin(), r.end()); }

  void push_back(std::initializer_list<scalar_value_type> r) {
    push_back(r.begin(), r.end());
  }

  void emplace_back() { push_back({}); }

  template <typename... Args>
  void emplace_back(Args&&... args) {
    push_back(std::forward<decltype(args)>(args)...);
  }

  // push_back_unsafe --------
  // like push_back but requires you to have enough capacity for added range.
  // happened to give a 2x performance improvements on certain benchmarks.

  // requires to have enough capacity
  template <typename I, typename S>
  auto push_back_unsafe(I f, S l) -> std::enable_if_t<iterator_of_scalars<I>> {
    detail::append_range_unsafe(data_, f, l);
    markers_.push_back(static_cast<difference_type>(data_.size()));
  }

  template <typename R>
  auto push_back_unsafe(R&& r) -> std::enable_if_t<
      range_of_scalars<R> &&
      !std::is_convertible_v<R, const_reference>> // !convertible to handle \0
  {
    push_back_unsafe(std::begin(r), std::end(r));
  }

  void push_back_unsafe(const_reference r) {
    push_back_unsafe(r.begin(), r.end());
  }

  // record builder (constructing last record) -------

  class scoped_record_builder;
  [[nodiscard]] scoped_record_builder record_builder();

  // insert one record ----------

  template <typename I, typename S>
  auto insert(const_iterator pos, I f, S l)
      -> std::enable_if_t<iterator_of_scalars<I>, iterator>;

  template <std::ranges::input_range R>
  auto insert(const_iterator pos, R&& r) -> std::enable_if_t<
      range_of_scalars<R> && !std::is_convertible_v<R, const_reference>,
      iterator> {
    return insert(pos, std::ranges::begin(r), std::ranges::end(r));
  }

  iterator insert(
      const_iterator pos, std::initializer_list<scalar_value_type> r) {
    return insert(pos, r.begin(), r.end());
  }

  iterator insert(const_iterator pos, const_reference r) {
    return insert(pos, r.begin(), r.end());
  }

  // capacity ------

  void reserve(size_type records, size_type elements) {
    markers_.reserve(records + 1);
    data_.reserve(elements);
  }

  // assumes that 1 element per record. This is likely to help a bit.
  void reserve(size_type records) {
    markers_.reserve(records + 1);
    data_.reserve(records);
  }

  // resize/clear -------

  // same args as for push_back/emplace back are accepted
  template <typename... Args>
  void resize(size_type new_size, const Args&... args);

  void clear() {
    markers_.resize(1);
    data_.clear();
  }

  // erase -------

  void pop_back() {
    data_.resize(data_.size() - back().size());
    markers_.pop_back();
  }

  // note: same behaviour as for std::vector, erasing end() is UB
  iterator erase(const_iterator pos) {
    assert(pos != end());
    return erase(pos, pos + 1);
  }

  iterator erase(const_iterator f, const_iterator l);

  // ordering --------

  friend bool operator==(const tape& x, const tape& y) {
    return x.markers_ == y.markers_ && x.data_ == y.data_;
  }

  friend bool operator!=(const tape& x, const tape& y) { return !(x == y); }

  friend bool operator<(const tape& x, const tape& y) {
    return std::lexicographical_compare(x.begin(), x.end(), y.begin(), y.end());
  }

  friend bool operator>(const tape& x, const tape& y) { return y < x; }
  friend bool operator<=(const tape& x, const tape& y) { return !(y < x); }
  friend bool operator>=(const tape& x, const tape& y) { return !(x < y); }

 private:
  template <typename I, typename S>
  void range_constructor(I f, S l);

  // NOTE: using container difference_type might be too much here but,
  // on the other hand, there should be reasonably few items on the tape and
  // this makes interface simpler.
  std::vector<difference_type> markers_ = {0};
  container_type data_;
};

// Allows you an easy to construct a last record in a similar way
// you would an std::vector.
template <FOLLY_TAPE_CONTAINER_REQUIRES Container>
class tape<Container>::scoped_record_builder {
 public:
  scoped_record_builder(const scoped_record_builder&) = delete;
  scoped_record_builder(scoped_record_builder&&) = delete;
  scoped_record_builder& operator=(const scoped_record_builder&) = delete;
  scoped_record_builder& operator=(scoped_record_builder&&) = delete;

  using iterator = typename container_type::iterator;
  using const_iterator = typename container_type::const_iterator;
  using reference = std::iter_reference_t<iterator>;
  using const_reference = std::iter_reference_t<const_iterator>;
  using size_type = typename container_type::size_type;
  using difference_type = std::iter_difference_t<iterator>;

  // iterators -----

  [[nodiscard]] iterator begin() {
    return self_->data_.begin() + self_->markers_.back();
  }
  [[nodiscard]] const_iterator begin() const {
    return self_->data_.cbegin() + self_->markers_.back();
  }
  [[nodiscard]] const_iterator cbegin() const { return begin(); }

  [[nodiscard]] iterator end() { return self_->data_.end(); }
  [[nodiscard]] const_iterator end() const { return self_->data_.cend(); }
  [[nodiscard]] const_iterator cend() const { return end(); }

  // sometimes functions (like fmt) optimize for vector back inserter.
  // so better expose that.
  [[nodiscard]] auto back_inserter() {
    return std::back_inserter(self_->data_);
  }

  // access ---

  [[nodiscard]] bool empty() const { return begin() == end(); }

  [[nodiscard]] size_type size() const {
    return static_cast<size_type>(end() - begin());
  }

  [[nodiscard]] reference operator[](size_type i) {
    return begin()[static_cast<difference_type>(i)];
  }

  [[nodiscard]] const_reference operator[](size_type i) const {
    return begin()[static_cast<difference_type>(i)];
  }

  [[nodiscard]] reference back() { return self_->data_.back(); }
  [[nodiscard]] const_reference back() const { return self_->data_.back(); }

  // mutators ---

  void push_back(scalar_value_type x) { self_->data_.push_back(std::move(x)); }

  template <typename... Args>
  void emplace_back(Args&&... args) {
    self_->data_.emplace_back(std::forward<Args>(args)...);
  }

  ~scoped_record_builder() { self_->markers_.push_back(self_->data_.size()); }

 private:
  friend class tape;

  explicit scoped_record_builder(tape& self) : self_(&self) {}

  tape* self_;
};

template <FOLLY_TAPE_CONTAINER_REQUIRES Container>
auto tape<Container>::record_builder() -> scoped_record_builder {
  return scoped_record_builder{*this};
}

template <FOLLY_TAPE_CONTAINER_REQUIRES Container>
template <typename I, typename S>
void tape<Container>::range_constructor(I f, S l) {
  if constexpr (auto maybe = detail::compute_total_tape_len_if_possible(f, l);
                std::is_same_v<decltype(maybe), detail::fake_type>) {
    while (f != l) {
      push_back(*f);
      ++f;
    }
  } else {
    auto [nrecords, total_len] = maybe;
    reserve(nrecords, total_len);

    while (f != l) {
      push_back_unsafe(*f);
      ++f;
    }
  }
}

template <FOLLY_TAPE_CONTAINER_REQUIRES Container>
template <typename... Args>
void tape<Container>::resize(size_type new_size, const Args&... args) {
  if (new_size >= size()) {
    new_size -= size();
    while (new_size--) {
      emplace_back(args...);
    }
    return;
  }

  data_.resize(markers_[new_size]);
  markers_.resize(new_size + 1);
}

template <FOLLY_TAPE_CONTAINER_REQUIRES Container>
template <typename I, typename S>
auto tape<Container>::insert(const_iterator pos, I f, S l)
    -> std::enable_if_t<iterator_of_scalars<I>, iterator> {
  auto data_pos = data_.begin() + markers_[pos.get_index()];
  size_type old_size = data_.size();
  data_.insert(data_pos, f, l);

  auto inserted_len = static_cast<difference_type>(data_.size() - old_size);

  difference_type start = markers_[pos.get_index()];

  auto markers_tail =
      markers_.insert(markers_.begin() + pos.get_index(), start);
  ++markers_tail;

  std::transform(
      markers_tail, markers_.end(), markers_tail, [&](difference_type m) {
        return m + inserted_len;
      });

  // both tape* and index stayed the same
  return pos;
}

template <FOLLY_TAPE_CONTAINER_REQUIRES Container>
auto tape<Container>::erase(const_iterator f, const_iterator l) -> iterator {
  difference_type from = f.get_index();
  difference_type to = l.get_index();

  auto markers_f = markers_.begin() + from;
  auto markers_l = markers_.begin() + to;
  auto data_f = data_.begin() + *markers_f;
  auto data_l = data_.begin() + *markers_l;

  std::ptrdiff_t removed_length = data_l - data_f;
  std::transform(markers_l, markers_.end(), markers_l, [&](difference_type m) {
    return m - removed_length;
  });

  markers_.erase(markers_f, markers_l);
  data_.erase(data_f, data_l);

  // both tape* and index stayed the same
  return f;
}

#undef FOLLY_TAPE_CONTAINER_REQUIRES

} // namespace folly
