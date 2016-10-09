// FreeTTCN is a free compiler and execution environment for TTCN-3 language.
//
// Copyright (C) 2016 Mateusz Pusz
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "shared_ptr_2.h"
#include <gtest/gtest.h>

template<typename T>
using shared_ptr = experimental::shared_ptr<T>;

namespace {

struct A {};
struct B : A {};

struct test_state {
  int deleter_count = 0;
  int allocated_bytes = 0;
  int deallocated_bytes = 0;
};

template <typename T>
struct test_deleter {
  test_state* state = 0;

  void operator()(T* ptr)
  {
    delete ptr;
    if(state) {
      ++state->deleter_count;
    }
  }
};

template <typename T>
struct test_allocator {
  test_state* state = 0;

  using value_type = T;

  test_allocator() = default;
  test_allocator(test_state* s) noexcept : state{s} {}
  template <class U> test_allocator(const test_allocator<U>& other) noexcept : state{other.state} {}

  T* allocate(std::size_t n)
  {
    if(state) {
      state->allocated_bytes += n;
    }
    return static_cast<T*>(::operator new(n * sizeof(T)));
  }
  void deallocate(T* p, std::size_t n)
  {
    if(state) {
      state->deallocated_bytes += n;
    }
    ::operator delete(p);
  }
};

template <class T1, class T2>
bool operator==(const test_allocator<T1>& lhs, const test_allocator<T2>& rhs)
{
  return true;
}

template <class T1, class T2>
bool operator!=(const test_allocator<T1>& lhs, const test_allocator<T2>& rhs)
{
  return std::rel_ops::operator!=(lhs, rhs);
}
}


TEST(shared_ptr, defaultConstructor)
{
  shared_ptr<A> ptr;
  EXPECT_EQ(0, ptr.use_count());
  EXPECT_EQ(nullptr, ptr.get());
}

TEST(shared_ptr, constructorPtr)
{
  A* p = new A;
  shared_ptr<A> ptr{p};
  EXPECT_EQ(1, ptr.use_count());
  EXPECT_EQ(p, ptr.get());
}

TEST(shared_ptr, constructorPtrDeleter)
{
  test_state state;
  test_deleter<A> deleter{&state};
  {
    A* p = new A;
    shared_ptr<A> ptr{p, deleter};
    EXPECT_EQ(1, ptr.use_count());
    EXPECT_EQ(p, ptr.get());
    EXPECT_EQ(0, state.deleter_count);
  }
  EXPECT_EQ(1, state.deleter_count);
}

TEST(shared_ptr, constructorPtrDeleterAllocator)
{
  test_state state;
  test_allocator<A> allocator{&state};
  test_deleter<A> deleter{&state};
  {
    A* p = new A;
    shared_ptr<A> ptr{p, deleter, allocator};
    EXPECT_EQ(1, ptr.use_count());
    EXPECT_EQ(p, ptr.get());
    EXPECT_EQ(0, state.deleter_count);
    EXPECT_NE(0, state.allocated_bytes);
    EXPECT_EQ(0, state.deallocated_bytes);
  }
  EXPECT_EQ(1, state.deleter_count);
  EXPECT_NE(0, state.allocated_bytes);
  EXPECT_EQ(state.allocated_bytes, state.deallocated_bytes);
  std::cout << "Allocated size: " << state.allocated_bytes << "\n";
}

TEST(shared_ptr, constructorNullptrDeleter)
{
  test_state state;
  test_deleter<A> deleter{&state};
  {
    shared_ptr<A> ptr{nullptr, deleter};
    EXPECT_EQ(1, ptr.use_count());
    EXPECT_EQ(nullptr, ptr.get());
    EXPECT_EQ(0, state.deleter_count);
  }
  EXPECT_EQ(1, state.deleter_count);
}

TEST(shared_ptr, constructorNullptrDeleterAllocator)
{
  test_state state;
  test_allocator<A> allocator{&state};
  test_deleter<A> deleter{&state};
  {
    shared_ptr<A> ptr{nullptr, deleter, allocator};
    EXPECT_EQ(1, ptr.use_count());
    EXPECT_EQ(nullptr, ptr.get());
    EXPECT_EQ(0, state.deleter_count);
    EXPECT_NE(0, state.allocated_bytes);
    EXPECT_EQ(0, state.deallocated_bytes);
  }
  EXPECT_EQ(1, state.deleter_count);
  EXPECT_NE(0, state.allocated_bytes);
  EXPECT_EQ(state.allocated_bytes, state.deallocated_bytes);
  std::cout << "Allocated size: " << state.allocated_bytes << "\n";
}

TEST(shared_ptr, constructorAliasing)
{
  shared_ptr<B> p1{new B};
  shared_ptr<B> p2{p1};
  int val;
  shared_ptr<int> ptr(p1, &val);
  EXPECT_EQ(&val, ptr.get());
  EXPECT_EQ(p1.use_count(), ptr.use_count());
}

TEST(shared_ptr, constructorAliasingNull)
{
  shared_ptr<B> p1{new B};
  shared_ptr<B> p2{p1};
  shared_ptr<A> ptr(p1, nullptr);
  EXPECT_EQ(nullptr, ptr.get());
  EXPECT_FALSE(ptr);
  EXPECT_EQ(p1.use_count(), ptr.use_count());
}

TEST(shared_ptr, constructorAliasingEmpty)
{
  shared_ptr<B> p1;
  int val;
  shared_ptr<int> ptr(p1, &val);
  EXPECT_EQ(&val, ptr.get());
  EXPECT_TRUE(static_cast<bool>(ptr));
  EXPECT_EQ(0, p1.use_count());
  EXPECT_EQ(p1.use_count(), ptr.use_count());
}

// add tests for copy-construction/assignment to aliasing ptr

TEST(shared_ptr, copyConstructor)
{
  shared_ptr<A> p{new A};
  shared_ptr<A> ptr{p};
  EXPECT_EQ(p.get(), ptr.get());
  EXPECT_EQ(p.use_count(), ptr.use_count());
  EXPECT_EQ(2, ptr.use_count());
}

TEST(shared_ptr, copyConstructorEmpty)
{
  shared_ptr<A> p;
  shared_ptr<A> ptr{p};
  EXPECT_EQ(p.get(), ptr.get());
  EXPECT_EQ(p.use_count(), ptr.use_count());
  EXPECT_EQ(0, ptr.use_count());
}

TEST(shared_ptr, copyConstructorOtherType)
{
  shared_ptr<B> p{new B};
  shared_ptr<A> ptr{p};
  EXPECT_EQ(p.get(), ptr.get());
  EXPECT_EQ(p.use_count(), ptr.use_count());
  EXPECT_EQ(2, ptr.use_count());
}

TEST(shared_ptr, copyConstructorOtherTypeEmpty)
{
  shared_ptr<B> p;
  shared_ptr<A> ptr{p};
  EXPECT_EQ(p.get(), ptr.get());
  EXPECT_EQ(p.use_count(), ptr.use_count());
  EXPECT_EQ(0, ptr.use_count());
}

TEST(shared_ptr, moveConstructor)
{
  A* p1 = new A;
  shared_ptr<A> p{p1};
  shared_ptr<A> ptr{std::move(p)};
  EXPECT_EQ(p1, ptr.get());
  EXPECT_EQ(nullptr, p.get());
  EXPECT_EQ(0, p.use_count());
  EXPECT_FALSE(p);
  EXPECT_EQ(1, ptr.use_count());
}

TEST(shared_ptr, moveConstructorEmpty)
{
  shared_ptr<A> p;
  shared_ptr<A> ptr{std::move(p)};
  EXPECT_EQ(nullptr, ptr.get());
  EXPECT_EQ(nullptr, p.get());
  EXPECT_EQ(0, p.use_count());
  EXPECT_FALSE(p);
  EXPECT_EQ(0, ptr.use_count());
}

TEST(shared_ptr, moveConstructorOtherType)
{
  B* p1 = new B;
  shared_ptr<B> p{p1};
  shared_ptr<A> ptr{std::move(p)};
  EXPECT_EQ(p1, ptr.get());
  EXPECT_EQ(nullptr, p.get());
  EXPECT_EQ(0, p.use_count());
  EXPECT_FALSE(p);
  EXPECT_EQ(1, ptr.use_count());
}

TEST(shared_ptr, moveConstructorOtherTypeEmpty)
{
  shared_ptr<B> p;
  shared_ptr<A> ptr{std::move(p)};
  EXPECT_EQ(p.get(), ptr.get());
  EXPECT_EQ(nullptr, p.get());
  EXPECT_EQ(0, p.use_count());
  EXPECT_FALSE(p);
  EXPECT_EQ(0, ptr.use_count());
}

TEST(shared_ptr, DISABLED_constructorFromWeak)
{
//  shared_ptr<B> p1{new B};
//  experimental::weak_ptr<B> p2{p1};
//  shared_ptr<A> ptr{p2};
//  ptr.use_count() == p2.use_count();
}

//void foo()
//{
//  {
//    experimental::weak_ptr<B> p;
//    {
//      shared_ptr<B> p1{new B};
//      p = p1;
//    }
//    EXPECT_THROW(shared_ptr<A> ptr{p}, std::bad_weak_ptr);
//  }
//  {
//    std::unique_ptr<B> p1;
//    shared_ptr<A> ptr{p1};
//    EXPECT_EQ(0, ptr.use_count());
//    EXPECT_EQ(nullptr, ptr.get());
//  }
//  {
//    std::unique_ptr<B> p1{new B};
//    shared_ptr<A> ptr{p1};
//    ptr.use_count() == p2.use_count();
//    // test for different deleters
//  }
//}
