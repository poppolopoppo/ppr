module;
#include "pP/Macros.h"
export module engine.core:service;

import :containers.stl;
import :hal;
import :memory.arena;
import :memory.pointer;
import std;

export namespace pP {
    // ------------------------------------------------------------------
    // service base interface, can be referenced safely with safe_ptr<>
    // ------------------------------------------------------------------

    class IService : public safe_object {
    public:
        // ReSharper disable once CppHidingFunction
        virtual ~IService() = default;
    };

    // ------------------------------------------------------------------
    // services are stored inside a flat map, for fast, cache-coherent lookups
    // ------------------------------------------------------------------

    class ServiceInjector;

    class ServicesStore : public safe_object {
        mutable std::shared_mutex m_shared_mutex{};
        FlatMap<std::type_index, safe_ptr<IService> > m_services{};
        safe_ptr<ServicesStore> m_parent{};

        [[nodiscard]] bool insert_(std::type_index service_key, safe_ptr<IService> &&service);
        void insert_or_assign_(std::type_index service_key, safe_ptr<IService> &&service);
        [[nodiscard]] bool release_(std::type_index service_key, const IService &service);
        [[nodiscard]] safe_ptr<IService> tryGet_(std::type_index service_key) const noexcept;

    public:
        ServicesStore() noexcept = default;

        explicit ServicesStore(safe_ptr<ServicesStore> parent) noexcept
            : m_parent{std::move(parent)} {
        }

        void reset() noexcept;

        [[nodiscard]] ServiceInjector inject() const noexcept;

        using iterator = FlatMap<std::type_index, safe_ptr<IService> >::iterator;
        using const_iterator = FlatMap<std::type_index, safe_ptr<IService> >::iterator;

        template<typename T>
            requires std::is_base_of_v<IService, T>
        [[nodiscard]] bool insert(safe_ptr<T> service) {
            return insert_(typeid(T), std::move(service));
        }

        template<typename T>
            requires std::is_base_of_v<IService, T>
        void insert_or_assign(safe_ptr<T> service) {
            return insert_or_assign_(typeid(T), std::move(service));
        }

        template<typename T>
            requires std::is_base_of_v<IService, T>
        bool erase(const T &service) noexcept {
            return release_(typeid(T), service);
        }

        template<typename T>
            requires std::is_base_of_v<IService, T>
        [[nodiscard]] safe_ptr<T> tryGet() const noexcept {
            if (safe_ptr<IService> service = tryGet_(typeid(T))) {
                return checked_cast<T>(service);
            }
            return nullptr;
        }

        template<typename T>
            requires std::is_base_of_v<IService, T>
        [[nodiscard]] safe_ptr<T> get() const noexcept {
            const safe_ptr<T> ptr = tryGet<T>();
            PPR_ASSERT(ptr.isValid() && "get() called on unknown service");
            return ptr;
        }
    };

    // ------------------------------------------------------------------
    // service injector can be used for implicit dependency injection
    // ------------------------------------------------------------------

    class ServiceInjector final {
        safe_ptr<const ServicesStore> m_store{};

    public:
        explicit ServiceInjector(safe_ptr<const ServicesStore> shared_store) noexcept
            : m_store(std::move(shared_store)) {
            PPR_ASSERT(m_store.isValid());
        }

        template<typename T>
            requires std::is_base_of_v<IService, T>
        // ReSharper disable once CppNonExplicitConversionOperator
        [[nodiscard]] operator safe_ptr<T>() const noexcept {
            return m_store->get<T>();
        }
    };

    [[nodiscard]] ServiceInjector ServicesStore::inject() const noexcept {
        return ServiceInjector(safe_ptr(this));
    }
}
