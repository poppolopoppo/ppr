module;
#include "pP/Macros.h"
module engine.core;

import :service;
import std;

namespace pP {
    bool ServicesStore::insert_(std::type_index service_key, safe_ptr<IService> &&service) {
        const std::unique_lock write_lock{m_shared_mutex};
        return m_services.emplace(std::move(service_key), std::move(service)).second;
    }

    void ServicesStore::insert_or_assign_(std::type_index service_key, safe_ptr<IService> &&service) {
        const std::unique_lock write_lock{m_shared_mutex};
        m_services.insert_or_assign(std::move(service_key), std::move(service));
    }

    bool ServicesStore::release_(std::type_index service_key, const IService &service) {
        const std::unique_lock write_lock{m_shared_mutex};
        if (const auto it = m_services.find(service_key); it != m_services.end() && it->second == std::addressof(service)) [[likely]] {
            m_services.erase(it);
            return true;
        }
        return false;
    }

    safe_ptr<IService> ServicesStore::tryGet_(const std::type_index service_key) const noexcept {
        std::shared_lock reader_lock{m_shared_mutex};
        if (const auto it = m_services.find(service_key); it != m_services.end()) [[likely]] {
            return it->second;
        }

        if (m_parent.isValid()) {
            reader_lock.unlock();
            return m_parent->tryGet_(service_key);
        }
        return nullptr;
    }

    void ServicesStore::reset() noexcept {
        const std::unique_lock write_lock{m_shared_mutex};
        m_services.clear();
    }
}
