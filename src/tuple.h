/*
 * Copyright 2019 SiFive, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You should have received a copy of LICENSE.Apache2 along with
 * this software. If not, you may obtain a copy at
 *
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef TUPLE_H
#define TUPLE_H

#include "runtime.h"
#include <vector>

struct Location;
struct Constructor;

struct alignas(PadObject) Promise {
  explicit operator bool() const {
    HeapObject *obj = value.get();
    return obj && !obj->is_work();
  }

  void await(Runtime &runtime, Continuation *c) const {
    if (*this) {
      c->resume(runtime, value.get());
    } else {
      c->next = static_cast<Continuation*>(value.get());
      value = c;
    }
  }

  // Use only if the value is known to already be available 
  template <typename T>
  T *coerce() { return static_cast<T*>(value.get()); }
  template <typename T>
  const T *coerce() const { return static_cast<const T*>(value.get()); }

  // Call once only!
  void fulfill(Runtime &runtime, HeapObject *obj);
  // Call only if the containing tuple was just constructed (no Continuations)
  void instant_fulfill(HeapObject *obj) {
#ifdef DEBUG_GC
     assert(!value);
     assert(!obj->is_work());
#endif
     value = obj;
  }

  template <typename T, T (HeapPointerBase::*memberfn)(T x)>
  T recurse(T arg) { return (value.*memberfn)(arg); }

private:
  mutable HeapPointer<HeapObject> value;
friend struct Tuple;
};

template <>
inline HeapStep Promise::recurse<HeapStep, &HeapPointerBase::explore>(HeapStep step) {
  if (*this) return value.explore(step);
  step.broken = this;
  return step;
}

struct Tuple : public HeapObject {
  virtual size_t size() const = 0;
  virtual Promise *at(size_t i) = 0;
  virtual const Promise *at(size_t i) const = 0;

  bool empty() const { return size() == 0; }

  static const size_t fulfiller_pads;
  Continuation *claim_fulfiller(Runtime &r, size_t i);

  void claim_instant_fulfiller(Runtime &r, size_t i, Promise *p) {
    if (*p) {
      at(i)->instant_fulfill(p->coerce<HeapObject>());
    } else {
      Continuation *cont = claim_fulfiller(r, i);
      cont->next = p->value;
      p->value = cont;
    }
  }
};

struct Record : public Tuple {
  Constructor *cons;

  Record(Constructor *cons_) : cons(cons_) { }

  void format(std::ostream &os, FormatState &state) const override;
  Hash hash() const override;

  static size_t reserve(size_t size);
  static Record *claim(Heap &h, Constructor *cons, size_t size); // requires prior h.reserve
  static Record *alloc(Heap &h, Constructor *cons, size_t size);
};

struct ScopeStack;
struct Scope : public Tuple {
  HeapPointer<Scope> next;

  Scope(Scope *next_) : next(next_) { }

  void format(std::ostream &os, FormatState &state) const override;
  Hash hash() const override;

  template <typename T, T (HeapPointerBase::*memberfn)(T x)>
  T recurse(T arg) {
    arg = Tuple::recurse<T, memberfn>(arg);
    arg = (next.*memberfn)(arg);
    return arg;
  }

  static bool debug;
  std::vector<Location> stack_trace() const;
  virtual const ScopeStack *stack() const = 0;
  virtual ScopeStack *stack() = 0;
  void set_expr(Expr *expr);

  static size_t reserve(size_t size);
  static Scope *claim(Heap &h, size_t size, Scope *next, Scope *parent, Expr *expr); // requires prior h.reserve
  static Scope *alloc(Heap &h, size_t size, Scope *next, Scope *parent, Expr *expr);
};

#endif
