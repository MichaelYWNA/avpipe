// SPDX-License-Identifier: MIT
#ifndef AVPIPE_HANDLE_HPP
#define AVPIPE_HANDLE_HPP

#include <utility>

namespace avpipe {

/// `Ptr` is a move-only owner for a libav object.
///
/// libav frees most objects through `f(T**)` (`av_frame_free`, `avcodec_free_context`, ...)
/// but a few through `f(T*)` (`sws_freeContext`). Rather than special-casing those call sites,
/// the deleter is a policy with one `release(T*&)` member and the two policies below adapt the
/// two libav signatures.

/// Policy for `void f(T**)`, the dominant libav convention.
template <class T, void (*Fn)(T**)>
struct free_via_pointer_to_pointer {
  static void release(T*& p) {
    if (p != nullptr) {
      Fn(&p);
      p = nullptr;
    }
  }
};

/// Policy for `void f(T*)` (e.g. `sws_freeContext`).
template <class T, void (*Fn)(T*)>
struct free_via_pointer {
  static void release(T*& p) {
    if (p != nullptr) {
      Fn(p);
      p = nullptr;
    }
  }
};

template <class T, class Deleter>
class Ptr {
 public:
  Ptr() = default;
  explicit Ptr(T* raw) : raw_(raw) {}
  ~Ptr() { Deleter::release(raw_); }

  Ptr(Ptr&& other) noexcept : raw_(other.raw_) { other.raw_ = nullptr; }
  Ptr& operator=(Ptr&& other) noexcept {
    if (this != &other) {
      Deleter::release(raw_);
      raw_ = other.raw_;
      other.raw_ = nullptr;
    }
    return *this;
  }

  Ptr(const Ptr&) = delete;
  Ptr& operator=(const Ptr&) = delete;

  T* get() const { return raw_; }
  T* operator->() const { return raw_; }
  explicit operator bool() const { return raw_ != nullptr; }

  /// Address of the internal pointer, for libav functions that need `T**` to allocate.
  T** out() { return &raw_; }

  T* release() {
    T* p = raw_;
    raw_ = nullptr;
    return p;
  }

  void reset(T* raw = nullptr) {
    if (raw != raw_) {
      Deleter::release(raw_);
      raw_ = raw;
    }
  }

 private:
  T* raw_ = nullptr;
};

}  // namespace avpipe

#endif  // AVPIPE_HANDLE_HPP
