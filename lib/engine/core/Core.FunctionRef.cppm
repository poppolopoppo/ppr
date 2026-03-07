/*
BSD 2-Clause License

Copyright (c) 2022, Zhihao Yuan
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation
and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
module;

#include "pP/Macros.h"

export module engine.core:function_ref;

import std;

// ------------------------------------------------------------------
// function_ref (waiting for C++26)
// ------------------------------------------------------------------

#if 0
namespace std23 {
    /// See also: https://www.agner.org/optimize/calling_conventions.pdf
    namespace details {
        template<class T>
        inline constexpr auto param_by_value_or_reference = [] {
            if constexpr (std::is_trivially_copyable_v<T>)
                return std::type_identity<T>();
            else
                return std::add_rvalue_reference<T>();
        };
    }

    template<class T>
    using param_ref_t = std::invoke_result_t<decltype(
        details::param_by_value_or_reference<T>)>::type;

    /// callable object abstraction
    template<typename T, class OperatorT = decltype(&T::operator ())>
    struct callable_object;

    namespace details {
        template<typename ReturnT, bool NoExceptV, bool ConstV, typename... ArgsT>
        struct callable_object_traits {
            using noexcept_t = std::bool_constant<NoExceptV>;
            using return_t = ReturnT;
            using signature_t = ReturnT(ArgsT...);

            static constexpr std::size_t arity_v = sizeof...(ArgsT);

            template<class T>
            using add_const_t = std::conditional_t<ConstV, std::add_const_t<T>, T>;

            template<class... T>
            static constexpr bool is_invocable_v = std::conditional_t<
                NoExceptV,
                std::is_nothrow_invocable_r<ReturnT, T..., ArgsT...>,
                std::is_invocable_r<ReturnT, T..., ArgsT...> >::value;
        };
    }

    // ------------------------------------------------------------------
    // free function
    // ------------------------------------------------------------------

    template<typename ReturnT, typename... ArgsT>
    struct callable_object<ReturnT(ArgsT...), void> :
            details::callable_object_traits<ReturnT, false, false, ArgsT...> {
        using free_function_t = ReturnT (*)(ArgsT...);

        free_function_t m_free_function;

        explicit constexpr callable_object(const free_function_t free_function) noexcept
            : m_free_function{free_function} {
        }

        constexpr ReturnT operator ()(ArgsT... args) const {
            return (*m_free_function)(std::forward<ArgsT>(args)...);
        }
    };

    template<typename ReturnT, typename... ArgsT>
    callable_object(ReturnT (*free_function)(ArgsT...)) -> callable_object<ReturnT(ArgsT...), void>;

    // ------------------------------------------------------------------
    // noexcept free function
    // ------------------------------------------------------------------

    template<typename ReturnT, typename... ArgsT>
    struct callable_object<ReturnT(ArgsT...) noexcept, void> :
            details::callable_object_traits<ReturnT, true, false, ArgsT...> {
        using free_function_t = ReturnT (*)(ArgsT...) noexcept;

        free_function_t m_free_function;

        explicit constexpr callable_object(const free_function_t free_function) noexcept
            : m_free_function{free_function} {
        }

        constexpr ReturnT operator ()(ArgsT... args) const noexcept {
            return (m_free_function)(std::forward<ArgsT>(args)...);
        }
    };

    template<typename ReturnT, typename... ArgsT>
    callable_object(ReturnT (*)(ArgsT...) noexcept) -> callable_object<ReturnT(ArgsT...) noexcept, void>;

    // ------------------------------------------------------------------
    // non-const member function
    // ------------------------------------------------------------------

    template<typename ReturnT, class ClassT, typename... ArgsT>
    struct callable_object<ReturnT(ClassT::*)(ArgsT...), void> :
            details::callable_object_traits<ReturnT, false, false, ArgsT...> {
        using free_function_t = ReturnT (*)(ClassT *, ArgsT...);

        ReturnT (ClassT::*m_member_function)(ArgsT...){};

        explicit constexpr callable_object(ReturnT (ClassT::*member_function)(ArgsT...)) noexcept
            : m_member_function(member_function) {
        }

        constexpr ReturnT operator ()(ClassT *p_obj, ArgsT... args) const {
            return (p_obj->*m_member_function)(std::forward<ArgsT>(args)...);
        }
    };

    template<typename ReturnT, class ClassT, typename... ArgsT>
    callable_object(ReturnT (ClassT::*)(ArgsT...)) -> callable_object<ReturnT(ClassT::*)(ArgsT...), void>;

    // ------------------------------------------------------------------
    // noexcept non-const member function
    // ------------------------------------------------------------------

    template<typename ReturnT, class ClassT, typename... ArgsT>
    struct callable_object<ReturnT(ClassT::*)(ArgsT...) noexcept, void> :
            details::callable_object_traits<ReturnT, true, false, ArgsT...> {
        using free_function_t = ReturnT (*)(ClassT *, ArgsT...) noexcept;

        ReturnT (ClassT::*m_member_function)(ArgsT...) noexcept{};

        explicit constexpr callable_object(ReturnT (ClassT::*member_function)(ArgsT...) noexcept) noexcept
            : m_member_function(member_function) {
        }

        constexpr ReturnT operator ()(ClassT *p_obj, ArgsT... args) const noexcept {
            return (p_obj->*m_member_function)(std::forward<ArgsT>(args)...);
        }
    };

    template<typename ReturnT, class ClassT, typename... ArgsT>
    callable_object(ReturnT (ClassT::*)(ArgsT...) noexcept) -> callable_object<ReturnT(ClassT::*)(ArgsT...) noexcept, void>;

    // ------------------------------------------------------------------
    // const member function
    // ------------------------------------------------------------------

    template<typename ReturnT, class ClassT, typename... ArgsT>
    struct callable_object<ReturnT(ClassT::*)(ArgsT...) const, void> :
            details::callable_object_traits<ReturnT, false, true, ArgsT...> {
        using free_function_t = ReturnT (*)(const ClassT *, ArgsT...);

        ReturnT (ClassT::*m_member_function)(ArgsT...) const{};

        explicit constexpr callable_object(ReturnT (ClassT::*member_function)(ArgsT...) const) noexcept
            : m_member_function(member_function) {
        }

        constexpr ReturnT operator ()(const ClassT *p_obj, ArgsT... args) const {
            return (p_obj->*m_member_function)(std::forward<ArgsT>(args)...);
        }
    };

    template<typename ReturnT, class ClassT, typename... ArgsT>
    callable_object(ReturnT (ClassT::*)(ArgsT...) const) -> callable_object<ReturnT(ClassT::*)(ArgsT...) const, void>;

    // ------------------------------------------------------------------
    // noexcept const member function
    // ------------------------------------------------------------------

    template<typename ReturnT, class ClassT, typename... ArgsT>
    struct callable_object<ReturnT(ClassT::*)(ArgsT...) const noexcept, void> :
            details::callable_object_traits<ReturnT, true, true, ArgsT...> {
        using free_function_t = ReturnT (*)(const ClassT *, ArgsT...) noexcept;

        ReturnT (ClassT::*m_member_function)(ArgsT...) const noexcept{};

        explicit constexpr callable_object(ReturnT (ClassT::*member_function)(ArgsT...) const noexcept) noexcept
            : m_member_function(member_function) {
        }

        constexpr ReturnT operator ()(const ClassT *p_obj, ArgsT... args) const noexcept {
            return (p_obj->*m_member_function)(std::forward<ArgsT>(args)...);
        }
    };

    template<typename ReturnT, class ClassT, typename... ArgsT>
    callable_object(ReturnT (ClassT::*)(ArgsT...) const noexcept) -> callable_object<ReturnT(ClassT::*)(ArgsT...) const noexcept, void>;

    // ------------------------------------------------------------------
    // object overload operator ()
    // ------------------------------------------------------------------

    template<typename T>
    struct callable_object<T, decltype(&T::operator ())> : callable_object<decltype(&T::operator ()), void> {
        using super_t = callable_object<decltype(&T::operator ()), void>;

        explicit constexpr callable_object(const T &obj) noexcept
            : super_t(&T::operator ()) {
        }
    };

    template<typename T>
    callable_object(const T &) -> callable_object<T>;

    // ------------------------------------------------------------------
    // c++20 concept to check is a type is callable
    // ------------------------------------------------------------------

    template<typename T>
    concept TCallable = requires(T &callback)
    {
        { callable_object(callback) };
    };

    template<TCallable FunctionT>
    using callable_signature_t = decltype(callable_object{std::declval<FunctionT>()})::signature_t;

    // ------------------------------------------------------------------
    // static_function avoid storing a function pointer if known at compile-time
    // ------------------------------------------------------------------

    template<auto FuncV> requires TCallable<decltype(FuncV)>
    struct [[nodiscard]] static_function_t {
        using callable_object_t = decltype(callable_object{FuncV});

        using noexcept_t = callable_object_t::noexcept_t;
        using return_t = callable_object_t::return_t;
        using signature_t = callable_object_t::signature_t;
        using free_function_t = callable_object_t::free_function_t;

        static constexpr std::size_t arity_v = callable_object_t::arity_v;

        template<class... T>
        static constexpr bool is_invocable_v = callable_object_t::template is_invocable_v<T...>;

        template<typename... ArgsT>
            requires std::is_invocable_r_v<return_t, callable_object_t, ArgsT &&...>
        constexpr return_t operator()(ArgsT &&... args) const noexcept(noexcept_t::value) {
            return callable_object_t{FuncV}(std::forward<ArgsT>(args)...);
        }
    };

    template<auto FuncV> requires TCallable<decltype(FuncV)>
    constexpr static_function_t<FuncV> static_function{};


    // ------------------------------------------------------------------
    // safe polymorphic storage to avoid undefined behavior
    // ------------------------------------------------------------------

    /// avoid undefined behavior, maybe?
    namespace details {
        struct function_ptr {
            union storage_t {
                void *m_p{nullptr};
                const void *m_cp;

                void (*m_fp)();

                constexpr storage_t() noexcept = default;

                explicit constexpr storage_t(void *p) noexcept
                    : m_p(p) {
                }

                explicit constexpr storage_t(const void *cp) noexcept
                    : m_cp(cp) {
                }

                template<typename T> requires std::is_object_v<T>
                explicit constexpr storage_t(T *p) noexcept
                    : m_p(p) {
                }

                template<typename T> requires std::is_object_v<T>
                explicit constexpr storage_t(const T *cp) noexcept
                    : m_cp(cp) {
                }

                template<typename T> requires std::is_function_v<T>
                explicit storage_t(T *p) noexcept
                    : m_fp(reinterpret_cast<decltype(m_fp)>(p)) {
                }

                [[nodiscard]] friend constexpr bool operator ==(const storage_t lhs, const storage_t rhs) noexcept {
                    return lhs.m_p == rhs.m_p;
                }
            };

            template<typename T>
            [[nodiscard]] constexpr static auto get(storage_t obj) noexcept {
                if constexpr (std::is_const_v<T>) {
                    return static_cast<T *>(obj.m_cp);
                } else if constexpr (std::is_object_v<T> || std::is_void_v<T>) {
                    return static_cast<T *>(obj.m_p);
                } else {
                    return reinterpret_cast<T *>(obj.m_fp);
                }
            }
        };
    }

    // ------------------------------------------------------------------
    // function_ref: polymorphic callable object thanks to type erasure
    // ------------------------------------------------------------------

    template<TCallable FunctionT, typename SignatureT = callable_signature_t<FunctionT> >
    class function_ref;

    template<TCallable FunctionT, typename ReturnT, typename... ArgsT>
    class function_ref<FunctionT, ReturnT(ArgsT...)> : details::function_ptr {
        using callable_t = callable_object<FunctionT, void>;
        using dispatch_t = ReturnT (*)(storage_t, ArgsT...);

        template<TCallable, typename>
        friend class function_ref;

        template<typename T>
        using add_const_t = callable_t::template add_const_t<T>;

        dispatch_t m_dispatch{};
        storage_t m_storage{};

    public:
        static constexpr bool is_noexcept_v = callable_t::noexcept_t::value;

        template<typename... T>
        static constexpr bool is_invocable_v = callable_t::template is_invocable_v<T...>;

        constexpr function_ref() noexcept = default;

        /// allow promotion if the functions have the same signature
        template<TCallable OtherFunctionT>
        constexpr function_ref(const function_ref<OtherFunctionT, ReturnT(ArgsT...)> &other) noexcept
            : m_dispatch(other.m_dispatch), m_storage(other.m_storage) {
        }

        [[nodiscard]] constexpr bool isValid() const noexcept {
            return m_dispatch != nullptr;
        }

        constexpr ReturnT operator()(ArgsT... args) const noexcept(is_noexcept_v) {
            PPR_ASSERT(isValid());
            if constexpr (std::is_void_v<ReturnT>) {
                m_dispatch(m_storage, std::forward<ArgsT>(args)...);
            } else {
                return m_dispatch(m_storage, std::forward<ArgsT>(args)...);
            }
        }

        constexpr void reset() noexcept {
            m_dispatch = nullptr;
            m_storage = {};
        }

        [[nodiscard]] friend constexpr bool operator==(function_ref a, function_ref b) noexcept {
            return a.m_dispatch == b.m_dispatch && a.m_storage == b.m_storage;
        }

        template<typename F>
            requires std::is_function_v<F> && is_invocable_v<const F *>
        constexpr function_ref(const F *func_ptr) noexcept
            : m_dispatch(
                  [](storage_t storage, param_ref_t<ArgsT>... args) constexpr noexcept(is_noexcept_v) -> ReturnT {
                      if constexpr (std::is_void_v<ReturnT>) {
                          std::invoke(*function_ptr::get<F>(storage), args...);
                      } else {
                          return std::invoke(*function_ptr::get<F>(storage), args...);
                      }
                  }),
              m_storage(func_ptr) {
        }

        template<auto F>
            requires is_invocable_v<decltype(F)>
        constexpr function_ref(static_function_t<F>) noexcept
            : m_dispatch(
                [](storage_t, ArgsT... args) constexpr noexcept(is_noexcept_v) -> ReturnT {
                    if constexpr (std::is_void_v<ReturnT>) {
                        static_function<F>(args...);
                    } else {
                        return static_function<F>(args...);
                    }
                }) {
        }

        template<auto F, typename FirstArgT>
            requires is_invocable_v<decltype(F), FirstArgT &>
        constexpr function_ref(static_function_t<F>, FirstArgT &first_arg PPR_LIFETIME_BOUND) noexcept
            : m_dispatch(
                  [](const storage_t storage, param_ref_t<ArgsT>... args) constexpr noexcept(is_noexcept_v) -> ReturnT {
                      if constexpr (std::is_void_v<ReturnT>) {
                          static_function<F>(function_ptr::get<FirstArgT>(storage), args...);
                      } else {
                          return static_function<F>(function_ptr::get<FirstArgT>(storage), args...);
                      }
                  }),
              m_storage(std::addressof(first_arg)) {
        }

        template<auto F, typename FirstArgT>
            requires is_invocable_v<decltype(F), FirstArgT *>
        constexpr function_ref(static_function_t<F>, FirstArgT *p_first_arg PPR_LIFETIME_BOUND) noexcept
            : m_dispatch(
                  [](const storage_t storage, param_ref_t<ArgsT>... args) constexpr noexcept(is_noexcept_v) -> ReturnT {
                      if constexpr (std::is_void_v<ReturnT>) {
                          static_function<F>(function_ptr::get<FirstArgT>(storage), args...);
                      } else {
                          return static_function<F>(function_ptr::get<FirstArgT>(storage), args...);
                      }
                  }),
              m_storage(p_first_arg) {
        }

        template<auto F, typename ClassT>
            requires is_invocable_v<decltype(F), add_const_t<ClassT> *>
        constexpr function_ref(static_function_t<F>, add_const_t<ClassT> *p_first_arg PPR_LIFETIME_BOUND) noexcept
            : m_dispatch(
                  [](const storage_t storage, param_ref_t<ArgsT>... args) constexpr noexcept(is_noexcept_v) -> ReturnT {
                      if constexpr (std::is_void_v<ReturnT>) {
                          static_function<F>(function_ptr::get<add_const_t<ClassT> >(storage), args...);
                      } else {
                          return static_function<F>(function_ptr::get<add_const_t<ClassT> >(storage), args...);
                      }
                  }),
              m_storage(p_first_arg) {
        }

        template<typename FunctorT>
            requires is_invocable_v<FunctorT &&>
        constexpr function_ref(FunctorT &&functor PPR_LIFETIME_BOUND) noexcept
            : m_dispatch(
                  [](storage_t storage, param_ref_t<ArgsT> ... args) constexpr noexcept(is_noexcept_v) -> ReturnT {
                      if constexpr (std::is_void_v<ReturnT>) {
                          std::invoke(*function_ptr::get<std::decay_t<FunctorT> >(storage), args...);
                      } else {
                          return std::invoke(*function_ptr::get<std::decay_t<FunctorT> >(storage), args...);
                      }
                  }),
              m_storage(std::addressof(functor)) {
        }
    };

    template<typename F>
    function_ref(F &&) -> function_ref<callable_signature_t<F> >;
}
#else
// https://github.com/zhihaoy/nontype_functional/blob/main/include/std23/__functional_base.h
namespace std23 {
    template<auto V>
    struct nontype_t // freestanding
    {
        explicit nontype_t() = default;
    };

    template<auto V>
    inline constexpr nontype_t<V> nontype{}; // freestanding

    using std::in_place_type;
    using std::in_place_type_t;
    using std::initializer_list;
    using std::nullptr_t;

    template<class R, class F, class... Args>
        requires std::is_invocable_r_v<R, F, Args...>
    constexpr R invoke_r(F &&f, Args &&... args) // freestanding
        noexcept(std::is_nothrow_invocable_r_v<R, F, Args...>) {
        if constexpr (std::is_void_v<R>)
            std::invoke(std::forward<F>(f), std::forward<Args>(args)...);
        else
            return std::invoke(std::forward<F>(f), std::forward<Args>(args)...);
    }

    // See also: https://www.agner.org/optimize/calling_conventions.pdf
    template<class T>
    inline constexpr auto _select_param_type = [] {
        if constexpr (std::is_trivially_copyable_v<T>)
            return std::type_identity<T>();
        else
            return std::add_rvalue_reference<T>();
    };

    template<class T>
    using _param_t = std::invoke_result_t<decltype(_select_param_type<T>)>::type;

    template<class T, class Self>
    inline constexpr bool _is_not_self =
            not std::is_same_v<std::remove_cvref_t<T>, Self>;

    template<class T, template<class...> class>
    inline constexpr bool _looks_nullable_to_impl = std::is_member_pointer_v<T>;

    template<class F, template<class...> class Self>
    inline constexpr bool _looks_nullable_to_impl<F *, Self> =
            std::is_function_v<F>;

    template<class... S, template<class...> class Self>
    inline constexpr bool _looks_nullable_to_impl<Self<S...>, Self> = true;

    template<class S, template<class...> class Self>
    inline constexpr bool _looks_nullable_to =
            _looks_nullable_to_impl<std::remove_cvref_t<S>, Self>;

    template<class T>
    inline constexpr bool _is_not_nontype_t = true;
    template<auto f>
    inline constexpr bool _is_not_nontype_t<nontype_t<f> > = false;

    template<class T>
    struct _adapt_signature;

    template<class F> requires std::is_function_v<F>
    struct _adapt_signature<F *> {
        using type = F;
    };

    template<class Fp>
    using _adapt_signature_t = _adapt_signature<Fp>::type;

    template<class S>
    struct _not_qualifying_this {
    };

    template<class R, class... Args>
    struct _not_qualifying_this<R(Args...)> {
        using type = R(Args...);
    };

    template<class R, class... Args>
    struct _not_qualifying_this<R(Args...) noexcept> {
        using type = R(Args...) noexcept;
    };

    template<class R, class... Args>
    struct _not_qualifying_this<R(Args...) const> : _not_qualifying_this<R(Args...)> {
    };

    template<class R, class... Args>
    struct _not_qualifying_this<R(Args...) volatile>
            : _not_qualifying_this<R(Args...)> {
    };

    template<class R, class... Args>
    struct _not_qualifying_this<R(Args...) const volatile>
            : _not_qualifying_this<R(Args...)> {
    };

    template<class R, class... Args>
    struct _not_qualifying_this<R(Args...) &> : _not_qualifying_this<R(Args...)> {
    };

    template<class R, class... Args>
    struct _not_qualifying_this<R(Args...) const &>
            : _not_qualifying_this<R(Args...)> {
    };

    template<class R, class... Args>
    struct _not_qualifying_this<R(Args...) volatile &>
            : _not_qualifying_this<R(Args...)> {
    };

    template<class R, class... Args>
    struct _not_qualifying_this<R(Args...) const volatile &>
            : _not_qualifying_this<R(Args...)> {
    };

    template<class R, class... Args>
    struct _not_qualifying_this<R(Args...) &&> : _not_qualifying_this<R(Args...)> {
    };

    template<class R, class... Args>
    struct _not_qualifying_this<R(Args...) const &&>
            : _not_qualifying_this<R(Args...)> {
    };

    template<class R, class... Args>
    struct _not_qualifying_this<R(Args...) volatile &&>
            : _not_qualifying_this<R(Args...)> {
    };

    template<class R, class... Args>
    struct _not_qualifying_this<R(Args...) const volatile &&>
            : _not_qualifying_this<R(Args...)> {
    };

    template<class R, class... Args>
    struct _not_qualifying_this<R(Args...) const noexcept>
            : _not_qualifying_this<R(Args...) noexcept> {
    };

    template<class R, class... Args>
    struct _not_qualifying_this<R(Args...) volatile noexcept>
            : _not_qualifying_this<R(Args...) noexcept> {
    };

    template<class R, class... Args>
    struct _not_qualifying_this<R(Args...) const volatile noexcept>
            : _not_qualifying_this<R(Args...) noexcept> {
    };

    template<class R, class... Args>
    struct _not_qualifying_this<R(Args...) & noexcept>
            : _not_qualifying_this<R(Args...) noexcept> {
    };

    template<class R, class... Args>
    struct _not_qualifying_this<R(Args...) const & noexcept>
            : _not_qualifying_this<R(Args...) noexcept> {
    };

    template<class R, class... Args>
    struct _not_qualifying_this<R(Args...) volatile & noexcept>
            : _not_qualifying_this<R(Args...) noexcept> {
    };

    template<class R, class... Args>
    struct _not_qualifying_this<R(Args...) const volatile & noexcept>
            : _not_qualifying_this<R(Args...) noexcept> {
    };

    template<class R, class... Args>
    struct _not_qualifying_this<R(Args...) && noexcept>
            : _not_qualifying_this<R(Args...) noexcept> {
    };

    template<class R, class... Args>
    struct _not_qualifying_this<R(Args...) const && noexcept>
            : _not_qualifying_this<R(Args...) noexcept> {
    };

    template<class R, class... Args>
    struct _not_qualifying_this<R(Args...) volatile && noexcept>
            : _not_qualifying_this<R(Args...) noexcept> {
    };

    template<class R, class... Args>
    struct _not_qualifying_this<R(Args...) const volatile && noexcept>
            : _not_qualifying_this<R(Args...) noexcept> {
    };

    template<class F, class T>
    struct _drop_first_arg_to_invoke;

    template<class T, class R, class G, class... Args>
    struct _drop_first_arg_to_invoke<R (*)(G, Args...), T> {
        using type = R(Args...);
    };

    template<class T, class R, class G, class... Args>
    struct _drop_first_arg_to_invoke<R (*)(G, Args...) noexcept, T> {
        using type = R(Args...) noexcept;
    };

    template<class T, class M, class G> requires std::is_object_v<M>
    struct _drop_first_arg_to_invoke<M G::*, T> {
        using type = std::invoke_result_t<M G::*, T>() noexcept;
    };

    template<class T, class M, class G> requires std::is_function_v<M>
    struct _drop_first_arg_to_invoke<M G::*, T> : _not_qualifying_this<M> {
    };

    template<class F, class T>
    using _drop_first_arg_to_invoke_t = _drop_first_arg_to_invoke<F, T>::type;
} // namespace std23

// https://github.com/zhihaoy/nontype_functional/blob/main/include/std23/function_ref.h
export namespace std23 {
    template<class Sig>
    struct _qual_fn_sig;

    template<class R, class... Args>
    struct _qual_fn_sig<R(Args...)> {
        using function = R(Args...);
        using without_noexcept = function;
        using without_const = function;
        static constexpr bool is_noexcept = false;

        template<class... T>
        static constexpr bool is_invocable_using =
                std::is_invocable_r_v<R, T..., Args...>;

        template<class T>
        using cv = T;
    };

    template<class R, class... Args>
    struct _qual_fn_sig<R(Args...) noexcept> {
        using function = R(Args...);
        using without_noexcept = function;
        using without_const = R(Args...) noexcept;
        static constexpr bool is_noexcept = true;

        template<class... T>
        static constexpr bool is_invocable_using =
                std::is_nothrow_invocable_r_v<R, T..., Args...>;

        template<class T>
        using cv = T;
    };

    template<class R, class... Args>
    struct _qual_fn_sig<R(Args...) const> : _qual_fn_sig<R(Args...)> {
        template<class T>
        using cv = T const;
        using without_noexcept = R(Args...) const;
    };

    template<class R, class... Args>
    struct _qual_fn_sig<R(Args...) const noexcept>
            : _qual_fn_sig<R(Args...) noexcept> {
        template<class T>
        using cv = T const;
        using without_noexcept = R(Args...) const;
    };

    struct _function_ref_base {
        union storage {
            void *p_ = nullptr;
            void const *cp_;

            void (*fp_)();

            constexpr storage() noexcept = default;

            template<class T> requires std::is_object_v<T>
            constexpr explicit storage(T *p) noexcept : p_(p) {
            }

            template<class T> requires std::is_object_v<T>
            constexpr explicit storage(T const *p) noexcept : cp_(p) {
            }

            template<class T> requires std::is_function_v<T>
            constexpr explicit storage(T *p) noexcept
                : fp_(reinterpret_cast<decltype(fp_)>(p)) {
            }
        };

        template<class T>
        constexpr static auto get(storage obj) {
            if constexpr (std::is_const_v<T>)
                return static_cast<T *>(obj.cp_);
            else if constexpr (std::is_object_v<T>)
                return static_cast<T *>(obj.p_);
            else
                return reinterpret_cast<T *>(obj.fp_);
        }
    };

    template<class Sig, class = typename _qual_fn_sig<Sig>::function>
    class function_ref; // freestanding

    template<class From, class To>
    inline constexpr bool _is_ref_convertible = false;

    template<class T, class U>
    inline constexpr bool _is_ref_convertible<function_ref<T>, function_ref<U> > =
            std::is_convertible_v<typename _not_qualifying_this<T>::type &,
                typename _not_qualifying_this<U>::type &>;

    template<class From, class To>
    inline constexpr bool _is_as_immutable_as = false;

    template<class T, class U>
    inline constexpr bool _is_as_immutable_as<function_ref<T>, function_ref<U> > =
            std::is_convertible_v<typename _qual_fn_sig<U>::template cv<int> &,
                typename _qual_fn_sig<T>::template cv<int> &>;

    template<class Sig, class R, class... Args>
    class function_ref<Sig, R(Args...)> // freestanding
            : _function_ref_base {
        using signature = _qual_fn_sig<Sig>;

        template<class T>
        using cv = signature::template cv<T>;
        template<class T>
        using cvref = cv<T> &;
        static constexpr bool noex = signature::is_noexcept;

        template<class... T>
        static constexpr bool is_invocable_using =
                signature::template is_invocable_using<T...>;

        template<class F>
        static constexpr bool is_convertible_from_specialization =
                _is_ref_convertible<F, function_ref> and
                _is_as_immutable_as<F, function_ref>;

        typedef R fwd_t(storage, _param_t<Args>...) noexcept(noex);

        fwd_t *fptr_ = nullptr;
        storage obj_;

        friend class function_ref<typename signature::without_noexcept>;
        friend class function_ref<typename signature::without_const>;
        friend class function_ref<typename signature::function>;

    public:
        template<class F>
        function_ref(F *f) noexcept
            requires std::is_function_v<F> and is_invocable_using<F>
            : fptr_(
                  [](storage fn_, _param_t<Args>... args) noexcept(noex) -> R {
                      if constexpr (std::is_void_v<R>)
                          get<F>(fn_)(static_cast<decltype(args)>(args)...);
                      else
                          return get<F>(fn_)(static_cast<decltype(args)>(args)...);
                  }),
              obj_(f) {
            PPR_ASSERT(f != nullptr && "must reference a function");
        }

        template<class F, class T = std::remove_reference_t<F> >
        constexpr function_ref(F &&f) noexcept
            requires(not is_convertible_from_specialization<std::remove_cv_t<T> > and
                     not std::is_member_pointer_v<T> and
                     is_invocable_using<cvref<T> >)
            : fptr_(
                  [](storage fn_, _param_t<Args>... args) noexcept(noex) -> R {
                      cvref<T> obj = *get<T>(fn_);
                      if constexpr (std::is_void_v<R>)
                          obj(static_cast<decltype(args)>(args)...);
                      else
                          return obj(static_cast<decltype(args)>(args)...);
                  }),
              obj_(std::addressof(f)) {
        }

        template<class F>
        constexpr function_ref(F f) noexcept
            requires(_is_not_self<F, function_ref> and
                     is_convertible_from_specialization<F>)
            : fptr_(f.fptr_), obj_(f.obj_) {
        }

        template<class T>
        function_ref &operator=(T)
            requires(not is_convertible_from_specialization<T> and
                     not std::is_pointer_v<T> and _is_not_nontype_t<T>)
        = delete;

        template<auto f>
        constexpr function_ref(nontype_t<f>) noexcept
            requires is_invocable_using<decltype((f))>
            : fptr_(
                [](storage, _param_t<Args>... args) noexcept(noex) -> R {
                    return std23::invoke_r<R>(
                        f, static_cast<decltype(args)>(args)...);
                }) {
            using F = decltype(f);
            if constexpr (std::is_pointer_v<F> or std::is_member_pointer_v<F>)
                static_assert(f != nullptr, "NTTP callable must be usable");
        }

        template<auto f, class U, class T = std::remove_reference_t<U> >
        constexpr function_ref(nontype_t<f>, U &&obj) noexcept
            requires(not std::is_rvalue_reference_v<U &&> and
                     is_invocable_using<decltype((f)), cvref<T> >)
            : fptr_(
                  [](storage this_, _param_t<Args>... args) noexcept(noex) -> R {
                      cvref<T> obj = *get<T>(this_);
                      return std23::invoke_r<R>(
                          f, obj, static_cast<decltype(args)>(args)...);
                  }),
              obj_(std::addressof(obj)) {
            using F = decltype(f);
            if constexpr (std::is_pointer_v<F> or std::is_member_pointer_v<F>)
                static_assert(f != nullptr, "NTTP callable must be usable");
        }

        template<auto f, class T>
        constexpr function_ref(nontype_t<f>, cv<T> *obj) noexcept
            requires is_invocable_using<decltype((f)), decltype(obj)>
            : fptr_(
                  [](storage this_, _param_t<Args>... args) noexcept(noex) -> R {
                      return std23::invoke_r<R>(
                          f, get<cv<T> >(this_),
                          static_cast<decltype(args)>(args)...);
                  }),
              obj_(obj) {
            using F = decltype(f);
            if constexpr (std::is_pointer_v<F> or std::is_member_pointer_v<F>)
                static_assert(f != nullptr, "NTTP callable must be usable");

            if constexpr (std::is_member_pointer_v<F>)
                PPR_ASSERT(obj != nullptr && "must reference an object");
        }

        constexpr R operator()(Args... args) const noexcept(noex) {
            return fptr_(obj_, std::forward<Args>(args)...);
        }
    };

    template<class F> requires std::is_function_v<F>
    function_ref(F *) -> function_ref<F>;

    template<auto V>
    function_ref(nontype_t<V>) -> function_ref<_adapt_signature_t<decltype(V)> >;

    template<auto V, class T>
    function_ref(nontype_t<V>, T &&)
        -> function_ref<_drop_first_arg_to_invoke_t<decltype(V), T &> >;
} // namespace std23
#endif
