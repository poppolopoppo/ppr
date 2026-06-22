module;
#include "pP/Macros.h"

module engine.core;
import :containers.flat_map;
import std;

template class pP::FlatMap<pP::u32, pP::u32>;
template class pP::FlatMap<pP::u64, pP::u64>;
template class pP::FlatMap<pP::u32, float>;
