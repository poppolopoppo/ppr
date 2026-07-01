module;
#include "pP/Macros.h"
export module engine.core:input_key;
import :types;
import std;

export namespace pP {

    struct InputKey : Numeric<u32, InputKey> {
        using Numeric::Numeric;
    };

    namespace keys {
        inline constexpr InputKey unknown{0};
        inline constexpr InputKey a{1}, b{2}, c{3}, d{4}, e{5}, f{6}, g{7}, h{8}, i{9}, j{10};
        inline constexpr InputKey k{11}, l{12}, m{13}, n{14}, o{15}, p{16}, q{17}, r{18}, s{19};
        inline constexpr InputKey t{20}, u{21}, v{22}, w{23}, x{24}, y{25}, z{26};
        inline constexpr InputKey num0{27}, num1{28}, num2{29}, num3{30}, num4{31};
        inline constexpr InputKey num5{32}, num6{33}, num7{34}, num8{35}, num9{36};
        inline constexpr InputKey space{37}, enter{38}, escape{39}, tab{40}, backspace{41};
        inline constexpr InputKey leftShift{42}, rightShift{43};
        inline constexpr InputKey leftCtrl{44}, rightCtrl{45};
        inline constexpr InputKey leftAlt{46}, rightAlt{47};
        inline constexpr InputKey mouseLeft{101}, mouseRight{102}, mouseMiddle{103};
        inline constexpr InputKey mouseX{104}, mouseY{105};
    }

} // namespace pP
