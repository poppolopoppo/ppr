module;
#include "pP/Macros.h"
module engine.core;

import :service;
import std;

namespace pP {

void ServicesStore::reset() noexcept {
    const std::unique_lock write_lock{m_shared_mutex};
    m_services.clear();
    m_parent = nullptr;
}

}
