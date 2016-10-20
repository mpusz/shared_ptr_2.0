#include <atomic>
#include <memory>
#include <type_traits>
#include <memory>
#include <ostream>

namespace experimental {

namespace detail {

template<typename A>
class alloc_guard {
public:
  using alloc_traits = std::allocator_traits<A>;
  using pointer = typename alloc_traits::pointer;

  explicit alloc_guard(A& alloc, pointer ptr) noexcept : alloc_{alloc}, ptr_{ptr} {}
  alloc_guard(const alloc_guard&) = delete;
  alloc_guard& operator=(const alloc_guard&) = delete;
  ~alloc_guard()
  {
    if(ptr_) {
      alloc_traits::deallocate(alloc_, ptr_, 1);
    }
  }
  void release() { ptr_ = nullptr; }

private:
  A& alloc_;
  pointer ptr_;
};

template<typename T, int Idx, bool UseEbo = !std::is_final<T>::value && std::is_empty<T>::value>
struct ebo_helper;

template<typename T, int Idx>
struct ebo_helper<T, Idx, true> : private T
{
  template<typename U>
  constexpr explicit ebo_helper(U&& t) : T{std::forward<U>(t)} {}
  constexpr T& get() { return *this; }
  constexpr const T& get() const { return *this; }
};

template<typename T, int Idx>
struct ebo_helper<T, Idx, false>
{
  template<typename U>
  constexpr explicit ebo_helper(U&& t) : t_{std::forward<U>(t)} {}
  constexpr T& get() { return t_; }
//  constexpr const T& get() const { return t_; }
private:
  T t_;
};

class state_base {
  virtual void release_ptr() noexcept = 0;
  virtual void destroy() noexcept = 0;

public:
  std::atomic_int shared_counter_{1};
  std::atomic_int weak_counter_{1};

  state_base() = default;
  state_base(const state_base&) = delete;
  state_base& operator=(const state_base&) = delete;
  virtual ~state_base() = default;

  void release()
  {
    if(--shared_counter_ == 0) {
      release_ptr();
      if(--weak_counter_ == 0) {
        destroy();
      }
    }
  }
};

template<typename Ptr,
         typename D = std::default_delete<std::remove_pointer_t<Ptr>>,
         typename A = std::allocator<std::remove_pointer_t<Ptr>>>
class state final : public state_base, private ebo_helper<D, 0>, private ebo_helper<A, 1> {
  using DBase = ebo_helper<D, 0>;
  using ABase = ebo_helper<A, 1>;
  Ptr ptr_;
  D& deleter() { return static_cast<DBase&>(*this).get(); }
  A& allocator() { return static_cast<ABase&>(*this).get(); }
public:
  using allocator_type = typename std::allocator_traits<A>::template rebind_alloc<state>;
  explicit state(Ptr ptr) noexcept : state{ptr, D{}, A{}} {}
  template<typename DD>
  state(Ptr ptr, DD&& d) noexcept : state{ptr, std::forward<DD>(d), A{}} {}
  template<typename DD, typename AA>
  state(Ptr ptr, DD&& d, AA&& a) noexcept : DBase{std::forward<DD>(d)}, ABase{std::forward<AA>(a)}, ptr_{ptr} {}

  void release_ptr() noexcept override { deleter()(ptr_); }
  void destroy() noexcept override
  {
    allocator_type alloc{allocator()};
    using alloc_traits = std::allocator_traits<allocator_type>;
    alloc_guard<allocator_type> guard{alloc, this};
    alloc_traits::destroy(alloc, this);
  }
};

class shared_state {
  state_base* base_ = nullptr;
public:
  shared_state() = default;

  template<typename Ptr>
  explicit shared_state(Ptr p) try : base_{new state<Ptr>{p}}
  {
  }
  catch(...) {
    delete p;
    throw;
  }

  template<typename Ptr, typename D>
  shared_state(Ptr p, D&& d) try : base_{new state<Ptr, D>{p, std::forward<D>(d)}}
  {
  }
  catch(...) {
    d(p);
    throw;
  }

  template<typename Ptr, typename D, typename A>
  shared_state(Ptr p, D&& d, A&& a) try
  {
    using state_type = state<Ptr, D, A>;
    using alloc_traits = std::allocator_traits<typename state_type::allocator_type>;

    typename state_type::allocator_type alloc{a};
    state_type* buffer = alloc_traits::allocate(alloc, 1);
    alloc_guard<typename state_type::allocator_type> guard{alloc, buffer};
    alloc_traits::construct(alloc, buffer, p, std::forward<D>(d), std::forward<A>(a));
    guard.release();
    base_ = buffer;
  }
  catch(...) {
    d(p);
    throw;
  }

  shared_state(const shared_state& other) noexcept : base_{other.base_}
  {
    if(base_) {
      ++base_->shared_counter_;
    }
  }

  shared_state(shared_state&& other) noexcept : base_{other.base_}
  {
    other.base_ = nullptr;
  }

  ~shared_state()
  {
    if(base_){
      base_->release();
    }
  }

  long use_count() const noexcept { return base_ ? base_->shared_counter_.load() : 0; }
};

}


template <typename T>
class weak_ptr;

template <class T>
class shared_ptr {
  template<typename U>
  using Convertible = std::enable_if_t<std::is_convertible<U, T*>::value>;

  T* ptr_ = nullptr;
  detail::shared_state state_;

  template<typename U> friend class shared_ptr;
  template<typename U> friend class weak_ptr;

public:
  using element_type = std::remove_extent_t<T>;
  using weak_type = weak_ptr<T>;

  // 20.11.2.2.1, constructors:
  constexpr shared_ptr() noexcept = default;

  template <class Y>
  explicit shared_ptr(Y* p) : ptr_{p}, state_{p}
  {
    static_assert(std::is_convertible<decltype(p), T*>::value, "p shall be convertible to T*");
    static_assert(!std::is_void<Y>::value, "Y shall be a complete type" );
    static_assert(sizeof(Y) > 0, "Y shall be a complete type" );
    static_assert(std::is_nothrow_destructible<decltype(p)>::value,
                  "The expression delete p shall not throw exceptions");

    // if (p != nullptr && p->weak_this.expired())
    //    p->weak_this = shared_ptr<remove_cv_t<Y>>(*this,
    //    const_cast<remove_cv_t<Y>*>(p));
  }

  template <class Y, class D>
  shared_ptr(Y* p, D d) : ptr_{p}, state_{p, std::move(d)}
  {
    static_assert(std::is_convertible<decltype(p), T*>::value, "p shall be convertible to T*");
    static_assert(std::is_copy_constructible<D>::value,
                  "D shall be CopyConstructible and such construction shall not throw exceptions");
//  static_assert(std::is_nothrow_copy_constructible(D),
//                "D shall be CopyConstructible and such construction shall not throw exceptions");
    static_assert(std::is_nothrow_destructible<D>::value, "The destructor of D shall not throw exceptions");

    // if (p != nullptr && p->weak_this.expired())
    //    p->weak_this = shared_ptr<remove_cv_t<Y>>(*this,
    //    const_cast<remove_cv_t<Y>*>(p));
  }

  template <class Y, class D, class A>
  shared_ptr(Y* p, D d, A a) : ptr_{p}, state_{p, std::move(d), std::move(a)}
  {
    static_assert(std::is_convertible<decltype(p), T*>::value, "p shall be convertible to T*");
    static_assert(std::is_copy_constructible<D>::value,
                  "D shall be CopyConstructible "
                  "and such construction shall "
                  "not throw exceptions");
    // static_assert(std::is_nothrow_copy_constructible(D), "D shall be
    // CopyConstructible and such construction shall not throw exceptions");
    static_assert(std::is_nothrow_destructible<D>::value, "The destructor of D shall not throw exceptions");
    static_assert(std::is_copy_constructible<A>::value,
                  "A shall be CopyConstructible "
                  "and such construction shall "
                  "not throw exceptions");
    //            static_assert(std::is_nothrow_copy_constructible(A), "A shall
    //            be CopyConstructible and such construction shall not throw
    //            exceptions");
    static_assert(std::is_nothrow_destructible<A>::value, "The destructor of A shall not throw exceptions");

    // if (p != nullptr && p->weak_this.expired())
    //    p->weak_this = shared_ptr<remove_cv_t<Y>>(*this,
    //    const_cast<remove_cv_t<Y>*>(p));
  }

  template <class D>
  shared_ptr(std::nullptr_t p, D d) : ptr_{p}, state_{p, std::move(d)}
  {
    static_assert(std::is_copy_constructible<D>::value,
                  "D shall be CopyConstructible "
                  "and such construction shall "
                  "not throw exceptions");
    // static_assert(std::is_nothrow_copy_constructible(D), "D shall be
    // CopyConstructible and such construction shall not throw exceptions");
    static_assert(std::is_nothrow_destructible<D>::value, "The destructor of D shall not throw exceptions");

  }

  template <class D, class A>
  shared_ptr(std::nullptr_t p, D d, A a) : ptr_{p}, state_{p, std::move(d), std::move(a)}
  {
    static_assert(std::is_copy_constructible<D>::value,
                  "D shall be CopyConstructible "
                  "and such construction shall "
                  "not throw exceptions");
    // static_assert(std::is_nothrow_copy_constructible(D), "D shall be
    // CopyConstructible and such construction shall not throw exceptions");
    static_assert(std::is_nothrow_destructible<D>::value, "The destructor of D shall not throw exceptions");
    static_assert(std::is_copy_constructible<A>::value,
                  "A shall be CopyConstructible "
                  "and such construction shall "
                  "not throw exceptions");
    //            static_assert(std::is_nothrow_copy_constructible(A), "A shall
    //            be CopyConstructible and such construction shall not throw
    //            exceptions");
    static_assert(std::is_nothrow_destructible<A>::value, "The destructor of A shall not throw exceptions");
  }

  template <class Y>
  shared_ptr(const shared_ptr<Y>& r, T* p) noexcept : ptr_(p), state_(r.state_)
  {
  }

  shared_ptr(const shared_ptr& r) noexcept = default;

  template <class Y, typename = Convertible<Y*>>
  shared_ptr(const shared_ptr<Y>& r) noexcept : ptr_(r.ptr_), state_{r.state_}
  {
  }

  shared_ptr(shared_ptr&& r) noexcept : ptr_{std::move(r.ptr_)}, state_{std::move(r.state_)}
  {
    r.ptr_ = nullptr;
  }

  template <class Y, typename = Convertible<Y*>>
  shared_ptr(shared_ptr<Y>&& r) noexcept : ptr_{std::move(r.ptr_)}, state_{std::move(r.state_)}
  {
    r.ptr_ = nullptr;
  }

  template <class Y>
  explicit shared_ptr(const weak_ptr<Y>& r) : ptr_{r.ptr_}, state_{r.state_}
  {
    static_assert(std::is_convertible<Y*, T*>::value, "Y shall be convertible to T*");
    if(r.expired) {
      throw std::bad_weak_ptr{};
    }
  }

  template <class Y, class D, typename = Convertible<typename std::unique_ptr<Y, D>::pointer>>
  shared_ptr(std::unique_ptr<Y, D>&& r)
  {
    if(r.get()) {
      // auto p = r.release;
      // if (p != nullptr && p->weak_this.expired())
      //    p->weak_this = shared_ptr<remove_cv_t<Y>>(*this,
      //    const_cast<remove_cv_t<Y>*>(p));

      if(std::is_reference<D>::value) {
        swap(shared_ptr{r.release(), std::ref(r.get_deleter())});
      }
      else {
        swap(shared_ptr{r.release(), r.get_deleter()});
      }
    }
  }

  constexpr shared_ptr(std::nullptr_t) noexcept : shared_ptr() {}

  // 20.11.2.2.2, destructor:
  ~shared_ptr() = default;

  // 20.11.2.2.3, assignment:
  shared_ptr& operator=(const shared_ptr& r) noexcept = default;
  template <class Y>
  shared_ptr& operator=(const shared_ptr<Y>& r) noexcept;
  shared_ptr& operator=(shared_ptr&& r) noexcept = default;
  template <class Y>
  shared_ptr& operator=(shared_ptr<Y>&& r) noexcept;
  template <class Y, class D>
  shared_ptr& operator=(std::unique_ptr<Y, D>&& r);
  // 20.11.2.2.4, modifiers:
  void swap(shared_ptr& r) noexcept
  {
    using std::swap;
    swap(state_, r.state_);
    swap(ptr_, r.ptr_);
  }
  void reset() noexcept;
  template <class Y>
  void reset(Y* p);
  template <class Y, class D>
  void reset(Y* p, D d);
  template <class Y, class D, class A>
  void reset(Y* p, D d, A a);
  // 20.11.2.2.5, observers:
  T* get() const noexcept { return ptr_; }
  T& operator*() const noexcept;
  T* operator->() const noexcept;
  long use_count() const noexcept { return state_.use_count(); }
  bool unique() const noexcept { return use_count() == 1; }
  explicit operator bool() const noexcept { return get() != nullptr; }
  template <class U>
  bool owner_before(shared_ptr<U> const& b) const;
  template <class U>
  bool owner_before(weak_ptr<U> const& b) const;
};

// 20.11.2.2.6, shared_ptr creation
template <class T, class... Args>
shared_ptr<T> make_shared(Args&&... args);
template <class T, class A, class... Args>
shared_ptr<T> allocate_shared(const A& a, Args&&... args);

// 20.11.2.2.7, shared_ptr comparisons:
template <class T, class U>
bool operator==(const shared_ptr<T>& a, const shared_ptr<U>& b) noexcept;
template <class T, class U>
bool operator!=(const shared_ptr<T>& a, const shared_ptr<U>& b) noexcept;
template <class T, class U>
bool operator<(const shared_ptr<T>& a, const shared_ptr<U>& b) noexcept;
template <class T, class U>
bool operator>(const shared_ptr<T>& a, const shared_ptr<U>& b) noexcept;
template <class T, class U>
bool operator<=(const shared_ptr<T>& a, const shared_ptr<U>& b) noexcept;
template <class T, class U>
bool operator>=(const shared_ptr<T>& a, const shared_ptr<U>& b) noexcept;
template <class T>
bool operator==(const shared_ptr<T>& a, nullptr_t) noexcept;
template <class T>
bool operator==(nullptr_t, const shared_ptr<T>& b) noexcept;
template <class T>
bool operator!=(const shared_ptr<T>& a, nullptr_t) noexcept;
template <class T>
bool operator!=(nullptr_t, const shared_ptr<T>& b) noexcept;
template <class T>
bool operator<(const shared_ptr<T>& a, nullptr_t) noexcept;
template <class T>
bool operator<(nullptr_t, const shared_ptr<T>& b) noexcept;
template <class T>
bool operator<=(const shared_ptr<T>& a, nullptr_t) noexcept;
template <class T>
bool operator<=(nullptr_t, const shared_ptr<T>& b) noexcept;
template <class T>
bool operator>(const shared_ptr<T>& a, nullptr_t) noexcept;
template <class T>
bool operator>(nullptr_t, const shared_ptr<T>& b) noexcept;
template <class T>
bool operator>=(const shared_ptr<T>& a, nullptr_t) noexcept;
template <class T>
bool operator>=(nullptr_t, const shared_ptr<T>& b) noexcept;
// 20.11.2.2.9, shared_ptr casts:
template <class T, class U>
shared_ptr<T> static_pointer_cast(const shared_ptr<U>& r) noexcept;
template <class T, class U>
shared_ptr<T> dynamic_pointer_cast(const shared_ptr<U>& r) noexcept;
template <class T, class U>
shared_ptr<T> const_pointer_cast(const shared_ptr<U>& r) noexcept;
// 20.11.2.2.10, shared_ptr get_deleter:
template <class D, class T>
D* get_deleter(const shared_ptr<T>& p) noexcept;
// 20.11.2.2.11, shared_ptr I/O:
template <class E, class T, class Y>
std::basic_ostream<E, T>& operator<<(std::basic_ostream<E, T>& os, const shared_ptr<Y>& p) { return os << p.get(); }

// 20.11.2.6, shared_ptr atomic access:
template <class T>
bool atomic_is_lock_free(const shared_ptr<T>* p);
template <class T>
shared_ptr<T> atomic_load(const shared_ptr<T>* p);
template <class T>
shared_ptr<T> atomic_load_explicit(const shared_ptr<T>* p, std::memory_order mo);
template <class T>
void atomic_store(shared_ptr<T>* p, shared_ptr<T> r);
template <class T>
void atomic_store_explicit(shared_ptr<T>* p, shared_ptr<T> r, std::memory_order mo);
template <class T>
shared_ptr<T> atomic_exchange(shared_ptr<T>* p, shared_ptr<T> r);
template <class T>
shared_ptr<T> atomic_exchange_explicit(shared_ptr<T>* p, shared_ptr<T> r, std::memory_order mo);
template <class T>
bool atomic_compare_exchange_weak(shared_ptr<T>* p, shared_ptr<T>* v, shared_ptr<T> w);
template <class T>
bool atomic_compare_exchange_strong(shared_ptr<T>* p, shared_ptr<T>* v, shared_ptr<T> w);
template <class T>
bool atomic_compare_exchange_weak_explicit(shared_ptr<T>* p, shared_ptr<T>* v, shared_ptr<T> w, std::memory_order success,
                                           std::memory_order failure);
template <class T>
bool atomic_compare_exchange_strong_explicit(shared_ptr<T>* p, shared_ptr<T>* v, shared_ptr<T> w, std::memory_order success,
                                             std::memory_order failure);
// 20.11.2.7 hash support
template <class T>
struct hash;
//template <class T, class D>
//struct hash<std::unique_ptr<T, D>>;
template <class T>
struct hash<shared_ptr<T>>;

}  // namespace experimental

namespace std {
// 20.11.2.2.8, shared_ptr specialized algorithms:
template <class T>
void swap(experimental::shared_ptr<T>& a, experimental::shared_ptr<T>& b) noexcept
{
  a.swap(b);
}
}