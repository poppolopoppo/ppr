import engine.core;
import engine.math;
import engine.rhi;
import engine.app;

import std;

#include "pP/Macros.h"

struct Toto {
    int m_age{0};

    inline static bool g_verbose{false};

    Toto() noexcept {
        if (g_verbose) {
            pP::hal::outputDebug(TEXT("toto default constructor\n"));
        }
    }

    Toto(int age) noexcept : m_age(age) {
        if (g_verbose) {
            pP::hal::outputDebugFmt(TEXT("toto constructor with age: {}\n"), age);
        }
    }

    Toto(const Toto &other) noexcept : m_age(other.m_age) {
        if (g_verbose) {
            pP::hal::outputDebugFmt(TEXT("toto copy constructor with age: {}\n"), other.m_age);
        }
    }

    Toto &operator=(const Toto &other) noexcept {
        m_age = other.m_age;
        if (g_verbose) {
            pP::hal::outputDebugFmt(TEXT("toto copy assignment operator with age: {}\n"), other.m_age);
        }
        return *this;
    }

    Toto(Toto &&rvalue) noexcept : m_age(rvalue.m_age) {
        if (g_verbose) {
            pP::hal::outputDebugFmt(TEXT("toto move constructor with age: {}\n"), rvalue.m_age);
        }
    }

    Toto &operator=(Toto &&rvalue) noexcept {
        m_age = rvalue.m_age;
        if (g_verbose) {
            pP::hal::outputDebugFmt(TEXT("toto move assignment operator with age: {}\n"), rvalue.m_age);
        }
        return *this;
    }

    ~Toto() noexcept {
        if (g_verbose) {
            pP::hal::outputDebugFmt(TEXT("toto destructor with age: {}\n"), m_age);
        }
    }
};

template<pP::details::TChar CharT>
struct std::formatter<Toto, CharT> : std::formatter<int, CharT> {
    template<typename FormatContextT>
    auto format(const Toto &value, FormatContextT &ctx) const -> decltype(ctx.out()) {
        return std::formatter<int, CharT>::format(value.m_age, ctx);
    }
};


// ------------------------------------------------------------------
// tests
// ------------------------------------------------------------------

template <std::size_t A, std::size_t B>
struct same_size {
    static constexpr bool value() noexcept {
        static_assert(A == B);
        return A == B;
    }
};

template <typename T, std::size_t SizeBytes>
struct check_size : same_size<sizeof(T), SizeBytes> {

};

int main(int argc, char *argv[]) {
    using namespace pP;
    std::println("Starting Encapsulated Video Game App...");

    // 1. Math check
    const auto perspective = math::float4x4::perspectiveD3D(4.0f, 3.0f, 0.1f, 1000.0f);
    //std::cout << "Math perspective matrix created successfully (Left-Handed, Z-to-1).\n";


    /*pP::hal::outputDebugFmt(TEXT("platform name: {}\n"), pP::hal::platformName());
    pP::hal::outputDebugFmt(TEXT("user name: {}\n"), pP::hal::userName());*/

    hal::outputDebugFmt(TEXT("home dir: {}\n"), hal::homeDir().path().native());
    hal::outputDebugFmt(TEXT("system dir: {}\n"), hal::systemDir().path());

    hal::outputDebugFmt(TEXT("app local dir: {}\n"), hal::appDataLocalDir());
    hal::outputDebugFmt(TEXT("app roaming dir: {}\n"), hal::appDataRoamingDir().path().native());

    std::println("app local dir: {}\n", hal::appDataLocalDir());


    mem::BitmapTree::BuildInfos btree_infos{256};
    const auto btree_data = mem::GPA::allocateRaw(btree_infos.getAllocationSize(), max_align_v);
    PPR_DEFER { mem::GPA::deallocateRaw(btree_data.ptr, btree_data.count, max_align_v); };

    mem::BitmapTree btree;
    btree.initialize(btree_infos, btree_data, false);

    u32 num_pages_allocated = 0u;

    auto print_tree = [&]() {
        hal::outputDebugFmt(TEXT("number of pages allocated: {}\n"), num_pages_allocated);
        for (std::size_t d = 0u; d < btree_infos.m_tree_depth; d++) {
            const auto nodes = btree.nodesAtDepth(btree_infos, d);
            hal::outputDebugFmt(TEXT("[{}] "), d);
            for (auto w: nodes) {
                hal::outputDebugFmt(TEXT("{:064b}"), w);
            }
            hal::outputDebug(TEXT("\n"));
        }
    };

    auto alloc_tree = [&]() -> u32 {
        bool need_commit = false;
        const u32 page_index = btree.allocate(btree_infos, need_commit);
        if (need_commit) {
            hal::outputDebugFmt(TEXT("commit new page at: {} (alloc at {})\n"),
                                      alignBackward(page_index, mem::BitmapTree::word_bit_count), page_index);
        }
        num_pages_allocated++;
        hal::outputDebugFmt(TEXT("alloc new page @{}\n"), page_index);
        return page_index;
    };

    auto alloc_batch_tree = [&](const u32 n) -> auto {
        bool need_commit = false;
        const auto batch = btree.allocateContiguous(btree_infos, n, need_commit);
        if (need_commit) {
            hal::outputDebugFmt(TEXT("commit new page at: {} (alloc at {})\n"),
                                      alignBackward(batch.m_first_bit, mem::BitmapTree::word_bit_count), batch.m_first_bit);
        }
        num_pages_allocated += batch.m_bit_count;
        hal::outputDebugFmt(TEXT("alloc new batch @{}:{}\n"), batch.m_first_bit, batch.m_first_bit + batch.m_bit_count);
        return batch;
    };

    auto dealloc_tree = [&](const u32 page_index) -> u32 {
        if (btree.deallocate(btree_infos, page_index)) {
            hal::outputDebugFmt(TEXT("decommit unused page at: {} (alloc at {})\n"),
                                      alignBackward(page_index, mem::BitmapTree::word_bit_count), page_index);
        }
        num_pages_allocated--;
        hal::outputDebugFmt(TEXT("dealloc page @{}\n"), page_index);
        return page_index;
    };

    auto print_range = [](const wstring_literal name, const auto &range) {
        hal::outputDebugFmt(TEXT("--->> {}:\n"), name.view());
        for (const auto &[index, item]: std::views::enumerate(range)) {
            hal::outputDebugFmt(TEXT("[{}] = {}\n"), index, item);
        }
    };

    print_tree();

    const auto alloc0 = alloc_tree();
    const auto alloc1 = alloc_tree();
    PPR_ASSERT(alloc0 != alloc1);
    PPR_ASSERT(btree.isAllocated(btree_infos, alloc0));
    PPR_ASSERT(btree.isAllocated(btree_infos, alloc1));

    print_tree();

    dealloc_tree(alloc0);
    PPR_ASSERT(!btree.isAllocated(btree_infos, alloc0));

    print_tree();

    const auto alloc2 = alloc_tree();
    PPR_ASSERT(alloc2 != alloc1);
    PPR_ASSERT(btree.isAllocated(btree_infos, alloc1));
    PPR_ASSERT(btree.isAllocated(btree_infos, alloc2));

    print_tree();

    const auto batch0 = alloc_batch_tree(8u);
    PPR_ASSERT(btree.isAllocated(btree_infos, alloc1));
    PPR_ASSERT(btree.isAllocated(btree_infos, alloc2));

    print_tree();

    dealloc_tree(alloc1);
    dealloc_tree(alloc2);
    PPR_ASSERT(!btree.isAllocated(btree_infos, alloc1));
    PPR_ASSERT(!btree.isAllocated(btree_infos, alloc2));

    print_tree();

    const auto batch1 = alloc_batch_tree(3u);

    print_tree();

    for (u32 i = 0u; i < batch1.m_bit_count; ++i) {
        dealloc_tree(batch1.m_first_bit + i);
    }

    print_tree();

    for (u32 i = 0u; i < batch0.m_bit_count; ++i) {
        dealloc_tree(batch0.m_first_bit + i);
    }

    print_tree();

    PPR_EXPR_IF_DEBUG(PPR_ASSERT(btree.isEmpty_forAssert(btree_infos)));

    Stack<int, 8> stack;
    stack.pushAssumeCapacity(1);
    stack.pushAssumeCapacity(2);
    sort::inplaceShell(stack);
    print_range(TEXT("stack"), stack);

    std::vector stl_vec = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    std::vector stl_vec2 = {2, 1, 0, 1, 2, 3, 11, 25, 13, 12, 15};
    stl_vec.insert(stl_vec.begin() + 3, stl_vec2.begin(), stl_vec2.end());
    sort::inplaceShell(stl_vec);
    print_range(TEXT("stl_vec"), stl_vec);

    StableVector vec = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    StableVector vec2 = {2, 1, 0, 1, 2, 3, 11, 25, 13, 12, 15};
    vec.insert(3, vec2);
    sort::inplaceShell(vec);
    print_range(TEXT("stable_vec"), vec);

    SparseVector<int> sparse_vec = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    SparseVector<int> sparse_vec2 = {2, 1, 0, 1, 2, 3, 11, 25, 13, 12, 15};

    auto it0 = sparse_vec.add(42);
    auto it1 = sparse_vec.add(69);
    auto it2 = sparse_vec.add(113);

    sparse_vec.append(sparse_vec2);
    print_range(TEXT("sparse_vec"), sparse_vec);

    sparse_vec.erase(it0);
    print_range(TEXT("sparse_vec"), sparse_vec);
    sparse_vec.erase(it2);
    print_range(TEXT("sparse_vec"), sparse_vec);
    sparse_vec.erase(it1);
    print_range(TEXT("sparse_vec"), sparse_vec);
    sparse_vec.append(std::initializer_list{43, 70, 114, 250});
    print_range(TEXT("sparse_vec"), sparse_vec);

    sparse_vec.clear();
    sparse_vec.append(vec);
    print_range(TEXT("sparse_vec"), sparse_vec);

    sparse_vec.reset();
    sparse_vec.append(vec);
    print_range(TEXT("sparse_vec"), sparse_vec);
    sparse_vec.append(vec);
    print_range(TEXT("sparse_vec"), sparse_vec);

    SparseVector<Toto> toto_vec;
    toto_vec.addDefault();
    const auto first_toto = toto_vec.add(Toto{42});
    toto_vec.emplace(69);
    print_range(TEXT("sparse_vec<Toto>"), toto_vec);
    toto_vec.erase(toto_vec.begin());
    auto *p_toto = toto_vec.tryGet(first_toto);
    PPR_ASSERT(p_toto);
    print_range(TEXT("sparse_vec<Toto>"), toto_vec);
    toto_vec.erase(first_toto);
    p_toto = toto_vec.tryGet(first_toto);
    PPR_ASSERT(p_toto == nullptr);
    print_range(TEXT("sparse_vec<Toto>"), toto_vec);
    toto_vec.emplace(113);
    p_toto = toto_vec.tryGet(first_toto);
    PPR_ASSERT(p_toto == nullptr);
    toto_vec.emplace(169);
    p_toto = toto_vec.tryGet(first_toto);
    PPR_ASSERT(p_toto == nullptr);
    print_range(TEXT("sparse_vec<Toto>"), toto_vec);
    toto_vec.clear();
    toto_vec.emplace(113);
    print_range(TEXT("sparse_vec<Toto>"), toto_vec);
    toto_vec.reset();

    std::initializer_list int_list{1, 2, 3, 4, 5, 6, 7, 8};

    SparseVectorInplace<int> inplace_vec;
    inplace_vec.append(int_list);
    print_range(TEXT("inplace_vec"), inplace_vec);
    inplace_vec.erase(++inplace_vec.begin());
    print_range(TEXT("inplace_vec"), inplace_vec);
    inplace_vec.add(43);
    print_range(TEXT("inplace_vec"), inplace_vec);
    inplace_vec.add(0);
    print_range(TEXT("inplace_vec"), inplace_vec);
    inplace_vec.reset();
    inplace_vec.add(0);
    print_range(TEXT("inplace_vec"), inplace_vec);

    const std::initializer_list int_keys{
        std::pair{1, 10},
        std::pair{2, 20},
        std::pair{3, 30},
        std::pair{4, 40},
    };
    HashMap int_map(int_keys);
    print_range(TEXT("int_map"), int_map);
    PPR_ASSERT(int_map.size() == int_keys.size());
    for (const auto &[key, ref_value]: int_map) {
        auto it = int_map.find(key);
        if (it == int_map.end()) {
            it = int_map.find(key);
        }
        PPR_ASSERT(it != int_map.end());
        PPR_ASSERT(it->first == key);
        PPR_ASSERT(it->second == ref_value);
        hal::outputDebugFmt(TEXT("int_map[{}] = {}\n"), key, it->second);
    }
    int_map.erase(3);
    print_range(TEXT("int_map"), int_map);
    PPR_ASSERT(int_map.size() + 1u == int_keys.size());

    HashSet<int> int_set(int_list);
    print_range(TEXT("int_set"), int_set);
    int_set.append(vec);
    print_range(TEXT("int_set"), int_set);
    int_set.append(vec2);
    print_range(TEXT("int_set"), int_set);

    HashSet<int> int_set2(int_list);
    print_range(TEXT("int_set2"), int_set2);
    for (auto it: vec) int_set2.insert(it);
    print_range(TEXT("int_set2"), int_set2);
    for (auto it: vec2) int_set2.insert(it);
    print_range(TEXT("int_set2"), int_set2);

    HashSet<int> int_set3(int_set2);
    print_range(TEXT("int_set3"), int_set3);

    PPR_ASSERT(int_set.size() == int_set2.size());
    PPR_ASSERT(int_set == int_set2);

    static_assert(std::is_empty_v<mem::GPA>, "pP::mem::GPA should be empty");
    static_assert(std::is_empty_v<mem::Allocator<mem::GPA>>, "pP::mem::GPA should be empty");
    static_assert(check_size<mem::GPA, 1u>::value(), "sizeof(pP::mem::GPA) == 1");

    struct toto_t : mem::Allocator<mem::GPA>{
        void *ptr{nullptr};
    };

    static_assert(check_size<toto_t, sizeof(void*)>::value(), "sizeof(toto_t) == void*");

    static_assert(check_size<mem::Allocator<mem::GPA>, 1u>::value(), "sizeof(pP::mem::Allocator<pP::mem::GPA>) == 1");
    static_assert(check_size<StableVector<int>, 16>::value(), "sizeof(pP::StableVector<int>) == 16");
    static_assert(check_size<SparseVector<int>, 24>::value(), "pP::SparseVector<int>) == 24");
    static_assert(check_size<SparseVectorInplace<int>, 96>::value(), "sizeof(pP::SparseVectorInplace<int>) == 96");

    static_assert(check_size<decltype(int_set), 24>::value(), "sizeof(HashSet<int>) == 24");

    // 2. App logic bounds
    app::Application app("ppr", std::span<const char * const>(&argv[0], argc));
    const int exit_code = app.run();

    math::float3 a{1, 2, 3};
    math::float3 b{4, 5, 6};
    [[maybe_unused]] auto v = (a + b) * .5f;
    [[maybe_unused]] auto n = math::normalize(v);
    [[maybe_unused]] auto d = math::distance(a, b);
    [[maybe_unused]] auto c = math::dot(a, b);
    [[maybe_unused]] auto d2 = math::sqrt(math::dot2(b - a));

#ifdef _MSC_VER
    std::println("MSVC version: {}", _MSC_VER);
    std::println("Full version: {}", _MSC_FULL_VER);
    std::println("Build: {}", _MSC_BUILD);
#else
    std::cout << "Not MSVC\n";
#endif

    return exit_code;
}
