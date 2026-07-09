module;
#include "pP/Macros.h"
export module engine.core:service;

import :containers;
import :containers.flat_map;
import :hal;
import :memory.pointer;

export namespace pP {
    // ------------------------------------------------------------------
    // services are keyed by constexpr interface type unique identifier
    // ------------------------------------------------------------------

    template<typename>
    [[nodiscard]] consteval hash_t typeUid() noexcept {
#if defined(_MSC_VER)
        constexpr std::string_view signature = __FUNCSIG__;
#else
        constexpr std::string_view signature = __PRETTY_FUNCTION__;
#endif
        return hash_t{hash::mix(hash::fnv1a(signature))};
    }

    // ------------------------------------------------------------------
    // service base interface, can be referenced safely with safe_ptr<>
    // ------------------------------------------------------------------

    class IService : public safe_object {
    public:
        // ReSharper disable once CppHidingFunction
        virtual ~IService() noexcept = default;

        struct Uid {
            hash_t m_hash_value{};

            constexpr Uid() noexcept = default;

            template<typename T>
                requires std::is_base_of_v<IService, T>
            constexpr Uid() noexcept
                : m_hash_value{typeUid<T>()} {
            }

            explicit constexpr Uid(const hash_t hash_value) noexcept
                : m_hash_value{hash_value} {
            }

            [[nodiscard]] friend bool operator==(const Uid lhs, const Uid rhs) noexcept {
                return lhs.m_hash_value == rhs.m_hash_value;
            }

            [[nodiscard]] friend std::strong_ordering operator<=>(const Uid lhs, const Uid rhs) noexcept {
                return lhs.m_hash_value <=> rhs.m_hash_value;
            }

            friend void swap(Uid &lhs, Uid &rhs) noexcept {
                swap(lhs.m_hash_value, rhs.m_hash_value);
            }

            [[nodiscard]] friend constexpr hash_t hashValue(const Uid uid) noexcept {
                return uid.m_hash_value;
            }
        };
    };

    // ------------------------------------------------------------------
    // services are stored inside a flat map, for fast, cache-coherent lookups
    // ------------------------------------------------------------------

    class ServiceInjector;

    class ServicesStore : public safe_object {
        mutable std::shared_mutex m_shared_mutex{};
        FlatMap<IService::Uid, safe_ptr<IService> > m_services{};
        safe_ptr<ServicesStore> m_parent{};

    public:
        ServicesStore() noexcept = default;

        explicit ServicesStore(safe_ptr<ServicesStore> parent) noexcept
            : m_parent{std::move(parent)} {
        }

        void reset() noexcept {
            const std::unique_lock write_lock{m_shared_mutex};
            m_services.reset();
            m_parent = nullptr;
        }

        template<typename T>
            requires std::is_base_of_v<IService, T>
        bool insert(safe_ptr<T> &&service) {
            const IService::Uid service_key{typeUid<T>()};

            const std::unique_lock write_lock{m_shared_mutex};
            return m_services.insert({service_key, std::move(service).template upcast<IService>()}).second;
        }

        template<typename T>
            requires std::is_base_of_v<IService, T>
        bool erase() noexcept {
            const IService::Uid service_key{typeUid<T>()};

            const std::unique_lock write_lock{m_shared_mutex};
            return m_services.erase(service_key);
        }

        template<typename T>
            requires std::is_base_of_v<IService, T>
        [[nodiscard]] safe_ptr<T> tryGet() const noexcept {
            std::vector<const ServicesStore *> visited;
            auto *current = this;
            while (current) {
                for (const auto *v: visited) {
                    if (v == current) {
                        return safe_ptr<T>{};
                    }
                }
                visited.push_back(current);

                const IService::Uid service_key{typeUid<T>()};
                const std::shared_lock read_lock{current->m_shared_mutex};
                if (const auto it = current->m_services.find(service_key); it.isValid()) {
                    return checked_cast<T>(it->second);
                }
                current = current->m_parent.get();
            }
            return safe_ptr<T>{};
        }

        template<typename T>
            requires std::is_base_of_v<IService, T>
        [[nodiscard]] safe_ptr<T> get() const noexcept {
            const safe_ptr<T> ptr = tryGet<T>();
            PPR_ASSERT(ptr.isValid() && "get() called on unknown service");
            return ptr;
        }

        [[nodiscard]] ServiceInjector inject() const noexcept;
    };

    // ------------------------------------------------------------------
    // service injector can be used for implicit dependency injection
    // ------------------------------------------------------------------

    class ServiceInjector {
        safe_ptr<const ServicesStore> m_store;
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
