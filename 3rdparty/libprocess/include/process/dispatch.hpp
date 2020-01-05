// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License

#ifndef __PROCESS_DISPATCH_HPP__
#define __PROCESS_DISPATCH_HPP__

#include <functional>
#include <memory>
#include <string>

#include <process/process.hpp>

#include <stout/lambda.hpp>
#include <stout/preprocessor.hpp>
#include <stout/result_of.hpp>

namespace process {

// The dispatch mechanism enables you to "schedule" a method to get
// invoked on a process. The result of that method invocation is
// accessible via the future that is returned by the dispatch method
// (note, however, that it might not be the _same_ future as the one
// returned from the method, if the method even returns a future, see
// below). Assuming some class 'Fibonacci' has a (visible) method
// named 'compute' that takes an integer, N (and returns the Nth
// fibonacci number) you might use dispatch like so:
//
// PID<Fibonacci> pid = spawn(new Fibonacci(), true); // Use the GC.
// Future<int> f = dispatch(pid, &Fibonacci::compute, 10);
//
// Because the pid argument is "typed" we can ensure that methods are
// only invoked on processes that are actually of that type. Providing
// this mechanism for varying numbers of function types and arguments
// requires support for variadic templates, slated to be released in
// C++11. Until then, we use the Boost preprocessor macros to
// accomplish the same thing (albeit less cleanly). See below for
// those definitions.
//
// Dispatching is done via a level of indirection. The dispatch
// routine itself creates a promise that is passed as an argument to a
// partially applied 'dispatcher' function (defined below). The
// dispatcher routines get passed to the actual process via an
// internal routine called, not surprisingly, 'dispatch', defined
// below:

namespace internal {

// The internal dispatch routine schedules a function to get invoked
// within the context of the process associated with the specified pid
// (first argument), unless that process is no longer valid. Note that
// this routine does not expect anything in particular about the
// specified function (second argument). The semantics are simple: the
// function gets applied/invoked with the process as its first
// argument.
void dispatch(
    const UPID& pid,
    std::unique_ptr<lambda::CallableOnce<void(ProcessBase*)>> f,
    const Option<const std::type_info*>& functionType = None());


// NOTE: This struct is used by the public `dispatch(const UPID& pid, F&& f)`
// function. See comments there for the reason why we need this.
template <typename R>
struct Dispatch;


// Partial specialization for callable objects returning `void` to be dispatched
// on a process.
// NOTE: This struct is used by the public `dispatch(const UPID& pid, F&& f)`
// function. See comments there for the reason why we need this.
template <>
struct Dispatch<void>
{
  template <typename F>
  void operator()(const UPID& pid, F&& f)
  {
    std::unique_ptr<lambda::CallableOnce<void(ProcessBase*)>> f_(
        new lambda::CallableOnce<void(ProcessBase*)>(
            lambda::partial(
                [](typename std::decay<F>::type&& f, ProcessBase*) {
                  std::move(f)();
                },
                std::forward<F>(f),
                lambda::_1)));

    internal::dispatch(pid, std::move(f_));
  }
};


// Partial specialization for callable objects returning `Future<R>` to be
// dispatched on a process.
// NOTE: This struct is used by the public `dispatch(const UPID& pid, F&& f)`
// function. See comments there for the reason why we need this.
template <typename R>
struct Dispatch<Future<R>>
{
  template <typename F>
  Future<R> operator()(const UPID& pid, F&& f)
  {
    std::unique_ptr<Promise<R>> promise(new Promise<R>());
    Future<R> future = promise->future();

    std::unique_ptr<lambda::CallableOnce<void(ProcessBase*)>> f_(
        new lambda::CallableOnce<void(ProcessBase*)>(
            lambda::partial(
                [](std::unique_ptr<Promise<R>> promise,
                   typename std::decay<F>::type&& f,
                   ProcessBase*) {
                  promise->associate(std::move(f)());
                },
                std::move(promise),
                std::forward<F>(f),
                lambda::_1)));

    internal::dispatch(pid, std::move(f_));

    return future;
  }
};


// Dispatches a callable object returning `R` on a process.
// NOTE: This struct is used by the public `dispatch(const UPID& pid, F&& f)`
// function. See comments there for the reason why we need this.
template <typename R>
struct Dispatch
{
  template <typename F>
  Future<R> operator()(const UPID& pid, F&& f)
  {
    std::unique_ptr<Promise<R>> promise(new Promise<R>());
    Future<R> future = promise->future();

    std::unique_ptr<lambda::CallableOnce<void(ProcessBase*)>> f_(
        new lambda::CallableOnce<void(ProcessBase*)>(
            lambda::partial(
                [](std::unique_ptr<Promise<R>> promise,
                   typename std::decay<F>::type&& f,
                   ProcessBase*) {
                  promise->set(std::move(f)());
                },
                std::move(promise),
                std::forward<F>(f),
                lambda::_1)));

    internal::dispatch(pid, std::move(f_));

    return future;
  }
};

} // namespace internal {


// Okay, now for the definition of the dispatch routines
// themselves. For each routine we provide the version in C++11 using
// variadic templates so the reader can see what the Boost
// preprocessor macros are effectively providing. Using C++11 closures
// would shorten these definitions even more.
//
// First, definitions of dispatch for methods returning void:

template <typename T>
void dispatch(const PID<T>& pid, void (T::*method)())
{
  std::unique_ptr<lambda::CallableOnce<void(ProcessBase*)>> f(
      new lambda::CallableOnce<void(ProcessBase*)>(
          [=](ProcessBase* process) {
            assert(process != nullptr);
            T* t = dynamic_cast<T*>(process);
            assert(t != nullptr);
            (t->*method)();
          }));

  internal::dispatch(pid, std::move(f), &typeid(method));
}

template <typename T>
void dispatch(const Process<T>& process, void (T::*method)())
{
  dispatch(process.self(), method);
}

template <typename T>
void dispatch(const Process<T>* process, void (T::*method)())
{
  dispatch(process->self(), method);
}


template <typename T, typename...MethodArgs, typename ...Args>                                                 
void dispatch(
    const PID<T>& pid,
    void (T::*method)(MethodArgs...),
    Args&&... args)
{                                                                     
  auto call = [method](ProcessBase* process, MethodArgs&&... args) {      
    assert(process != nullptr);                           
    T* t = dynamic_cast<T*>(process);                     
    assert(t != nullptr);                                 
    (t->*method)(std::forward<MethodArgs>(args)...);
  };

  std::unique_ptr<lambda::CallableOnce<void(ProcessBase*)>> f(        
      new lambda::CallableOnce<void(ProcessBase*)>(                   
          lambda::partial(std::move(call), lambda::_1, std::forward<Args>(args)...)));
  internal::dispatch(pid, std::move(f), &typeid(method));
}
                                                                        
/*
template <typename T, typename...MethodArgs, typename ...Args>                                                 
void dispatch(                                           
    const Process<T>& process,                           
    void (T::*method)(MethodArgs...),                
    Args&&.. args)                       
{                                                        
  dispatch(process.self(), method, std::forward<Args>(args)...);
}


template <typename T, typename...MethodArgs, typename ...Args>                                                 
void dispatch(                                           
    const Process<T>* process,                           
    void (T::*method)(MethodArgs...),                
    Args&&.. args)                       
{                                                        
  dispatch(process->self(), method, std::forward<Args>(args)...);
}
*/

// Next, definitions of methods returning a future:

template <typename R, typename T>
Future<R> dispatch(const PID<T>& pid, Future<R> (T::*method)())
{
  std::unique_ptr<Promise<R>> promise(new Promise<R>());
  Future<R> future = promise->future();

  std::unique_ptr<lambda::CallableOnce<void(ProcessBase*)>> f(
      new lambda::CallableOnce<void(ProcessBase*)>(
          lambda::partial(
              [=](std::unique_ptr<Promise<R>> promise, ProcessBase* process) {
                assert(process != nullptr);
                T* t = dynamic_cast<T*>(process);
                assert(t != nullptr);
                promise->associate((t->*method)());
              },
              std::move(promise),
              lambda::_1)));

  internal::dispatch(pid, std::move(f), &typeid(method));

  return future;
}

template <typename R, typename T>
Future<R> dispatch(const Process<T>& process, Future<R> (T::*method)())
{
  return dispatch(process.self(), method);
}

template <typename R, typename T>
Future<R> dispatch(const Process<T>* process, Future<R> (T::*method)())
{
  return dispatch(process->self(), method);
}


template <typename T, typename R, typename...MethodArgs, typename ...Args>                                                 
Future<R> dispatch(
    const PID<T>& pid,
    Future<R> (T::*method)(MethodArgs...),
    Args&&... args)
{ 
  Promise<R> promise;
  Future<R> future = promise.future();
                                                                    
  auto call = [method](ProcessBase* process, Promise<R>&& promise_, MethodArgs&&... args) {      
    assert(process != nullptr);                           
    T* t = dynamic_cast<T*>(process);                     
    assert(t != nullptr);                                 
    promise_.associate((t->*method)(std::forward<MethodArgs>(args)...));
  };

  std::unique_ptr<lambda::CallableOnce<void(ProcessBase*)>> f(        
      new lambda::CallableOnce<void(ProcessBase*)>(                   
          lambda::partial(std::move(call), lambda::_1, std::move(promise), std::forward<Args>(args)...)));

  internal::dispatch(pid, std::move(f), &typeid(method));
  return future;
}

/*
template <typename T, typename R, typename...MethodArgs, typename ...Args>                                                 
Future<R> dispatch(
    const Process<T>& process,
    Future<R> (T::*method)(MethodArgs...),
    Args&&... args)
{
  return dispatch(process.self(), method, std::forward<Args>(args)...);
}


template <typename T, typename R, typename...MethodArgs, typename ...Args>                                                 
Future<R> dispatch(
    const Process<T>* process,
    Future<R> (T::*method)(MethodArgs...),
    Args&&... args)
{
  return dispatch(process->self(), method, std::forward<Args>(args)...);
}
*/

// Next, definitions of methods returning a value.

template <typename R, typename T>
Future<R> dispatch(const PID<T>& pid, R (T::*method)())
{
  std::unique_ptr<Promise<R>> promise(new Promise<R>());
  Future<R> future = promise->future();

  std::unique_ptr<lambda::CallableOnce<void(ProcessBase*)>> f(
      new lambda::CallableOnce<void(ProcessBase*)>(
          lambda::partial(
              [=](std::unique_ptr<Promise<R>> promise, ProcessBase* process) {
                assert(process != nullptr);
                T* t = dynamic_cast<T*>(process);
                assert(t != nullptr);
                promise->set((t->*method)());
              },
              std::move(promise),
              lambda::_1)));

  internal::dispatch(pid, std::move(f), &typeid(method));

  return future;
}

template <typename R, typename T>
Future<R> dispatch(const Process<T>& process, R (T::*method)())
{
  return dispatch(process.self(), method);
}

template <typename R, typename T>
Future<R> dispatch(const Process<T>* process, R (T::*method)())
{
  return dispatch(process->self(), method);
}


// TODO: move to proper locations
namespace cpp17 {

template<class...Ts> struct make_void {using type = void;};
template<class...Ts> using void_t = typename make_void<Ts...>::type;

}


template<
  class F, 
  class X = typename std::decay<decltype(std::declval<F>().get())>::type >
struct FutureReturn{ using Type = X;};


template<class F, class = cpp17::void_t<> > 
struct IsFuture : std::false_type {};


template<class F>
struct IsFuture<F, cpp17::void_t<typename FutureReturn<F>::Type> >
  : std::is_same<F, Future<typename FutureReturn<F>::Type>> {};


// This overload is disabled for methods returning Future
template <typename T, typename R, typename...MethodArgs, typename ...Args>                                                 
typename std::enable_if<!IsFuture<R>::value, Future<R>>::type dispatch(
    const PID<T>& pid,
    R (T::*method)(MethodArgs...),
    Args&&... args)
{ 
  Promise<R> promise;
  Future<R> future = promise.future();
                                                                    
  auto call = [method](ProcessBase* process, Promise<R>&& promise_, MethodArgs&&... args) {      
    assert(process != nullptr);                           
    T* t = dynamic_cast<T*>(process);                     
    assert(t != nullptr);                                 
    promise_.set((t->*method)(std::forward<MethodArgs>(args)...));
  };

  std::unique_ptr<lambda::CallableOnce<void(ProcessBase*)>> f(        
      new lambda::CallableOnce<void(ProcessBase*)>(                   
          lambda::partial(std::move(call), lambda::_1, std::move(promise), std::forward<Args>(args)...)));

  internal::dispatch(pid, std::move(f), &typeid(method));
  return future;
}


// Convenience wrappers for method dispatches
template <typename T, typename R, typename...MethodArgs, typename ...Args>                                               
auto dispatch(
    const Process<T>& process,
    R (T::*method)(MethodArgs...),
    Args&&... args)
  -> decltype(dispatch(process.self(), method, std::forward<Args>(args)...))
{
  return dispatch(process.self(), method, std::forward<Args>(args)...);
}


template <typename T, typename R, typename...MethodArgs, typename ...Args>                                               
auto dispatch(
    const Process<T>* process,
    R (T::*method)(MethodArgs...),
    Args&&... args)
  -> decltype(dispatch(process->self(), method, std::forward<Args>(args)...))
{
  return dispatch(process->self(), method, std::forward<Args>(args)...);
}


// We use partial specialization of
//   - internal::Dispatch<void> vs
//   - internal::Dispatch<Future<R>> vs
//   - internal::Dispatch
// in order to determine whether R is void, Future or other types.
template <typename F, typename R = typename result_of<F()>::type>
auto dispatch(const UPID& pid, F&& f)
  -> decltype(internal::Dispatch<R>()(pid, std::forward<F>(f)))
{
  return internal::Dispatch<R>()(pid, std::forward<F>(f));
}

} // namespace process {

#endif // __PROCESS_DISPATCH_HPP__
