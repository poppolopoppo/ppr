import playground;
import rendering;

//==========================================================================================
// Playground bindings
//__________________________________________________________________________________________
//==========================================================================================

[playground::MOUSE_POSITION]
uniform float4 un_mouse;
[playground::TIME]
uniform float un_time_elapsed;
[playground::FRAME_ID]
uniform float un_time_frame;

namespace Time
{
property float elapsed
{
    [ForceInline]
    get
    {
        return un_time_elapsed;
    }
}
property float delta
{
    [ForceInline]
    get
    {
        return Game.state.frame_time_delta;
    }
}
property uint frame
{
    [ForceInline]
    get
    {
        return uint(un_time_frame);
    }
}
property float game
{
    [ForceInline]
    get
    {
        return Game.state.game_time_elaspsed;
    }
}
}

namespace Mouse
{
property float2 pos
{
    [ForceInline]
    get
    {
        return un_mouse.xy;
    }
}
property float2 click_pos
{
    [ForceInline]
    get
    {
        return abs(un_mouse.zw);
    }
}
property bool is_down
{
    [ForceInline]
    get
    {
        return un_mouse.z < 0;
    }
}
#if 0 // this is not working
    property bool is_clicked
    {
        [ForceInline]
        get { return un_mouse.w < 0; }
    }
#endif
} //! namespace Mouse

//------------------------------------------------------------------------------------------
// - Keyboard
//------------------------------------------------------------------------------------------

[playground::KEY("ArrowUp")]
uniform uint key_up;
[playground::KEY("ArrowLeft")]
uniform uint key_left;
[playground::KEY("ArrowRight")]
uniform uint key_right;
[playground::KEY("ArrowDown")]
uniform uint key_down;

[playground::KEY("Control")]
uniform uint key_ctrl;
[playground::KEY("Shift")]
uniform uint key_shift;
[playground::KEY("Alt")]
uniform uint key_alt;

[playground::KEY("w")]
uniform uint key_W;
[playground::KEY("a")]
uniform uint key_A;
[playground::KEY("s")]
uniform uint key_S;
[playground::KEY("d")]
uniform uint key_D;

/*
[playground::KEY("0")]
uniform uint key_0;
[playground::KEY("1")]
uniform uint key_1;
[playground::KEY("2")]
uniform uint key_2;
[playground::KEY("3")]
uniform uint key_3;
[playground::KEY("4")]
uniform uint key_4;
[playground::KEY("5")]
uniform uint key_5;
[playground::KEY("6")]
uniform uint key_6;
[playground::KEY("7")]
uniform uint key_7;
[playground::KEY("8")]
uniform uint key_8;
[playground::KEY("9")]
uniform uint key_9;
*/
//==========================================================================================
// Main initialization pass
//__________________________________________________________________________________________
//==========================================================================================

#define MAP_SIZE 1024

// Must run as first shader
[shader("compute")]
[numthreads(1, 1, 1)]
[playground::CALL::ONCE]
[playground::CALL(1, 1, 1)]
void passMainInitialization(uint2 dispatchThreadID: SV_DispatchThreadID)
{
    var state = Game::State(Camera::Pan(
        float3(0, 0, 500), // position of the camera
        80,                // field of view in degrees
        Map.volume));

    Map::initElementTables();

    Shading::LightManager.reset();
    if (false)
    {
        let light = Shading::Light(
            state.camera.persp.eye,
            M_TAU,
            state.camera.persp.look_at,
            radians(60),
            float3(1, 1, 1),
            1.0, );
        state.frontal_light = Shading::LightManager.allocate(light).value;
    }

    Game.state = state;
}

//==========================================================================================
// Common helpers
//__________________________________________________________________________________________
//==========================================================================================

//------------------------------------------------------------------------------------------
// - FAT buffers: merge several semantics inside the same GPU resource (eg a RWStructuredBuffer)
//------------------------------------------------------------------------------------------

namespace Fat
{

interface IBuffer<T>
{
    static T load(uint idx);
    static void store(uint idx, T value);
};

extension<T, TBuffer : IBuffer<T>> TBuffer
{
    typealias Raw = Fat::View::Raw<T, This>;

    typealias Vec2 = Fat::View::Vector<T, This, 2>;
    typealias Vec3 = Fat::View::Vector<T, This, 3>;
    typealias Vec4 = Fat::View::Vector<T, This, 4>;

    struct Section<
        TView : IView<T, TBuffer>,
        let Count : uint,
        let Stride : uint,
        let Offset : uint> : ISection<T, TBuffer>
    {
        static const uint COUNT = Count * Stride;
        static const uint TAIL = Count * Stride + Offset;
        typealias View = TView;

        [ForceInline]
        static TView get()
        {
            static_assert(Stride == TView.STRIDE, "stride does not match view");
            return TView(Offset);
        }
    };

    struct PingPong<
        TView : IView<T, TBuffer>,
        let Count : uint,
        let Stride : uint,
        let Offset : uint> : ISection<T, TBuffer>
    {
        static const uint COUNT = Count * Stride * 2;
        static const uint TAIL = Count * Stride * 2 + Offset;
        typealias View = TView;

        [ForceInline]
        static TView get(uint phase)
        {
            static_assert(Stride == TView.STRIDE, "stride does not match view");
            let phase_offset = (phase & 1u) * Count * Stride;
            return TView(Offset + phase_offset);
        }
    };
};

extension<TBuffer : IBuffer<uint32_t>> TBuffer
{
    typealias Half2x16ToFloat = Fat::View::Half2x16ToFloat<TBuffer>;
    typealias RGB9E5 = Fat::View::RGB9E5<TBuffer>;

    struct Grid<let Width : uint, let Height : uint> : IView<uint32_t, TBuffer>
    {
        static const uint STRIDE = Width;

        constexpr uint offset;

        [ForceInline]
        constexpr __init(constexpr uint offset)
        {
            this.offset = offset;
        }

        [ForceInline]
        static uint linearIndex(int2 pos)
        {
            pos = clamp(pos, int2(0), int2(Width, Height)-1);
            return pos.y * Width + pos.x;
        }

        [ForceInline]
        [constref]
        constexpr uint32_t load(int2 pos)
        {
            return TBuffer.load(this.offset + linearIndex(pos));
        }

        [ForceInline]
        [constref]
        constexpr void store(int2 pos, uint32_t value)
        {
            TBuffer.store(this.offset + linearIndex(pos), value);
        }
    };

    struct Cube<let Width : uint, let Height : uint, let Depth : uint> : IView<uint32_t, TBuffer>
    {
        static const uint STRIDE = Width * Height;

        constexpr uint offset;

        [ForceInline]
        constexpr __init(constexpr uint offset)
        {
            this.offset = offset;
        }

        [ForceInline]
        static uint linearIndex(int3 pos)
        {
            pos = clamp(pos, int3(0), int3(Width, Height, Depth)-1);
            return (pos.z * Height + pos.y) * Width + pos.x;
        }

        [ForceInline]
        [constref]
        constexpr uint32_t load(int3 pos)
        {
            return TBuffer.load(this.offset + linearIndex(pos));
        }

        [ForceInline]
        [constref]
        constexpr void store(int3 pos, uint32_t value)
        {
            TBuffer.store(this.offset + linearIndex(pos), value);
        }
    };

    struct MipMaps<let Width: uint, let Height: uint, let Levels: uint> : IView<uint32_t, TBuffer>
    {
        static const uint AREA = Width * Height;
        static const uint STRIDE = uint(((AREA - (AREA >> (2 * Levels))) << 2) / 3);

        constexpr uint offset;

        [ForceInline]
        constexpr __init(constexpr uint offset)
        {
            this.offset = offset;
        }

        [ForceInline]
        static uint linearIndex(int2 pos, uint level)
        {
            let level_offset = ((AREA - (AREA >> (2 * level))) << 2) / 3;
            let stride = uint2(Width, Height) >> level;
            pos = clamp(pos, int2(0), stride-1);
            return level_offset + pos.y * stride.x + pos.x;
        }

        [ForceInline]
        [constref]
        constexpr uint32_t load(int2 pos, uint level)
        {
            return TBuffer.load(this.offset + linearIndex(pos, level));
        }

        [ForceInline]
        [constref]
        constexpr void store(int2 pos, uint level, uint32_t value)
        {
            TBuffer.store(this.offset + linearIndex(pos, level), value);
        }
    };

    struct MipMapVector<let Width: uint, let Height: uint, let Levels: uint, let Dim: uint> : IView<uint32_t, TBuffer>
    {
        static const uint AREA = Width * Height;
        static const uint STRIDE = Dim * uint(((AREA - (AREA >> (2 * Levels))) << 2) / 3);

        constexpr uint offset;

        [ForceInline]
        constexpr __init(constexpr uint offset)
        {
            this.offset = offset;
        }

        [ForceInline]
        static uint linearIndex(int2 pos, uint level)
        {
            let level_offset = (((AREA - (AREA >> (2 * level))) << 2) / 3);
            let stride = uint2(Width, Height) >> level;
            pos = clamp(pos, int2(0), stride-1);
            return (level_offset + pos.y * stride.x + pos.x) * Dim;
        }

        [ForceInline]
        [constref]
        constexpr vector<uint32_t, Dim> load(uint2 pos, uint level)
        {
            var result: vector<uint32_t, Dim>;
            let cursor = this.offset + linearIndex(pos, level);
            [ForceUnroll]
            for (uint i = 0u; i < Dim; i++)
            {
                result[i] = TBuffer.load(cursor + i);
            }
            return result;
        }

        [ForceInline]
        [constref]
        constexpr void store(uint2 pos, uint level, vector<uint32_t, Dim> value)
        {
            let cursor = this.offset + linearIndex(pos, level);
            [ForceUnroll]
            for (uint i = 0u; i < Dim; i++)
            {
                TBuffer.store(cursor + i, value[i]);
            }
        }
    };
};

interface ISection<T, TBuffer : IBuffer<T>>
{
    static const uint COUNT;
    static const uint TAIL;

    associatedtype View : IView<T, TBuffer>;
};

interface IView<T, TBuffer : IBuffer<T>>
{
    static const uint STRIDE;
    constexpr __init(constexpr uint offset);
};

namespace View {

struct Raw<T, TBuffer : IBuffer<T>> : IView<T, TBuffer>
{
    static const uint STRIDE = 1u;

    constexpr uint offset;

    [ForceInline]
    constexpr __init(constexpr uint offset)
    {
        this.offset = offset;
    }

    [ForceInline]
    [constref]
    constexpr T load(uint idx)
    {
        return TBuffer.load(this.offset + idx);
    }

    [ForceInline]
    [constref]
    constexpr void store(uint idx, T value)
    {
        TBuffer.store(this.offset + idx, value);
    }
};

struct Vector<T, TBuffer : IBuffer<T>, let Dim : uint> : IView<T, TBuffer> //, IBuffer<vector<T, Dim>>
{
    static const uint STRIDE = Dim;

    constexpr uint offset;

    [ForceInline]
    constexpr __init(constexpr uint offset)
    {
        this.offset = offset;
    }

    [ForceInline]
    [constref]
    constexpr vector<T, Dim> load(uint idx)
    {
        var result : vector<T, Dim>;
        [ForceUnroll]
        for (uint i = 0u; i < Dim; i++)
        {
            result[i] = TBuffer.load(this.offset + idx * Dim + i);
        }
        return result;
    }

    [ForceInline]
    [constref]
    constexpr void store(uint idx, vector<T, Dim> value)
    {
        [ForceUnroll]
        for (uint i = 0u; i < Dim; i++)
        {
            TBuffer.store(this.offset + idx * Dim + i, value[i]);
        }
    }
};

struct Half2x16ToFloat<TBuffer : IBuffer<uint32_t>> : IView<uint32_t, TBuffer>
{
    static const uint STRIDE = 2;

    constexpr uint offset;

    [ForceInline]
    constexpr __init(constexpr uint offset)
    {
        this.offset = offset;
    }

    [ForceInline]
    [constref]
    constexpr float4 load(uint idx)
    {
        return float4(
            unpackHalf2x16ToFloat(TBuffer.load(this.offset + idx * STRIDE + 0)),
            unpackHalf2x16ToFloat(TBuffer.load(this.offset + idx * STRIDE + 1))
            );
    }

    [ForceInline]
    [constref]
    constexpr void store(uint idx, float4 value)
    {
        TBuffer.store(this.offset + idx * STRIDE + 0, packHalf2x16(value.xy));
        TBuffer.store(this.offset + idx * STRIDE + 1, packHalf2x16(value.zw));
    }
};

struct RGB9E5<TBuffer : IBuffer<uint32_t>> : IView<uint32_t, TBuffer>
{
    static const uint STRIDE = 1;

    constexpr uint offset;

    [ForceInline]
    constexpr __init(constexpr uint offset)
    {
        this.offset = offset;
    }

    [ForceInline]
    [constref]
    constexpr float3 load(uint idx)
    {
        return Color::RGB9E5(TBuffer.load(this.offset + idx)).unpack();
    }

    [ForceInline]
    [constref]
    constexpr void store(uint idx, float3 value)
    {
        TBuffer.store(this.offset + idx, Color::RGB9E5(value).packed);
    }
};

} //! namespace View
}; //!namespace Fat

//------------------------------------------------------------------------------------------
// - Containers
//------------------------------------------------------------------------------------------

// The interface for an iterator type.
interface IIterator
{
    // An iterator needs to know how to move to the next element.
    This next();
    bool equals(This other);
};

[ForceInline]
bool operator ==<It : IIterator>(It lhs, It rhs)
{
    return lhs.equals(rhs);
}
[ForceInline]
bool operator !=<It : IIterator>(It lhs, It rhs)
{
    return !lhs.equals(rhs);
}
[ForceInline]
__postfix It operator ++<It : IIterator>(inout It iter)
{
    iter = iter.next();
    return iter;
}

interface IContainer<T>
{
    associatedtype Iterator : IIterator;

    property uint count { get; }

    Iterator begin();
    Iterator end();

    T at(Iterator iter);

    Iterator foreach (IFunc<bool, T> yield)
    {
        for (var iter = begin(); !iter.equals(end()); iter = iter.next())
        {
            let item = at(iter);
            if (yield(item))
                return iter;
        }
        return end();
    }

    Iterator foreach (IFunc<bool, T, Iterator> yield)
    {
        for (var iter = begin(); !iter.equals(end()); iter = iter.next())
        {
            let item = at(iter);
            if (yield(item, iter))
                return iter;
        }
        return end();
    }

    vector<float, N> accumulate<let N : uint>(vector<float, N> init, IFunc<vector<float, N>, T> yield)
    {
        var result = init;
        for (var iter = begin(); !iter.equals(end()); iter = iter.next())
        {
            let item = at(iter);
            result += yield(item);
        }
        return result;
    }

    vector<float, N> accumulate<let N : uint, Func>(vector<float, N> init, IFunc<vector<float, N>, T, Iterator> yield)
    {
        var result = init;
        for (var iter = begin(); !iter.equals(end()); iter = iter.next())
        {
            let item = at(iter);
            result += yield(item, iter);
        }
        return result;
    }
};

struct IndexIterator : IIterator
{
    uint index;

    [ForceInline]
    __init(uint index)
    {
        this.index = index;
    }

    [ForceInline]
    IndexIterator next()
    {
        return IndexIterator(index + 1);
    }
    [ForceInline]
    bool equals(IndexIterator other)
    {
        return (index == other.index);
    }
    [ForceInline]
    bool lessThan(IndexIterator other)
    {
        return (index < other.index);
    }
    [ForceInline]
    bool lessThanOrEquals(IndexIterator other)
    {
        return (index <= other.index);
    }
};

struct ArrayContainer<T, let N : uint> : IContainer<T>
{
    typealias Iterator = IndexIterator;

    Array<T, N> arr;

    property uint count
    {
        [ForceInline]
        get
        {
            return N;
        }
    }

    [ForceInline]
    Iterator begin()
    {
        return Iterator(0);
    }
    [ForceInline]
    Iterator end()
    {
        return Iterator(N);
    }

    [ForceInline]
    T at(Iterator iter)
    {
        return arr[iter.index];
    }
};

struct FuncContainer<T> : IContainer<T>
{
    typealias Iterator = IndexIterator;

    IFunc<T, uint> getter;
    uint count;

    __init(uint count, IFunc<T, uint> getter)
    {
        this.getter = getter;
        this.count = count;
    }

    [ForceInline]
    Iterator begin()
    {
        return Iterator(0);
    }
    [ForceInline]
    Iterator end()
    {
        return Iterator(count);
    }

    [ForceInline]
    T at(Iterator iter)
    {
        return getter(iter.index);
    }
};

struct FixedSizeContainer<T, let N : int> : IContainer<T>
{
    typealias Iterator = IndexIterator;

    static const int CAPACITY = N;
    Array<T, N> items;
    int count = 0;

    property bool empty
    {
        [ForceInline]
        get
        {
            return this.count == 0;
        }
    }
    property bool full
    {
        [ForceInline]
        get
        {
            return this.count == N;
        }
    }
    property int capacity
    {
        [ForceInline]
        get
        {
            return N;
        }
    }

    __subscript(int idx)->T
    {
        [ForceInline]
        get
        {
            return this.items[idx];
        }
        [ForceInline]
        set
        {
            this.items[idx] = newValue;
        }
    }

    [mutating]
    void push(T value)
    {
        if (this.count < N)
            this.items[this.count++] = value;
    }

    [mutating]
    void pushReservoir(T value, inout Rand::IPRNG rng, uint iteration)
    {
        var idx : uint = N;
        if (this.count < N)
        {
            idx = this.count++;
        }
        else
        {
            idx = rng.rand(uint(iteration));
            if (idx >= N)
                return;
        }
        this.items[idx] = value;
    }

    [mutating]
    Optional<T> pop()
    {
        if (this.count > 0)
            return this.items[--this.count];
        return none;
    }

    [ForceInline]
    Iterator begin()
    {
        return Iterator(0);
    }
    [ForceInline]
    Iterator end()
    {
        return Iterator(count);
    }

    [ForceInline]
    T at(Iterator iter)
    {
        return this.items[iter.index];
    }
};

struct Bitmask<let N : int>
{
    static const uint WORD_BITS = 32u; // sizeof(uint32_t) * 8
    static const uint WORD_SHIFT = 5u; // log2(32)
    static const uint WORD_MASK = 31u; // 32 - 1
    static const uint WORD_FULL = 0xFFFFFFFFu;
    static const uint CAPACITY = (WORD_BITS * N);

    vector<uint32_t, N> bits = vector<uint32_t, N>(WORD_FULL);

    [ForceInline]
    __init(vector<uint32_t, N> bits)
    {
        this.bits = bits;
    }

    __init(bool broadcast)
    {
        this.bits = vector<uint32_t, N>(select(broadcast, WORD_FULL, 0));
    }

    property uint count
    {
        [ForceInline]
        get
        {
            return dot(countbits<uint32_t, N>(bits), vector<uint32_t, N>(1));
        }
    }
    property bool empty
    {
        [ForceInline]
        get
        {
            return all(bits == vector<uint32_t, N>(0));
        }
    }
    property bool full
    {
        [ForceInline]
        get
        {
            return all(bits == vector<uint32_t, N>(WORD_FULL));
        }
    }

    __subscript(uint idx)->bool
    {
        [ForceInline]
        get
        {
            return contains(idx);
        }
        [ForceInline]
        set
        { set(idx, newValue); }
    }

    [ForceInline]
    bool contains(uint idx)
    {
        return (bits[idx >> WORD_SHIFT] & uint32_t(1u << (idx & WORD_MASK))) != uint32_t(0);
    }

    [mutating]
    void set(uint idx, bool value)
    {
        let w = (idx >> WORD_SHIFT);
        let mask = uint32_t(1u << (idx & WORD_MASK));
        bits[w] = select(value, bits[w] | mask, bits[w] & ~mask);
    }

    [mutating]
    void insert(uint idx)
    {
        bits[idx >> WORD_SHIFT] |= uint32_t(1u << (idx & WORD_MASK));
    }

    [mutating]
    void erase(uint idx)
    {
        bits[idx >> WORD_SHIFT] &= ~uint32_t(1u << (idx & WORD_MASK));
    }

    [ForceInline]
    Bitmask<N> invert()
    {
        return Bitmask<N>( ~this.bits );
    }

    [ForceInline]
    Bitmask<N> reverse()
    {
        return Bitmask<N>( reversebits(this.bits) );
    }

    [mutating]
    int allocate()
    {
        let bz = (this.bits != vector<uint32_t, N>(0));
        [ForceUnroll]
        for (uint idx = 0; idx < N; idx++)
        {
            if (bz[idx])
            {
                let bit = firstbitlow(this.bits[idx]);
                this.bits[idx] &= ~(uint32_t(1) << bit);
                return ((idx << WORD_SHIFT) | bit);
            }
        }
        return CAPACITY;
    }

    [ForceInline]
    [mutating]
    void release(int alloc)
    {
        if (alloc >= 0 && alloc < int(CAPACITY))
            this.release(uint(alloc));
    }

    [mutating]
    void release(uint alloc)
    {
        this.bits[alloc >> WORD_SHIFT] |= (uint32_t(1) << (alloc & WORD_MASK));
    }

    [ForceInline]
    [mutating]
    void reset()
    {
        this.bits = vector<uint32_t, N>( WORD_FULL );
    }
};

//------------------------------------------------------------------------------------------
// - Trigonometry
//------------------------------------------------------------------------------------------

static const float M_EPSILON = 1e-4;
static const float M_EPSILON_SQ = sqrt(M_EPSILON);
static const float M_PI = float.getPi();
static const float M_TAU = float.getPi()*2.0;
static const float M_PHI = (1.0 + sqrt(5.0)) / 2.0;

[ForceInline]
T sqr<T : __BuiltinArithmeticType>(T x)
{
    return x * x;
}

[ForceInline]
vector<float, N> sqr<let N : int>(vector<float, N> x)
{
    return x * x;
}

[ForceInline]
float dot2<let N : int>(vector<float, N> v)
{
    return dot(v, v);
}

[ForceInline]
float hsum<let N : int>(vector<float, N> v)
{
    return dot(v, vector<float, N>(1));
}

[ForceInline]
vector<float, N> linearStep<let N : int>(vector<float, N> vmin, vector<float, N> vmax, vector<float, N> x)
{
    return (x - vmin) / (vmax - vmin);
}

[ForceInline]
vector<float, N> smoothHermite<let N : int>(vector<float, N> x)
{
    return x * x * (3.0 - 2.0 * x);
}

[ForceInline]
vector<float, N> smootherStep<let N : int>(vector<float, N> edge0, vector<float, N> edge1, vector<float, N> x)
{
    let t = saturate((x - edge0) / (edge1 - edge0));
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

[ForceInline]
float ddxyLen(float ddx, float ddy)
{
    return length(float2(ddx, ddy));
}

float ddxyLenSq<let N : int>(vector<float, N> ddx, vector<float, N> ddy)
{
#if 0 // isotropic, max-norm
    return max(dot(ddx, ddx), dot(ddy, ddy));
#else // anisotropic, major axis of the footprint ellipse
    let a = dot(ddx, ddx);
    let b = dot(ddx, ddy);
    let c = dot(ddy, ddy);
    // major eigenvalue of JᵀJ
    let lambda_max = 0.5 * ((a + c) + sqrt((a - c) * (a - c) + 4.0 * b * b));
    return max(lambda_max, 0.0);
#endif
}

[ForceInline]
float ddxyLen<let N : int>(vector<float, N> ddx, vector<float, N> ddy)
{
    return sqrt(ddxyLenSq(ddx, ddy));
}

[ForceInline]
float ddxyRcpLen<let N : int>(vector<float, N> ddx, vector<float, N> ddy)
{
    return rsqrt(ddxyLenSq(ddx, ddy));
}

[ForceInline]
vector<float, N> normalizeSafe<let N : int>(vector<float, N> v, vector<float, N> fallback)
{
    let d2 = dot(v, v);
    return select(d2 > M_EPSILON_SQ, v * rsqrt(d2), fallback);
}

[ForceInline]
float2 unitDir(float phi)
{
    let rad = phi * M_TAU;
    return float2(cos(rad), sin(rad));
}

// Normalize angle to [-π, π]
[ForceInline]
float normalizeAngle(float rad)
{
    let wrapped = fmod(rad + M_PI, M_TAU) - M_PI;
    return select(wrapped <= -M_PI, wrapped + M_TAU, wrapped);
}

// Alternative: Normalize to [0, 2π]
[ForceInline]
float normalizeAnglePositive(float angle)
{
    return fract(angle / M_TAU) * M_TAU;
}

[ForceInline]
float3 cuteSort(float3 n)
{
    let a = min(min(n.x, n.y), n.z);
    let b = max(max(n.x, n.y), n.z);
    return float3(a, n.x + n.y + n.z - a - b, b);
}

[ForceInline]
float2 dir2(float phi)
{
    return float2(cos(phi), sin(phi));
}

// https://www.shadertoy.com/view/wdXSW8
float2 halton(uint index)
{
    static const float2 coprimes = float2(2, 3);
    float2 s = float2(index);
    float4 a = float4(1, 1, 0, 0);
    while (s.x > 0. && s.y > 0.)
    {
        a.xy = a.xy / coprimes;
        a.zw += a.xy * fmod(s, coprimes);
        s = floor(s / coprimes);
    }
    return a.zw;
}

//  http://holger.dammertz.org/stuff/notes_HammersleyOnHemisphere.html
//  http://www.pbr-book.org/3ed-2018/Sampling_and_Reconstruction/The_Halton_Sampler.html

//  While it looks like an arcane faerie incantation, it actually makes sense if you follow
//  the references. Bring some tea.

[ForceInline]
float radicalInverse(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return (float(bits) * 2.3283064365386963e-10); // / 0x100000000
}

float2 hammersley(int i, int N)
{
    return fract(float2(float(i) * 0.6180339887, radicalInverse(uint(i))));
}

// Branchless construction of an orthonormal basis.
// https://graphics.pixar.com/library/OrthonormalB/paper.pdf
void orthonormalBasis(float3 n, out float3 b1, out float3 b2)
{
    if (n.z < -1.0 + M_EPSILON) // Handle the singularity
    {
        b1 = float3(0.0, -1.0, 0.0);
        b2 = float3(-1.0, 0.0, 0.0);
        return;
    }
    let s = select(n.z >= 0.0, 1.0, -1.0);
    let a = -1.0 / (s + n.z);
    let b = n.x * n.y * a;
    b1 = float3(1.0 + s * n.x * n.x * a, s * b, -s * n.x);
    b2 = float3(b, s + n.y * n.y * a, -n.y);
}

float maxNorm<let N : int>(vector<float, N> v)
{
    var norm = abs(v[0]);
    [ForceUnroll]
    for (var i = 1; i < N; i++)
    {
        norm = max(norm, abs(v[i]));
    }
    return norm;
}

// https://liamtyler.github.io/posts/octahedral_analysis/
struct Octahedral
{
    static float2 encode(float3 v)
    {
        v = v / (abs(v.x) + abs(v.y) + abs(v.z));
        float2 oct;
        oct.x = v.x + v.y;
        oct.y = -v.x + v.y;

        if (v.z > 0)
            oct.x += 2;
        oct.x = 0.5 * oct.x - 0.5; // from [-1,3] to [-1, 1]

        return 0.5 * oct + +0.5; // from [-1, 1] to [0, 1]
    }

    static float3 decode(float2 oct)
    {
        float hemisphere = 1;
        oct.x *= 2; // [0, 2]
        if (oct.x > 1)
        {
            hemisphere = -1;
            oct.x -= 1; // [1, 2] -> [0, 1]
        }
        float2 octaPos = 2.0 * oct - 1.0; // [0, 1] -> [-1, 1]

        float3 v;
        v.x = 0.5 * (octaPos.x - octaPos.y);
        v.y = 0.5 * (octaPos.x + octaPos.y);
        v.z = hemisphere * (1.0 - abs(v.x) - abs(v.y));
        return normalize(v);
    }
};

//------------------------------------------------------------------------------------------
// - Collisions
//------------------------------------------------------------------------------------------

// https://iquilezles.org/articles/distfunctions/
// https://iquilezles.org/articles/intersectors/

namespace Collision
{
struct HitResult
{
    float3 normal = float3(0);
    float2 t = float2(float.maxValue);
    int id = -1;

    property bool valid
    {
        [ForceInline]
        get
        {
            return this.t.x < float.maxValue;
        }
    }

    property float penetration
    {
        [ForceInline]
        get
        {
            return max(this.t.y - this.t.x, 0.0);
        }
    }

    [ForceInline]
    float3 entry(Ray ray)
    {
        return ray.org + ray.dir * t.x;
    }

    [ForceInline]
    float3 exit(Ray ray)
    {
        return ray.org + ray.dir * select(t.y < float.maxValue, t.y, t.x);
    }

    void differentials(float3 pos, Ray ddx_ray, Ray ddy_ray,
                       out float3 pos_ddx,
                       out float3 pos_ddy)
    {
        // IQ: https://www.shadertoy.com/view/tdBXRW
        // Use safe division to avoid NaN at grazing angles (when ray is perpendicular to normal)
        let ddx_dn = dot(ddx_ray.dir, normal);
        let ddy_dn = dot(ddy_ray.dir, normal);
        let ddx_denom = select(abs(ddx_dn) > M_EPSILON, ddx_dn, M_EPSILON);
        let ddy_denom = select(abs(ddy_dn) > M_EPSILON, ddy_dn, M_EPSILON);
        let ddx_pos = ddx_ray.org - ddx_ray.dir * dot(ddx_ray.org - pos, normal) / ddx_denom;
        let ddy_pos = ddy_ray.org - ddy_ray.dir * dot(ddy_ray.org - pos, normal) / ddy_denom;

        pos_ddx = ddx_pos - pos;
        pos_ddy = ddy_pos - pos;
    }

    Shading::Geometry geometry(Ray ray, Ray ddx_ray, Ray ddy_ray)
    {
        var result : Shading::Geometry;
        result.surface_to_eye = -ray.dir;
        result.normal = this.normal;
        orthonormalBasis(result.normal, result.tangent, result.binormal);
        result.position = this.entry(ray);
        differentials(result.position, ddx_ray, ddy_ray, result.position_ddx, result.position_ddy);
        return result;
    }

    Shading::Geometry geometry(Ray ray)
    {
        var result : Shading::Geometry;
        result.surface_to_eye = -ray.dir;
        result.normal = this.normal;
        orthonormalBasis(result.normal, result.tangent, result.binormal);
        result.position = this.entry(ray);
        result.position_ddx = 0.5 * result.tangent;
        result.position_ddy = 0.5 * result.binormal;
        return result;
    }

    [ForceInline]
    Shading::Geometry geometry(float3 start, float3 end)
    {
        float dist;
        return this.geometry(Ray.fromSegment(start, end, dist));
    }

    [ForceInline]
    Shading::Geometry geometry(float2 start, float2 end)
    {
        var geo = this.geometry(float3(start,0), float3(end, 0));
        geo.position = Map.mapToWorld(geo.position.xy);
        return geo;
    }
};

[ForceInline]
bool operator<(HitResult a, HitResult b)
{
    return (a.t.x < b.t.x);
}

HitResult hitOverrideId(int id, HitResult hit)
{
    if (hit.valid)
    {
        hit.id = id;
    }
    return hit;
}

__transparent_block
{
    [ForceInline]
    HitResult min(HitResult a, HitResult b)
    {
        return (a.t.x < b.t.x ? a : b);
    }
}

struct Ray
{
    float3 org;
    float3 dir;

    static Ray fromSegment(float3 start, float3 end, out float dist)
    {
        let dir = (end - start);
        dist = length(dir);
        return Ray(start, select(dist > M_EPSILON, dir / dist, float3(0, 0, 1)));
    }

    [ForceInline]
    HitResult intersect<T : IVolume>(int id, T volume)
    {
        return volume.intersect(this);
    }
};

interface IDistanceField
{
    float euclideanDistance(float3 pos);
};

interface IRayTracer
{
    HitResult trace(float3 start, float3 end);
};

extension<T : IDistanceField> T : IRayTracer {
    [ForceInline] float3 euclideanGradient(float3 pos, float3 ddx, float3 ddy) {
        return euclideanGradient(pos, ddxyLen(ddx, ddy));
}

float3 euclideanGradient(float3 pos, float ddxy_len)
{
    let ddx = float3(ddxy_len, 0, 0);
    let ddy = float3(0, ddxy_len, 0);
    let ddz = float3(0, 0, ddxy_len);
    let dx = euclideanDistance(pos + ddx) - euclideanDistance(pos - ddx);
    let dy = euclideanDistance(pos + ddy) - euclideanDistance(pos - ddy);
    let dz = euclideanDistance(pos + ddz) - euclideanDistance(pos - ddz);
    return normalize(float3(dx, dy, dz) / (2.0 * ddxy_len));
}

float sphereTrace(Ray ray, float t_min, float t_max)
{
    var t = t_min;
    var p = ray.org + ray.dir * t_min;
    for (uint i = 0; i < 100 && t < t_max; ++i)
    {
        let d = euclideanDistance(p);
        if (abs(d) < M_EPSILON * max(t - t_min, 1.0))
            return t + d;

        let dt = abs(d) + M_EPSILON;
        t += dt;
        p += ray.dir * dt;
    }
    return t_max;
}

HitResult trace(float3 start, float3 end)
{
    var t_max = 0.0;
    let ray = Ray.fromSegment(start, end, t_max);
    var dist = this.sphereTrace(ray, 0, t_max);
    if (dist < t_max)
    {
        var hit : HitResult;
        hit.t = float2(dist);
        hit.normal = this.euclideanGradient(hit.entry(ray), 0.001);
        return hit;
    }
    return HitResult();
}
};

interface IVolume : IDistanceField
{
    bool contains(float3 pos);

    float3 closestPoint(float3 pos);

    HitResult intersect(Ray ray);
};

struct Empty : IVolume, IRayTracer
{
    float euclideanDistance(float3 pos)
    {
        return float.maxValue;
    }

    bool contains(float3 pos)
    {
        return false;
    }

    float3 closestPoint(float3 pos)
    {
        return pos;
    }

    HitResult intersect(Ray ray)
    {
        return HitResult();
    }

    HitResult trace(float3 start, float3 end)
    {
        return HitResult();
    }
};

struct Plane : IVolume
{
    float3 normal;
    float dist;

    __init(float3 pos, float3 normal)
    {
        this.normal = normal;
        dist = -dot(pos, normal);
    }

    property float3 origin
    {
        [ForceInline]
        get
        {
            return (normal * -dist);
        }
    }

    [ForceInline]
    float euclideanDistance(float3 pos)
    {
        return dot(pos, normal) + dist;
    }

    [ForceInline]
    bool contains(float3 pos)
    {
        return (euclideanDistance(pos) < M_EPSILON);
    }

    [ForceInline]
    float3 closestPoint(float3 pos)
    {
        return pos - (dot(normal, pos) + dist) * normal;
    }

    HitResult intersect(Ray ray)
    {
        let dn = dot(ray.dir, this.normal);
        let dx = -(dot(ray.org, this.normal) + this.dist) / dn;
        if (abs(dn) > M_EPSILON && dx >= 0.0)
        {
            var hit : HitResult;
            hit.t = float2(dx);
            hit.normal = this.normal;
            return hit;
        }
        return HitResult();
    }
};

struct Sphere : IVolume
{
    float3 center;
    float radius;

    [ForceInline]
    float euclideanDistance(float3 p)
    {
        return distance(p, center) - radius;
    }

    [ForceInline]
    bool contains(float3 pos)
    {
        return (dot2(pos - center) <= radius * radius);
    }

    [ForceInline]
    float3 closestPoint(float3 pos)
    {
        return center + normalize(pos - center) * radius;
    }

    HitResult intersect(Ray ray)
    {
        let L = center - ray.org;

        // Project L onto Ray Direction (tca = time to closest approach)
        let tca = dot(L, ray.dir);

        // Geometric check:
        // If tca < 0, the sphere center is behind the ray.
        // However, if we are INSIDE the sphere, we still need to hit it.
        // We defer the "behind" check until we solve for t0/t1.

        // Square distance from origin to center projected perpendicular to ray
        let d2 = dot(L, L) - tca * tca;

        // If perpendicular distance > radius, we miss
        let r2 = radius * radius;
        if (d2 > r2)
            return HitResult();

        // Distance from closest approach to intersection points
        let thc = sqrt(r2 - d2);

        // t0 = near intersection, t1 = far intersection
        var t0 = tca - thc;
        var t1 = tca + thc;

        // Logic to handle being inside the sphere or sphere behind camera
        if (t1 < M_EPSILON)
            return HitResult(); // Both intersections behind

        var hit : HitResult;
        // If t0 is negative (inside sphere), use t1
        hit.t.x = select(t0 > M_EPSILON, t0, t1);
        hit.t.y = t1;
        hit.normal = normalize((ray.org + ray.dir * hit.t.x) - center);
        // Normal points INWARDS if we are inside
        hit.normal = select(t0 > M_EPSILON, hit.normal, -hit.normal);
        return hit;
    }

    [ForceInline]
    bool intersect(Sphere other)
    {
        return dot2(this.center - other.center) <= sqr(this.radius + other.radius);
    }
};

struct Aabb : IVolume
{
    float3 center;
    float3 extent;

    __init(float3 center, float3 extent)
    {
        this.center = center;
        this.extent = extent;
    }

    [ForceInline]
    float3 min()
    {
        return center - extent;
    }
    [ForceInline]
    float3 max()
    {
        return center + extent;
    }

    float diagonal()
    {
        return length(2.0 * this.extent);
    }

    static Aabb fromPoints(float3 a, float3 b)
    {
        var result : Aabb;
        result.center = (a + b) * 0.5;
        result.extent = abs(b - a) * 0.5;
        return result;
    }

    [ForceInline]
    Sphere boundingSphere()
    {
        return Sphere(this.center, this.diagonal());
    }

    [ForceInline]
    float3 worldToObject(float3 world_pos)
    {
        return (((world_pos - center) / extent) + 1.0) / 2.0;
    }

    [ForceInline]
    float3 objectToWorld(float3 object_pos)
    {
        return (object_pos * 2.0 - 1.0) * this.extent + this.center;
    }

    float euclideanDistance(float3 p)
    {
        let q = abs(p - center) - extent;
        return length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0);
    }

    [ForceInline]
    bool contains(float3 pos)
    {
        return all(abs(pos - center) <= extent);
    }

    [ForceInline]
    float3 closestPoint(float3 pos)
    {
        return clamp(pos, center - extent, center + extent);
    }

    HitResult intersect(Ray ray)
    {
        // Handle rays parallel to axes
        let m = select(abs(ray.dir) > M_EPSILON, rcp(ray.dir), float3(1e10));
        let n = m * (ray.org - center);
        let k = abs(m) * extent;

        let t1 = -n - k;
        let t2 = -n + k;

        let tN = max(max(t1.x, t1.y), t1.z);
        let tF = min(min(t2.x, t2.y), t2.z);
        if (tN > tF || tF < M_EPSILON)
            return HitResult();

        var hit : HitResult;
        hit.t.x = select(tN > M_EPSILON, tN, tF);
        hit.t.y = tF;

        // Determine which face was hit by comparing entry distances
        // The face corresponding to the maximum t1 value is the one we hit
        let is_outside = (tN > M_EPSILON);
        let t12 = select(is_outside, t1, t2);
        let tNF = select(is_outside, tN, tF);
        hit.normal = float3(0);
        if (abs(t12.x - tNF) < M_EPSILON)
            hit.normal.x = sign(ray.dir.x);
        else if (abs(t12.y - tNF) < M_EPSILON)
            hit.normal.y = sign(ray.dir.y);
        else if (abs(t12.z - tNF) < M_EPSILON)
            hit.normal.z = sign(ray.dir.z);
        hit.normal = select(is_outside, -hit.normal, hit.normal);
        return hit;
    }
};

struct RoundedCone : IVolume
{
    float3 pa;
    float ra;
    float3 pb;
    float rb;

    [ForceInline]
    __init(float3 pa, float ra, float3 pb, float rb)
    {
        this.pa = pa;
        this.ra = ra;
        this.pb = pb;
        this.rb = rb;
    }

    float euclideanDistance(float3 p)
    {
        float3 ba = pb - pa;
        float l2 = dot(ba, ba);
        float rr = ra - rb;
        float d2 = l2 - rr * rr;

        float3 oa = p - pa;
        float m0 = dot(oa, ba);
        float m1 = dot(oa, oa);

        // Selection logic for the three regions: Cap A, Cap B, or Conical Body
        float k2 = d2;
        float k1 = d2 * m0 - m1 * rr * ra; // Error corrected: should be based on projection

        // Simplified but precise version for lighting culling:
        float h = m1 - m0 * m0 / l2; // Distance squared to infinite line
        float t = m0 / l2;           // Projection factor [0, 1]

        // Re-aligning t for the slanted conical surface
        // We solve for the point on the axis where the spheres tangent
        float x = m0 / l2;
        float y = sqrt(max(0.0, m1 - m0 * m0 / l2));

        // Analytical cone distance
        float k = d2;
        float f = saturate((rr * (y - ra) + x * l2) / (rr * rr + l2));

        float dist_x = y - ra - f * rr;
        float dist_y = x - f;

        // Check if point is "inside" the spheres or the conical sheath
        // Note: Sign is negative inside the volume
        return length(float2(dist_x, dist_y * sqrt(l2))) * sign(max(dist_x, abs(dist_y - 0.5) - 0.5));
    }

    [ForceInline]
    bool contains(float3 pos)
    {
        // Faster than the full SDF: check if the point is within the
        // convex hull of the two spheres.
        float3 ba = pb - pa;
        float l2 = dot(ba, ba);
        float rr = ra - rb;
        float3 oa = pos - pa;
        float m0 = dot(oa, ba);

        // Distance to axis
        float t = saturate(m0 / l2);
        float r_t = lerp(ra, rb, t);
        float3 p_axis = pa + t * ba;

        return dot(pos - p_axis, pos - p_axis) <= (r_t * r_t);
    }

    float3 closestPoint(float3 p)
    {
        float3 ba = pb - pa;
        float l2 = dot(ba, ba);
        float3 oa = p - pa;
        float t = saturate(dot(oa, ba) / l2);

        float r_t = lerp(ra, rb, t);
        float3 p_axis = pa + t * ba;
        float3 dir = p - p_axis;
        float d = length(dir);

        if (d < r_t)
            return p;
        return p_axis + (dir / d) * r_t;
    }

    HitResult intersect(Ray ray)
    {
        // Using the optimized intersection logic derived previously
        float3 ba = pb - pa;
        float3 oa = ray.org - pa;
        float3 ob = ray.org - pb;
        float rr = ra - rb;
        float m0 = dot(ba, ba);
        float m1 = dot(ba, oa);
        float m2 = dot(ba, ray.dir);
        float m3 = dot(ray.dir, oa);
        float m5 = dot(oa, oa);
        float m6 = dot(ob, ray.dir);
        float m7 = dot(ob, ob);

        float d2 = m0 - rr * rr;

        // Body (Conical part)
        float k2 = d2 - m2 * m2;
        float k1 = d2 * m3 - m1 * m2 + m2 * rr * ra;
        float k0 = d2 * m5 - m1 * m1 + m1 * rr * ra * 2.0 - m0 * ra * ra;

        float h = k1 * k1 - k0 * k2;

        if (h >= 0.0)
        {
            float t = (-k1 - sqrt(h)) / k2;
            float y = m1 - ra * rr + t * m2;
            if (y > 0.0 && y < d2 && t > 0.0)
            {
                HitResult hit;
                hit.t = float2(t);
                hit.normal = normalize(d2 * (oa + t * ray.dir) - ba * y);
                return hit;
            }
        }

        // Caps (Spherical parts)
        float h1 = m3 * m3 - m5 + ra * ra;
        float h2 = m6 * m6 - m7 + rb * rb;

        float t_min = 1e20;
        float3 n_min = float3(0);

        if (h1 > 0.0)
        {
            float t = -m3 - sqrt(h1);
            if (t > 0.0 && t < t_min)
            {
                t_min = t;
                n_min = (oa + t * ray.dir) / ra;
            }
        }
        if (h2 > 0.0)
        {
            float t = -m6 - sqrt(h2);
            if (t > 0.0 && t < t_min)
            {
                t_min = t;
                n_min = (ob + t * ray.dir) / rb;
            }
        }

        if (t_min < 1e19)
        {
            HitResult hit;
            hit.t = float2(t_min);
            hit.normal = n_min;
            return hit;
        }

        return HitResult();
    }

    bool intersect(Collision::Aabb bounds)
    {
        float3 axis = pb - pa;
        float h = length(axis);
        float3 n_axis = axis / h;
        float rr = ra - rb;

        float3 box_extents = bounds.extent;
        float3 box_center_rel = bounds.center - pa;

        // Slab: Project AABB interval onto axis
        float r_proj = dot(abs(n_axis), box_extents);
        float center_proj = dot(box_center_rel, n_axis);
        if (center_proj + r_proj < -ra || center_proj - r_proj > h + rb)
            return false;

        // Cap A (sphere at pa)
        if (Collision::Sphere(pa, ra).intersect(bounds.boundingSphere()))
            return true;

        // Cap B (sphere at pb)
        if (Collision::Sphere(pb, rb).intersect(bounds.boundingSphere()))
            return true;

        // Body: Full frustum test (conservative: check all AABB corners vs cone radius at proj t)
        bool intersects_body = false;
        [unroll]
        for (uint i = 0; i < 8; i++)
        { // 8 AABB corners
            let signs = float3((i & 1) != 0 ? 1 : -1, (i & 2) != 0 ? 1 : -1, (i & 4) != 0 ? 1 : -1);
            let corner = bounds.center + bounds.extent * signs;
            let corner_rel = corner - pa;
            let proj = saturate(dot(corner_rel, n_axis) / h);
            let r_at_proj = lerp(ra, rb, proj);
            let dist_to_axis = length(corner_rel - proj * axis);
            intersects_body = intersects_body || (dist_to_axis <= r_at_proj);
        }
        return intersects_body;
    }

    Sphere boundingSphere()
    {
        float3 axis = pb - pa;
        float h = length(axis);

        // Fallback for degenerate cone (two spheres at same spot)
        if (h < M_EPSILON)
            return Sphere(pa, max(ra, rb));

        // Solve for x (distance from pa along axis) where distance to
        // surface of sphere A equals distance to surface of sphere B.
        // Eq: sqrt(x^2 + ra^2) is for flat caps, but for rounded caps:
        // We simply want the sphere that touches the "outer" edges of both.

        // Radius of the circumsphere
        float radius = (h + ra + rb) * 0.5;

        // Center of the circumsphere
        // Offset from pa by (radius - ra)
        float3 center = pa + (axis / h) * (radius - ra);

        // Safety: If one sphere completely swallows the other
        if (ra >= h + rb)
            return Sphere(pa, ra);
        if (rb >= h + ra)
            return Sphere(pb, rb);

        return Sphere(center, radius);
    }
};

struct Scene<each T> : IVolume
    where T : IDistanceField, IVolume
{
    Tuple<expand each T> volumes;

    __init(expand each T volumes)
    {
        this.volumes = { expand each volumes };
    }

    float euclideanDistance(float3 p)
    {
        var d = float.maxValue;
        expand d = min(d, (each volumes).euclideanDistance(p));
        return d;
    }

    bool contains(float3 pos)
    {
        var result = false;
        expand result |= (each volumes).contains(pos);
        return result;
    }

    float3 closestPoint(float3 pos)
    {
        var result = float4(pos, float.maxValue);
        expand result = closest(pos, result, (each volumes).closestPoint(pos));
        return result.xyz;
    }

    HitResult intersect(Ray ray)
    {
        var hit = HitResult();
        var id : int = 0;
        expand hit = min(hit, hitOverrideId(id++, (each volumes).intersect(ray)));
        return hit;
    }

    [ForceInline]
    static float4 closest(float3 pos, float4 best, float3 candidate)
    {
        let d2 = dot2(pos - candidate);
        return select(d2 < best.w, float4(candidate, d2), best);
    }
};

__generic<each T>
Scene<expand each T> scene(expand each T volumes)
    where T : IDistanceField, IVolume
{
    return { expand each volumes };
}

// Returns the distance along the ray to the exit point
float intersectRayAABBInside(float2 origin, float2 dir, float4 box)
{
    // 1. Determine which planes are candidate exit planes based on direction signs.
    // If direction is positive, we check against box.max.
    // If direction is negative, we check against box.min.
    // We can use a branchless 'select' or 'lerp' for this selection.

    // Select the target planes based on direction signs
    float2 planes = select(dir >= 0.0, box.zw, box.xy);

    // 2. Calculate distances to those planes for each axis independently.
    // distance = (plane_coord - ray_origin) / ray_direction
    // Using precomputed inv_dir is faster: (plane - origin) * inv_dir
    float2 t = (planes - origin) / dir;

    // 3. The true exit distance is the CLOSEST of the two axis intersections.
    // Since we are inside, both 't.x' and 't.y' are guaranteed to be positive.
    // We just need the smallest one.
    return min(t.x, t.y);
}

} //! namespace Collision

//------------------------------------------------------------------------------------------
// - Matrix transforms
//------------------------------------------------------------------------------------------

namespace Transform
{

[ForceInline]
float2x2 rotate2D(float a)
{
    let s = sin(a);
    let c = cos(a);
    return float2x2(c, s, -s, c);
}

float2 rotate2D(float rad, float2 v)
{
    return mul(rotate2D(rad), v);
}

float3x3 rotateX(float theta)
{
    let c = cos(theta);
    let s = sin(theta);
    return float3x3(
        float3(1, 0, 0),
        float3(0, c, -s),
        float3(0, s, c));
}

float3x3 rotateY(float theta)
{
    let c = cos(theta);
    let s = sin(theta);
    return float3x3(
        float3(c, 0, s),
        float3(0, 1, 0),
        float3(-s, 0, c));
}

float3x3 rotateZ(float theta)
{
    let c = cos(theta);
    let s = sin(theta);
    return float3x3(
        float3(c, -s, 0),
        float3(s, c, 0),
        float3(0, 0, 1));
}

float3x3 rotateAround(float3 axis, float theta)
{
    let cosTheta = cos(theta);
    let sinTheta = sin(theta);
    let oneMinusCosTheta = saturate(1.0 - cosTheta);

    var rotationMatrix : float3x3;
    rotationMatrix[0][0] = cosTheta + axis.x * axis.x * oneMinusCosTheta;
    rotationMatrix[0][1] = axis.x * axis.y * oneMinusCosTheta - axis.z * sinTheta;
    rotationMatrix[0][2] = axis.x * axis.z * oneMinusCosTheta + axis.y * sinTheta;

    rotationMatrix[1][0] = axis.y * axis.x * oneMinusCosTheta + axis.z * sinTheta;
    rotationMatrix[1][1] = cosTheta + axis.y * axis.y * oneMinusCosTheta;
    rotationMatrix[1][2] = axis.y * axis.z * oneMinusCosTheta - axis.x * sinTheta;

    rotationMatrix[2][0] = axis.z * axis.x * oneMinusCosTheta - axis.y * sinTheta;
    rotationMatrix[2][1] = axis.z * axis.y * oneMinusCosTheta + axis.x * sinTheta;
    rotationMatrix[2][2] = cosTheta + axis.z * axis.z * oneMinusCosTheta;
    return rotationMatrix;
}

//  Return a world space sample vector based on a random hemisphere point, the surface normal
//  and the roughness of the surface.
//  https://google.github.io/filament/Filament.html#annex/choosingimportantdirectionsforsamplingthebrdf

// From tangent-space vector to world-space sample vector
float3 rotateToNormal(float3 L, float3 N)
{
    float3 tangent;
    float3 bitangent;
    orthonormalBasis(N, tangent, bitangent);

    tangent = normalize(tangent);
    bitangent = normalize(bitangent);

    return normalize(tangent * L.x + bitangent * L.y + N * L.z);
}

float3x3 rotation(float3 right, float3 up, float3 forward)
{
    let z = forward;
    let x = right;
    let y = up;
    return float3x3(x, y, z);
}

float4x4 translate(float3 t)
{
    return float4x4(
        float4(1., 0., 0., t.x),
        float4(0., 1., 0., t.y),
        float4(0., 0., 1., t.z),
        float4(0., 0., 0., 1.));
}

float4x4 translateInv(float3 t)
{
    return translate(-t);
}

float4x4 scale(float3 s)
{
    return float4x4(
        float4(s.x, 0., 0., 0.),
        float4(0., s.y, 0., 0.),
        float4(0., 0., s.z, 0.),
        float4(0., 0., 0., 1.));
}

float4x4 scaleInv(float3 s)
{
    return scale(rcp(s));
}

float4x4 projection(float n, float f)
{
    // n 0 0    0
    // 0 n 0    0
    // 0 0 n+f  -fn   ← Fixed: -fn in last column
    // 0 0 1    0     ← Fixed: 1 in z position
    return float4x4(
        float4(n, 0., 0., 0.),
        float4(0., n, 0., 0.),
        float4(0., 0., n + f, -f * n), // ✓ Fixed!
        float4(0., 0., 1., 0.));       // ✓ Fixed!
}

float4x4 projectionInv(float n, float f)
{
    // 1/n  0    0      0
    // 0    1/n  0      0
    // 0    0    0      1        ← Fixed: 1 in last column
    // 0    0    -1/fn  (f+n)/fn ← Fixed: both terms
    return float4x4(
        float4(1. / n, 0., 0., 0.),
        float4(0., 1. / n, 0., 0.),
        float4(0., 0., 0., 1.),                            // ✓ Fixed!
        float4(0., 0., -1. / (f * n), (f + n) / (f * n))); // ✓ Fixed!
}

// Reversed-Z Projection (Near=1, Far=0)
// This significantly improves depth precision for large scenes
float4x4 projectionRevZ(float n, float f)
{
    return float4x4(
        float4(n, 0, 0, 0),
        float4(0, n, 0, 0),
        float4(0, 0, 0, n), // At z=n, result is n/n = 1
        float4(0, 0, 1, 0)  // Note: Far plane is effectively at infinity here
    );
}

// Inverse Reversed-Z Projection
float4x4 projectionRevZInv(float n, float f)
{
    return float4x4(
        float4(1. / n, 0, 0, 0),
        float4(0, 1. / n, 0, 0),
        float4(0, 0, 0, 1.),
        float4(0, 0, 1. / n, 0));
}

float4x4 lookAt(float3 eye, float3 center, float3 up)
{
    let z = normalize(eye - center);
    let x = normalize(cross(up, z));
    let y = cross(z, x);

    float4x4 v = float4x4(
        float4(x.x, x.y, x.z, 0.),
        float4(y.x, y.y, y.z, 0.),
        float4(z.x, z.y, z.z, 0.),
        float4(0., 0., 0., 1.));

    return mul(v, translate(-eye));
}

float4x4 lookAtInv(float3 eye, float3 center, float3 up)
{
    let z = normalize(eye - center);
    let x = normalize(cross(up, z));
    let y = cross(z, x);

    float4x4 v_inv = float4x4(
        float4(x.x, y.x, z.x, 0.),
        float4(x.y, y.y, z.y, 0.),
        float4(x.z, y.z, z.z, 0.),
        float4(0., 0., 0., 1.));

    return mul(translate(eye), v_inv);
}

float4x4 orthographic(float l, float r, float b, float t, float n, float f)
{
    return mul(scale(float3(2. / (r - l), 2. / (t - b), 2. / (f - n))),
               translate(float3(-(l + r) / 2., -(t + b) / 2., -(f + n) / 2.)));
}

float4x4 orthographicInv(float l, float r, float b, float t, float n, float f)
{
    return mul(translateInv(float3(-(l + r) / 2., -(t + b) / 2., -(f + n) / 2.)),
               scaleInv(float3(2. / (r - l), 2. / (t - b), 2. / (f - n))));
}

float4x4 rightToLeft()
{
    return scale(float3(1., 1., -1.));
}

float4x4 rightToLeftInv()
{
    return rightToLeft();
}

float4x4 perspective(float fov, float aspect, float n, float f)
{
    let l = tan(fov / 2.) * n;
    let b = l / aspect;

    return mul(mul(orthographic(-l, l, -b, b, n, f),
                   projectionRevZ(n, f)),
               rightToLeft());
}

float4x4 perspectiveInv(float fov, float aspect, float n, float f)
{
    let l = tan(fov / 2.) * n;
    let b = l / aspect;

    return mul(mul(rightToLeftInv(),
                   projectionRevZInv(n, f)),
               orthographicInv(-l, l, -b, b, n, f));
}

float2x3 symmetricPack(float3x3 m)
{
    return float2x3(m[0][0], m[0][1], m[0][2],
                    m[1][1], m[1][2], m[2][2]);
}

float3x3 symmetricUnpack(float2x3 m)
{
    return float3x3(m[0][0], m[0][1], m[0][2],
                    m[0][1], m[1][0], m[1][1],
                    m[0][2], m[1][1], m[1][2]);
}

struct Quaternion
{
    float4 data = float4(1, 0, 0, 0);

    Quaternion normalized()
    {
        float length = sqrt(dot(data, data));
        return Quaternion( select(length > M_EPSILON, data / length, float4(1, 0, 0, 0)) );
    }

    float3x3 toRotation()
    {
        float w = data.x;
        float x = data.y;
        float y = data.z;
        float z = data.w;

        float xx = x * x;
        float yy = y * y;
        float zz = z * z;
        float xy = x * y;
        float xz = x * z;
        float yz = y * z;
        float wx = w * x;
        float wy = w * y;
        float wz = w * z;

        return float3x3(
            float3(1.0 - 2.0 * (yy + zz), 2.0 * (xy - wz), 2.0 * (xz + wy)),
            float3(2.0 * (xy + wz), 1.0 - 2.0 * (xx + zz), 2.0 * (yz - wx)),
            float3(2.0 * (xz - wy), 2.0 * (yz + wx), 1.0 - 2.0 * (xx + yy)));
    }

    static Quaternion fromRotation(float3x3 m)
    {
        let trace = m[0][0] + m[1][1] + m[2][2];

        var q : float4;
        if (trace > 0.0)
        {
            var s = sqrt(trace + 1.0);
            q.x = s * 0.5;
            s = 0.5 / s;
            q.y = (m[2][1] - m[1][2]) * s;
            q.z = (m[0][2] - m[2][0]) * s;
            q.w = (m[1][0] - m[0][1]) * s;
        }
        else
        {
            var i = 0;
            if (m[1][1] > m[0][0])
                i = 1;
            if (m[2][2] > m[i][i])
                i = 2;
            let j = (i + 1) % 3;
            let k = (i + 2) % 3;

            var s = sqrt(m[i][i] - m[j][j] - m[k][k] + 1.0);
            q[i + 1] = s * 0.5;
            s = 0.5 / s;
            q.x = (m[k][j] - m[j][k]) * s;
            q[j + 1] = (m[j][i] + m[i][j]) * s;
            q[k + 1] = (m[k][i] + m[i][k]) * s;
        }
        return Quaternion(q);
    }
};

__transparent_block
{
    Quaternion normalize(Quaternion q)
    {
        return q.normalized();
    }
}

} //! namespace Transform

//------------------------------------------------------------------------------------------
// - Camera
//------------------------------------------------------------------------------------------

namespace Camera
{

float2 fragmentCoord(uint2 dispatch_id, uint2 screen_size)
{
    // Pixel coordinates (center of pixel, origin at bottom left)
    return float2(dispatch_id.x, screen_size.y - 1 - dispatch_id.y) + 0.5;
}

float2 fragmentCoord(float2 screen_pos, float2 screen_size)
{
    // Pixel coordinates (center of pixel, origin at bottom left)
    return float2(screen_pos.x, screen_size.y - 1.0 - screen_pos.y);
}

struct Model
{
    float2 viewport;
    float2 z_range; // z_near, z_far - z_near

    float4x4 view;
    float4x4 inv_view;

    float4x4 projection;
    float4x4 inv_projection;

    float3 viewDir(float2 frag_coord)
    {
        let ndc = float4(frag_coord / viewport * 2.0 - 1.0, -1.0, 1.0);
        let eye = mul(inv_projection, ndc);
        return normalize(eye.xyz / eye.w);
    }

    Collision::Ray worldRay(float2 frag_coord)
    {
        let ndc = float4(frag_coord / viewport * 2.0 - 1.0, -1.0, 1.0);

        var ray : Collision::Ray;
        ray.org = mul(inv_view, float4(0, 0, 0, 1)).xyz;
        var eye = mul(inv_projection, ndc);

        eye = float4(normalize(eye.xyz / eye.w), 0);
        ray.dir = mul(inv_view, eye).xyz;
        ray.dir = normalize(ray.dir);
        return ray;
    }

    Collision::Ray worldRayDdxDdy(float2 frag_coord, out Collision::Ray ddx, out Collision::Ray ddy)
    {
        ddx = worldRay(frag_coord + float2(1, 0));
        ddy = worldRay(frag_coord + float2(0, 1));
        return worldRay(frag_coord);
    }

    float3 ndcToScreen(float3 screen_pos)
    {
        return float3(saturate((screen_pos.xy + 1.0) / 2.0) * viewport, screen_pos.z);
    }

    float3 viewToNdc(float3 view_pos)
    {
        let ndc = mul(projection, float4(view_pos, 1.0));
        return ndc.xyz / ndc.w;
    }

    float3 viewToScreen(float3 view_pos)
    {
        let ndc = viewToNdc(view_pos);
        return ndcToScreen(ndc);
    }

    float3 worldToNdc(float3 world_pos)
    {
        let view_pos = mul(view, float4(world_pos, 1.0));
        return viewToNdc(view_pos.xyz);
    }

    float3 worldToScreen(float3 world_pos)
    {
        let ndc = worldToNdc(world_pos);
        return ndcToScreen(ndc);
    }
};

interface IProjection
{
    Model project(float2 viewport);
};

struct LookAtPerspective : IProjection
{
    float3 eye = float3(0, 0, 5);
    float fov = radians(90);

    float3 look_at = float3(0, 0, 0);
    float z_near = 0.01;

    float3 up = float3(0, 1, 0);
    float z_far = 10000.0;

    __init(float3 eye, float3 look_at, float fov)
    {
        this.eye = eye;
        this.look_at = look_at;
        this.fov = radians(fov);
    }

    Model project(float2 viewport)
    {
        var result : Model;
        result.viewport = viewport;
        result.z_range = float2(z_near, z_far - z_near);

        result.view = Transform.lookAt(eye, look_at, up);
        result.inv_view = Transform.lookAtInv(eye, look_at, up);

        let aspect = (viewport.x / viewport.y);
        result.projection = Transform.perspective(fov, aspect, z_near, z_far);
        result.inv_projection = Transform.perspectiveInv(fov, aspect, z_near, z_far);
        return result;
    }
};

struct Axis<let N : int>
{
    vector<float, N> target;
    vector<float, N> smoothed;

    [ForceInline]
    __init(vector<float, N> value)
    {
        target = value;
        smoothed = value;
    }

    [ForceInline]
    [mutating]
    void reset(vector<float, N> value)
    {
        target = value;
        smoothed = value;
    }

    [ForceInline]
    [mutating]
    void set(vector<float, N> value)
    {
        target = value;
    }

    [mutating]
    vector<float, N> update(float dt, float sensitivity)
    {
        let smooth_rate = saturate(1.0 - exp(-dt * sensitivity));
        smoothed = lerp(smoothed, target, smooth_rate);
        return smoothed;
    }
};

interface IController : IProjection
{
    [mutating]
    Model update(float2 viewport, float dt, bool has_focus);
};

struct Orbit : IController
{
    LookAtPerspective persp;

    Axis<3> spherical_coords;

    float speed_walk = 30.0;
    float speed_run = 200.0;

    __init(LookAtPerspective init)
    {
        persp = init;
        let radius = distance(persp.eye, persp.look_at);
        spherical_coords = Axis<3>(float3(0, 0, radius));
    }

    Model project(float2 viewport)
    {
        return persp.project(viewport);
    }

    [mutating]
    Model update(float2 viewport, float dt, bool has_focus)
    {
        // Mouse → angles
        if (Mouse.is_down && has_focus)
        {
            let uv = smoothstep(float2(0), viewport, Mouse.pos);

            spherical_coords.target.x = (uv.x - 0.5) * M_TAU;       // left/right
            spherical_coords.target.y = (uv.y - 0.5) * M_PI * 0.45; // up/down, clamped
        }

        // Keyboard -> zoom
        let zoom_speed = select(key_shift != 0, speed_run, speed_walk) * dt;
        if (key_up != 0)
        {
            spherical_coords.target.z -= zoom_speed;
        }
        if (key_down != 0)
        {
            spherical_coords.target.z += zoom_speed;
        }
        spherical_coords.target.z = clamp(spherical_coords.target.z, persp.z_near, persp.z_far);

        // Keyboard -> fov
        let fov_speed = radians(select(key_shift != 0, 60.0, 15.0)) * dt;
        if (key_left != 0)
        {
            persp.fov -= fov_speed;
        }
        if (key_right != 0)
        {
            persp.fov += fov_speed;
        }
        persp.fov = clamp(persp.fov, radians(30), radians(180));

        let sph = spherical_coords.update(dt, M_TAU);

        let cosEl = cos(sph.y);
        let sinEl = sin(sph.y);

        persp.eye = float3(
            sph.z * cosEl * sin(sph.x),
            sph.z * sinEl,
            sph.z * cosEl * cos(sph.x));

        return persp.project(viewport);
    }
};

struct Pan : IController
{
    Collision::Aabb bounds;
    LookAtPerspective persp;

    Axis<2> pos;
    Axis<1> fov;
    Axis<1> zoom;

    uint pan_enabled = 0u;
    float2 pan_origin;

    float speed_walk = 10.0;
    float speed_run = 50.0;

    __init(float3 eye, float fov, Collision::Aabb bounds)
    {
        this.bounds = bounds;

        let look_at = bounds.closestPoint(eye);
        this.persp = LookAtPerspective(eye, look_at, fov);

        this.pos.reset(this.persp.eye.xy);
        this.fov.reset(this.persp.fov);
        this.zoom.reset(this.persp.eye.z - this.bounds.center.z);
    }

    Model project(float2 viewport)
    {
        return persp.project(viewport);
    }

    [mutating]
    Model update(float2 viewport, float dt, bool has_focus)
    {
        // Distance-based speed scaling (further = faster pan)
        let camera_distance = distance(this.persp.eye, this.persp.look_at);
        let adaptive_speed = camera_distance * 0.1; // Scale by 10% of distance
        let speed = select(key_shift != 0, this.speed_run, this.speed_walk) * dt * adaptive_speed;

        // Mouse drag&drop
        if (Mouse.is_down && has_focus)
        {
            if (this.pan_enabled == 0u)
            {
                this.pan_enabled = 1u;
                this.pan_origin = this.pos.smoothed;
            }

            // Calculate mouse delta in screen space
            let mouse_delta_screen = Mouse.pos - Mouse.click_pos;

            // Convert screen delta to normalized device coordinates
            let ndc_delta = (mouse_delta_screen / viewport) * 2.0;

            // Calculate world-space pan vector
            // Scale by distance and FOV for intuitive panning
            let fov_scale = tan(this.persp.fov * 0.5);
            let aspect = viewport.x / viewport.y;

            var pan_relative = ndc_delta * camera_distance * fov_scale;
            pan_relative.x *= -aspect;

            this.pos.target = this.pan_origin - pan_relative;
        }
        else
        {
            this.pan_enabled = 0u;
        }

        // Keyboard W/A/S/D -> pan in camera space
        if (key_A != 0)
            this.pos.target.x += speed;
        if (key_D != 0)
            this.pos.target.x -= speed;
        if (key_W != 0)
            this.pos.target.y -= speed;
        if (key_S != 0)
            this.pos.target.y += speed;

        // Keyboard Up/Down -> zoom (move along forward vector)
        if (key_up != 0)
            this.zoom.target -= speed;
        if (key_down != 0)
            this.zoom.target += speed;

        // Keyboard Left/Right -> fov
        let fov_speed = radians(select(key_shift != 0, 60.0, 15.0)) * dt;
        if (key_left != 0)
            this.fov.target -= fov_speed;
        if (key_right != 0)
            this.fov.target += fov_speed;

        // Apply camera constraints
        this.pos.target = clamp(this.pos.target, this.bounds.min().xy, this.bounds.max().xy);
        this.zoom.target = clamp(this.zoom.target, this.bounds.extent.z + this.persp.z_near, length(this.bounds.extent) * 5);
        this.fov.target = clamp(this.fov.target, radians(10), radians(180));

        // Smooth camera movement
        const float k_smooth = 10.0;
        this.persp.fov = this.fov.update(dt, 10.0);
        this.persp.eye = this.bounds.center + float3(
                                                  this.pos.update(dt, k_smooth),
                                                  -this.zoom.update(dt, k_smooth));
        this.persp.look_at = this.bounds.closestPoint(this.persp.eye);

        return persp.project(viewport);
    }
};

} //! namespace Camera

//------------------------------------------------------------------------------------------
// - Hashing
//------------------------------------------------------------------------------------------

[ForceInline]
uint2 hash(uint2 v)
{
    // A standard PCG-based mixer for 2D seeds
    // This provides high-quality decorrelation between adjacent pixels
    uint2 q = v * 1664525u + 1013904223u;

    q.x += q.y * 1664525u;
    q.y += q.x * 1664525u;

    q ^= q >> 16u;

    q.x += q.y * 1664525u;
    q.y += q.x * 1664525u;

    q ^= q >> 16u;
    return q;
}

uint4 hash(uint4 seed)
{
    var x = seed | uint4(1u);
    var y = seed.yzwx ^ uint4(0x9E3779B9u);
    var z = seed.zwxy ^ uint4(0x85EBCA6Bu);

    y ^= (y >> uint4(13u));
    y ^= (y << uint4(17u));
    y ^= (y >> uint4(5u));
    y *= uint4(0x2545F491u);

    x ^= y;
    x ^= (x >> uint4(13u));
    x ^= (x << uint4(17u));
    x ^= (x >> uint4(5u));
    x *= uint4(0x4F6CDD1Du);

    z ^= x;
    z ^= (z >> uint4(13u));
    z ^= (z << uint4(17u));
    z ^= (z >> uint4(5u));
    z *= uint4(0x1D6C45F4u);

    var h = z;
    h ^= (h >> uint4(16u));
    h *= uint4(0x7FEB352Du);
    h ^= (h >> uint4(15u));
    h *= uint4(0x846CA68Bu);
    h ^= (h >> uint4(16u));
    return h;
}

vector<float, N> hashToFloat<let N:uint>(vector<uint, N> h)
{
#if 1
    let w = (h >> 9) | 0x3f800000u;   // exponent = 1.0
    return bit_cast<vector<float, N>>(w) - 1.0;
#else
    let c = 2.3283064365387e-10;
    return saturate(c * vector<float, N>(h));
#endif
}

float hash(int2 p)
{
    let h = hash(bit_cast<uint2>(p).xyxy);
    let x = h.x;
    return hashToFloat(x);
}

// Hash without Sine https://www.shadertoy.com/view/4djSRW

float2 hash(float2 seed)
{
    var p3 = frac(float3(seed.xyx) * float3(.1031, .1030, .0973));
    p3 += dot(p3, p3.yzx+33.33);
    return fract((p3.xx+p3.yz)*p3.zy);
}

float3 hash(float3 seed)
{
    var p3 = frac(seed * float3(.1031, .1030, .0973));
    p3 += dot(p3, p3.yxz+33.33);
    return fract((p3.xxy + p3.yxx)*p3.zyx);
}

float4 hash(float4 seed)
{
    var p4 = frac(seed * float4(.1031, .1030, .0973, .1099));
    p4 += dot(p4, p4.wzxy+33.33);
    return frac((p4.xxyz+p4.yzzw)*p4.zywx);
}

namespace Noise
{

float interleavedGradient(int2 pixel, uint frame)
{
    // Avoid numeric drift by limiting frame range
    let f = float(frame % 64);
    let x = float(pixel.x) + 5.588238 * f;
    let y = float(pixel.y) + 5.588238 * f;

    // Compute the core interleaved gradient noise pattern
    let v = 0.06711056 * x + 0.00583715 * y;
    // frac() = value - floor(value), same as fmod(value, 1.0) for positive numbers
    let noise = frac(52.9829189 * fract(v));
    return noise;
}

// x: distance to edge
// y: cell center x
// z: cell center y
// w: cell class index
float4 voronoi(float2 x, float2 seed, float num_classes)
{
    let ip = floor(x);
    let fp = fract(x);

    //----------------------------------
    // first pass: regular voronoi
    //----------------------------------
	var mg: float2;
    var mr: float2;
    var mp: float2;
    var md = 8.0;
    var mb = 0.0;
    for (var j = -1; j <= 1; j++)
    for (var i = -1; i <= 1; i++)
    {
        let g = float2(i, j);
		let o = hash(float4(ip + g, seed)).xyz;

        let r = g + o.xy - fp;
        let d = dot(r,r);

        if (d < md)
        {
            md = d;
            mr = r;
            mg = g;
            mp = ip + g + o.xy;
            mb = floor(num_classes * o.z);
        }
    }

    //----------------------------------
    // second pass: distance to borders
    //----------------------------------
    md = 8.0;
    for (var j = -2; j <= 2; j++)
    for (var i = -2; i <= 2; i++)
    {
        let g = mg + float2(i, j);
		let o = hash(float4(ip + g, seed)).xyz;

        let r = g + o.xy - fp;
        let b = floor(num_classes * o.z);

        if (b != mb && dot(mr-r,mr-r) > M_EPSILON)
        {
            md = min(md, dot(0.5*(mr+r), normalize(r-mr)));
        }
    }

    return float4(md, mp, mb);
}

// Dual Voronoi - returns distance to edges connecting cell centers
// x: distance to nearest dual edge (line connecting adjacent cell centers)
// y: edge midpoint x
// z: edge midpoint y
// w: edge orientation (angle in radians)
float4 voronoiDual(float2 x, float2 seed, float num_classes)
{
    let ip = floor(x);
    let fp = fract(x);

    //----------------------------------
    // first pass: find our cell center
    //----------------------------------
    var md = 8.0;
    var mr: float2;
    var mg: float2;
    var mp: float2;
    var mb = 0.0;

    for (var j = -1; j <= 1; j++)
    for (var i = -1; i <= 1; i++)
    {
        let g = float2(i, j);
        let o = hash(float4(ip + g, seed)).xyz;
        let r = g + o.xy - fp;
        let d = dot(r, r);

        if (d < md)
        {
            md = d;
            mr = r;
            mg = g;
            mp = ip + g + o.xy;
            mb = floor(num_classes * o.z);
        }
    }

    //----------------------------------
    // second pass: find edges connecting to neighbors
    // Each edge goes from our center to an adjacent center
    //----------------------------------
    var min_edge_dist = 8.0;
    var edge_mid = mp;
    var edge_angle = 0.0;

    for (var j = -2; j <= 2; j++)
    for (var i = -2; i <= 2; i++)
    {
        let g = mg + float2(i, j);
        let o = hash(float4(ip + g, seed)).xyz;
        let neighbor_pos = ip + g + o.xy;
        let b = floor(num_classes * o.z);

        // Skip if it's our own cell or too far
        if (all(g == mg) || dot(neighbor_pos - mp, neighbor_pos - mp) > 4.0)
            continue;

        // Check if this neighbor is actually adjacent in the Voronoi diagram
        // by verifying the edge between centers crosses a Voronoi boundary
        let to_neighbor = neighbor_pos - mp;
        let edge_length = length(to_neighbor);

        if (edge_length < M_EPSILON) continue;

        let edge_dir = to_neighbor / edge_length;

        // Distance from point x to the line segment connecting mp and neighbor_pos
        let to_point = x - mp;
        let proj = clamp(dot(to_point, edge_dir), 0.0, edge_length);
        let closest_on_edge = mp + edge_dir * proj;
        let dist = length(x - closest_on_edge);

        // Verify this edge crosses a Voronoi boundary (i.e., these cells are neighbors)
        // Sample the midpoint of the edge - if it's equidistant from both centers, they're neighbors
        let edge_midpoint = (mp + neighbor_pos) * 0.5;
        let d1 = length(edge_midpoint - mp);
        let d2 = length(edge_midpoint - neighbor_pos);

        // Check other nearby cells to confirm these are actually neighbors
        var is_neighbor = true;
        for (var jj = -1; jj <= 1; jj++)
        for (var ii = -1; ii <= 1; ii++)
        {
            let gg = mg + float2(ii, jj);
            if (all(gg == mg) || all(gg == g)) continue;

            let oo = hash(float4(ip + gg, seed)).xyz;
            let other_pos = ip + gg + oo.xy;
            let d_other = length(edge_midpoint - other_pos);

            // If another cell center is closer to the midpoint, these aren't direct neighbors
            if (d_other < d1 - 0.01)
            {
                is_neighbor = false;
                break;
            }
        }

        if (is_neighbor && dist < min_edge_dist)
        {
            min_edge_dist = dist;
            edge_mid = closest_on_edge;
            edge_angle = atan2(edge_dir.y, edge_dir.x);
        }
    }

    return float4(min_edge_dist, edge_mid, edge_angle);
}

// Value noise 2D: https://www.shadertoy.com/view/lsf3WH

float value(float2 seed)
{
    let i = int2(floor(seed));
    let f = fract(seed);

     // cubic interpolant
    let u = f*f*(3.0-2.0*f);

    return lerp(lerp(   hash(i + int2(0,0)),
                        hash(i + int2(1,0)), u.x),
                lerp(   hash(i + int2(0,1)),
                        hash(i + int2(1,1)), u.x), u.y) * 2.0 - 1.0;
}

// Value noise with derivativs: https://www.shadertoy.com/view/4dXBRH

// return value noise (in z) and its derivatives (in xy)
float3 valueD(float2 p)
{
    let i = int2(floor( p ));
    let f = fract( p );

    // quintic interpolation
    let u = f*f*f*(f*(f*6.0-15.0)+10.0);
    let du = 30.0*f*f*(f*(f-2.0)+1.0);

    let va = hash(i + int2(0,0));
    let vb = hash(i + int2(1,0));
    let vc = hash(i + int2(0,1));
    let vd = hash(i + int2(1,1));

    float k0 = va;
    float k1 = vb - va;
    float k2 = vc - va;
    float k4 = va - vb - vc + vd;

    return float3(  du*(u.yx*(va-vb-vc+vd) + float2(vb,vc) - va),     // derivative
                    va+(vb-va)*u.x+(vc-va)*u.y+(va-vb-vc+vd)*u.x*u.y);// value
}

// Simplex Noise 2D: https://www.shadertoy.com/view/Msf3WH

float simplex(float2 seed)
{
    const float K1 = 0.366025404; // (sqrt(3)-1)/2;
    const float K2 = 0.211324865; // (3-sqrt(3))/6;
    let i = floor(seed + (seed.x+seed.y)*K1);
    let a = seed - i + (i.x+i.y)*K2;
    let o = step(a.yx,a.xy);
    let b = a - o + K2;
    let c = a - 1.0 + 2.0*K2;
    let h = max( 0.5-float3(dot(a,a), dot(b,b), dot(c,c) ), float3(0.) );
    let n = h*h*h*h*float3(
        dot(a, hash(i + 1.) * 2. - 1.),
        dot(b, hash(i + 1. + o) * 2. - 1.),
        dot(c, hash(i + 2.) * 2. - 1.));
    return dot( n, float3(70.0) );
}

// Simplex Noise 3D: https://www.shadertoy.com/view/XsX3zB

float simplex(float3 seed)
{
    /* skew constants for 3d simplex functions */
    const float F3 = 0.3333333;
    const float G3 = 0.1666667;

    /* 1. find current tetrahedron T and it's four vertices */
    /* s, s+i1, s+i2, s+1.0 - absolute skewed (integer) coordinates of T vertices */
    /* x, x1, x2, x3 - unskewed coordinates of p relative to each of T vertices*/

    /* calculate s and x */
    let s = floor(seed + dot(seed, float3(F3)));
    let x = seed - s + dot(s, float3(G3));

    /* calculate i1 and i2 */
    let e = step(float3(0.0), x - x.yzx);
    let i1 = e*(1.0 - e.zxy);
    let i2 = 1.0 - e.zxy*(1.0 - e);

    /* x1, x2, x3 */
    let x1 = x - i1 + G3;
    let x2 = x - i2 + 2.0*G3;
    let x3 = x - 1.0 + 3.0*G3;

    /* 2. find four surflets and store them in d */
    var w = float4(0.);
    var d = float4(0.);

    /* calculate surflet weights */
    w.x = dot(x, x);
    w.y = dot(x1, x1);
    w.z = dot(x2, x2);
    w.w = dot(x3, x3);

    /* w fades from 0.6 at the center of the surflet to 0.0 at the margin */
    w = max(0.6 - w, float4(0.0));

    /* calculate surflet components */
    d.x = dot(hash(s + 1.) - .5, x);
    d.y = dot(hash(s + 1. + i1) - .5, x1);
    d.z = dot(hash(s + 1. + i2) - .5, x2);
    d.w = dot(hash(s + 2.) - .5, x3);

    /* multiply d by w^4 */
    w *= w;
    w *= w;
    d *= w;

    /* 3. return the sum of the four surflets */
    return dot(d, float4(52.0));
}

float ridged(float n)
{
    var n = abs(n);
    n = saturate(1 - n);
    return n * n;
}

float turbulence(float n)
{
    return abs(n);
}

float fbm(float2 seed, uint octaves, IFunc<float, float2> noise)
{
    var value = 0.0;
    var amplitude = 0.5;
    var p = seed;

    // Rotation matrix to break grid patterns
    let rot = float2x2(0.8, 0.6, -0.6, 0.8);

    for (var i = 0u; i < octaves; i++)
    {
        value += amplitude * noise(p);
        p = mul(p, rot) * 2.01; // Rotate and scale
        amplitude *= 0.5;
    }

    return value;
}

float fbm(float2 seed, float2 ddx, float2 ddy, uint octaves, IFunc<float, float2> noise)
{
    var w = ddxyLen(ddx, ddy);
    var value = 0.0;
    var amplitude = 0.5;
    var p = seed;

    // Rotation matrix to break grid patterns
    let rot = float2x2(0.8, 0.6, -0.6, 0.8);

    for (var i = 0u; i < octaves; i++)
    {
        value += amplitude * noise(p) * smoothstep(6.0, 0.5, w);
        p = mul(p, rot) * 2.01; // Rotate and scale
        w *= 2.01;
        amplitude *= 0.5;
    }

    return value;
}

float fbm(float3 seed, uint octaves, IFunc<float, float3> noise)
{
    var value = 0.0;
    var amplitude = 0.5;
    var p = seed;

    // Simple orthonormal-ish rotation matrix
    let rot = float3x3(
         0.00,  0.80,  0.60,
        -0.80,  0.36, -0.48,
        -0.60, -0.48,  0.64
    );

    for (var i = 0u; i < octaves; i++)
    {
        value += amplitude * noise(p);
        p = mul(p, rot) * 2.01;
        amplitude *= 0.5;
    }

    return value * 0.5 + 0.5;
}

// return value noise (in z) and its derivatives (in xy)
float3 fbmD(float2 seed, uint octaves, IFunc<float3, float2> noise)
{
    var value = float3(0.0);
    var amplitude = 0.5;
    var p = seed;

    // Rotation matrix to break grid patterns
    let rot = float2x2(0.8, 0.6, -0.6, 0.8);

    for (var i = 0u; i < octaves; i++)
    {
        value += amplitude * noise(p);
        p = mul(p, rot) * 2.01; // Rotate and scale
        amplitude *= 0.5;
    }

    return value;
}

} //!namespace Noise

//------------------------------------------------------------------------------------------
// - Random
//------------------------------------------------------------------------------------------

namespace Rand
{
    interface IPRNG
    {
        [mutating]
        uint    randUint();
        [mutating]
        float   randFloat();
    };

    extension<Rng:IPRNG> Rng
    {
        [mutating]
        uint rand(uint vmin, uint vmax)
        {
            let range = (vmax - vmin + 1);
            let limit = uint.maxValue - (uint.maxValue % range);

            var rnd: uint;
            do {
                rnd = randUint();
            } while (rnd >= limit);

            return vmin + (rnd % range);
        }

        [ForceInline]
        [mutating]
        int rand(int vmin, int vmax)
        {
            return int(rand(0u, uint(vmax - vmin))) + vmin;
        }

        [ForceInline]
        [mutating]
        float rand(float vmin, float vmax)
        {
            return lerp(vmin, vmax, randFloat());
        }

        [ForceInline]
        [mutating]
        uint rand(uint vmax)
        {
            return rand(0u, vmax);
        }

        [ForceInline]
        [mutating]
        int rand(int vmax)
        {
            return rand(0, vmax);
        }

        [ForceInline]
        [mutating]
        float rand(float vmax)
        {
            return rand(0, vmax);
        }

        [mutating]
        // Returns a triangular distribution in [0,1] centered around 0.5
        float triangular()
        {
            let v = randFloat();
            let r2 = 0.5 * v;
            return select(v < 0.5, sqrt(r2), 1.0 - sqrt(r2 - 0.25));
        }

        [mutating]
        float2 boxMuller()
        {
            let u0 = randFloat();
            let u1 = randFloat();
            let r = sqrt(-2.0 * log(max(u0, M_EPSILON)));
            let theta = M_TAU * u1;
            return float2(sin(theta), cos(theta)) * r;
        }

        [mutating]
        // Returns a cosine-weighted unit vector on a hemisphere centered around n.
        float3 cosineWeightedHemisphere(float3 n)
        {
            let r = randFloat();
            let angle = randFloat() * M_TAU;
            let sr = sqrt(r);
            let p = float2(cos(angle), sin(angle)) * sr;
            let ph = float3(p.xy, sqrt(1.0 - dot(p, p)));

            var b1: float3;
            var b2: float3;
            orthonormalBasis(n, b1, b2);
            return b1 * ph.x + b2 * ph.y + n * ph.z;
        }
    };

    // Efficient pseudorandom generator.
    // https://developer.nvidia.com/gpugems/gpugems3/part-vi-gpu-computing/chapter-37-efficient-random-number-generation-and-application
    struct HybridTaus : IPRNG
    {
        uint4 state;

        [ForceInline]
        __init(uint4 seed)
        {
            state = hash(seed);
        }

        [mutating]
        uint randUint()
        {
            state.x = tausStep(state.x, 13u, 19u, 12u, 4294967294u);
            state.y = tausStep(state.y, 2u, 25u, 4u, 4294967288u);
            state.z = tausStep(state.z, 3u, 11u, 17u, 4294967280u);
            state.w = lcgStep(state.w, 1664525u, 1013904223u);
            return (state .x ^ state.y ^ state.z ^ state.w);
        }

        [mutating]
        float randFloat()
        {
            let c = 2.3283064365387e-10;
            return saturate(c * float(randUint()));
        }

        static uint tausStep(uint z, uint S1, uint S2, uint S3, uint M)
        {
            let b = ((z << S1) ^ z) >> S2;
            return ((z & M) << S3) ^ b;
        }

        [ForceInline]
        static uint lcgStep(uint z, uint A, uint C)
        {
            return A * z + C;
        }
    };

    HybridTaus create(uint4 seed)
    {
        return HybridTaus(seed);
    }

    vector<T,N> vec<T, let N: int>(inout IPRNG rng, vector<T,N> vmax)
        where T: __BuiltinArithmeticType
    {
        var v: vector<T,N>;
        [ForceUnroll]
        for (var i = 0; i < N; i++)
        {
            if (let x = (vmax[i] as float))
                v[i] = (rng.rand(x) as T).value;
            else if (let x = (vmax[i] as int))
                v[i] = (rng.rand(x) as T).value;
            else
                v[i] = (rng.rand((vmax[i] as uint).value) as T).value;
        }
        return v;
    }

    vector<T,N> vec<T, let N: int>(inout IPRNG rng, vector<T,N> vmin, vector<T,N> vmax)
        where T: __BuiltinArithmeticType
    {
        var v: vector<T,N>;
        [ForceUnroll]
        for (var i = 0; i < N; i++)
        {
            if (let x = (vmax[i] as float))
                v[i] = (rng.rand((vmin[i] as float).value, x) as T).value;
            else if (let x = (vmax[i] as int))
                v[i] = (rng.rand((vmin[i] as int).value, x) as T).value;
            else
                v[i] = (rng.rand((vmin[i] as uint).value, (vmax[i] as uint).value) as T).value;
        }
        return v;
    }

} //!namespace Rand

//------------------------------------------------------------------------------------------
// - Easing
//------------------------------------------------------------------------------------------

namespace Ease
{

float outBounce(float x)
{
    var t = saturate(x);
    if (t < 1.0 / 2.75)
    {
        return 7.5625 * t * t;
    }
    else if (t < 2.0 / 2.75)
    {
        t -= 1.5 / 2.75;
        return 7.5625 * t * t + 0.75;
    }
    else if (t < 2.5 / 2.75)
    {
        t -= 2.25 / 2.75;
        return 7.5625 * t * t + 0.9375;
    }
    else
    {
        t -= 2.625 / 2.75;
        return 7.5625 * t * t + 0.984375;
    }
}

float inBounce(float x)
{
    return saturate(1.0 - outBounce(1.0 - x));
}

float inOutBounce(float x)
{
    if (x < 0.5)
        return 0.5 * (1.0 - outBounce(1.0 - 2.0 * x));
    else
        return 0.5 * outBounce(2.0 * x - 1.0) + 0.5;
}

} //! namespace Ease

//------------------------------------------------------------------------------------------
// - Colors
//------------------------------------------------------------------------------------------

namespace Color
{

[ForceInline]
float luminance(float3 rgb)
{
    return dot(rgb, float3(0.2126, 0.7152, 0.0722));
}

float4 alphaBlend(float4 src, float4 dst)
{
    var result : float4;
    result.rgb = src.rgb * src.a + dst.rgb * dst.a * (1.0 - src.a);
    result.a = src.a + dst.a * (1.0 - src.a);
    result.rgb = select(abs(result.a) > M_EPSILON, result.rgb / result.a, float3(0));
    return result;
}

// huwb: this modifies the blend alpha based on the brightness of the texture.
// i experimented with a couple of "brightness" measures and went with
float4 contrastBlend(float4 a, float4 b, float alpha)
{
    // brightness
    let Ab = a.x * b.y * a.z;
    let Bb = b.x * b.y * b.z;

    // work out blend param based on relative brightness, smoothstep
    // to increase contrast.
    let contrastAlpha = smoothstep(0., 1., 2. * Ab / (Ab + Bb));

    // push our alpha towards the contrastAlpha, but still follow alpha to 0 and 1.

    // goes to 0 when alpha is near 0 or 1
    var proximityTo01 = min(alpha, 1. - alpha);
    // powering it up seems to give better results
    proximityTo01 = pow(proximityTo01, .25);

    // mix between alpha and contrast-aware alpha
    let f = lerp(alpha, contrastAlpha, proximityTo01);

    // blend
    return lerp(a, b, f);
}

float3 hueShift(float3 rgb, float dhue)
{
    let s = sin(dhue);
    let c = cos(dhue);
    return (rgb * c) + mul(rgb * s, float3x3(float3(0.167444, 0.329213, -0.496657), float3(-0.327948, 0.035669, 0.292279), float3(1.250268, -1.047561, -0.202707))) +
           dot(float3(0.299, 0.587, 0.114), rgb) *
               saturate(1.0 - c);
}

interface IPalette
{
    static float3 rgb(float t);

    static float3 anim()
    {
        return rgb(smoothstep(-1.0, 1.0, cos(Time.elapsed * 10)));
    }
    static float3 blink()
    {
        return rgb(Ease::inOutBounce(cos(Time.elapsed * 20)*0.5+0.5));
    }
};

struct Error : IPalette
{
    static float3 rgb(float t)
    {
        return lerp(float3(1, 0, 0), float3(1, 1, 0), Ease::inOutBounce(t));
    }
};

struct Magma : IPalette
{
    static float3 rgb(float t)
    {
        let x = saturate(t);
        let x1 = float4(1.0, x, x * x, x * x * x); // 1 x x2 x3
        let x2 = x1 * x1.w * x;                    // x4 x5 x6 x7
        return float3(
            dot(x1, float4(-0.027780558, 1.228188385, 0.278906882, 3.892783760)) + dot(x2.xy, float2(-8.490712758, 4.069046086)),
            dot(x1, float4(0.014065206, 0.015360518, 1.605395918, -4.821108251)) + dot(x2.xy, float2(8.389314011, -4.193858954)),
            dot(x1, float4(-0.019628385, 3.122510347, -5.893222355, 2.798380308)) + dot(x2.xy, float2(-3.608884658, 4.324996022)));
    }
};

struct Pastel : IPalette
{
    static float3 rgb(float t)
    {
        // https://www.shadertoy.com/view/4d3SR4
        let h = frac(t + 0.92620819117478) * 6.2831853071796;
        let cocg = 0.25 * float2(cos(h), sin(h));
        let br = float2(-cocg.x, cocg.x) - cocg.y;
        let c = 0.729 + float3(br.y, cocg.y, br.x);
        return c * c;
    }
};

struct Temperature : IPalette
{
    static float3 rgb(float t)
    {
#if 0
        let f = saturate(t);
        var heat : float3;
        heat.r = smoothstep(0.5, 0.8, f);
        if (f >= 0.90)
            heat.r *= (1.1 - f) * 5.0;
        if (f > 0.7)
            heat.g = smoothstep(1.0, 0.7, f);
        else
            heat.g = smoothstep(0.0, 0.7, f);
        heat.b = smoothstep(1.0, 0.0, f);
        if (f <= 0.3)
            heat.b *= f / 0.3;
        return heat;
#else
        var c = 1.0 - pow(abs(float3(t) - float3(0.65, 0.5, 0.2)) * float3(3.0, 3.0, 5.0), float3(1.5, 1.3, 1.7));
        c.r = max((0.15 - sqr(abs(t - 0.04) * 5.0)), c.r);
        c.g = (t < 0.5) ? smoothstep(0.04, 0.45, t) : c.g;
        return saturate(c);
#endif
    }
};

struct BlackBody : IPalette
{
    static const float MAX_KELVIN = 6500.0;

    static float4 color(float kelvin)
    {
        // Physically meaningful temperature range
        let T = clamp(kelvin, 1000.0, 20000.0);

        // Convert T to chromaticity xy using Hernandez-Andres approximation
        // https://doi.org/10.1364/AO.37.001993
        var x : float;
        if (T <= 4000.0)
        {
            x = (-0.2661239e9 / (T * T * T)) - (0.2343580e6 / (T * T)) + (0.8776956e3 / T) + 0.179910;
        }
        else
        {
            x = (-3.0258469e9 / (T * T * T)) + (2.1070379e6 / (T * T)) + (0.2226347e3 / T) + 0.240390;
        }

        let y = -3.000 * x * x + 2.870 * x - 0.275;

        // Convert xyY → XYZ (we use Y = 1.0 nominal luminance before scaling)
        let Y = 1.0;
        let X = select(y > 0.0, x * (Y / y), 0.0);
        let Z = select(y > 0.0, ((1.0 - x - y) * (Y / y)), 0.0);

        // Convert XYZ → linear sRGB
        // Standard sRGB D65 transform
        let R = 3.2406 * X - 1.5372 * Y - 0.4986 * Z;
        let G = -0.9689 * X + 1.8758 * Y + 0.0415 * Z;
        let B = 0.0557 * X - 0.2040 * Y + 1.0570 * Z;

        // Ensure non-negative (light spectra produce no negative energy)
        let rgb = max(float3(R, G, B), float3(0.0));

        // Now compute *relative luminous intensity* using Stefan-Boltzmann:
        // Radiance ∝ T^4
        // We scale by 6500K reference so the result is stable in real scenes.
        let lumen = pow(kelvin / MAX_KELVIN, 4.0);

        return float4(rgb, lumen);
    }

    [ForceInline]
    static float3 rgb(float t)
    {
        return color(t * MAX_KELVIN).rgb;
    }

    static float3 lumen(float kelvin)
    {
        let col = color(kelvin);
        return col.rgb * col.a;
    }
};

float filteredXor(float2 p, float2 dx, float2 dy)
{
    // IQ: https://iquilezles.org/articles/filterableprocedurals/
    var xor = 0.0;
    var lp = p;
    var ldx = dx;
    var ldy = dy;
    for (var i = 0; i < 8; i++)
    {
        let w = max(abs(ldx), abs(ldy)) + 0.01;
        let f = 2.0 * (abs(fract((lp - 0.5 * w) / 2.0) - 0.5) - abs(fract((lp + 0.5 * w) / 2.0) - 0.5)) / w;
        xor += 0.5 - 0.5 * f.x * f.y;

        ldx *= 0.5;
        ldy *= 0.5;
        lp *= 0.5;
        xor *= 0.5;
    }
    return xor;
}

float3 filteredXorGrad(float2 p, float2 dx, float2 dy)
{
    float xor_val = 0.0f;
    float2 grad = float2(0.0f, 0.0f);
    float2 lp = p;
    float2 ldx = dx;
    float2 ldy = dy;
    float current_s = 1.0f;
    for (var i = 0; i < 8; i++)
    {
        float2 w = max(abs(ldx), abs(ldy)) + 0.01f;
        float2 h = 0.5f * w;
        float2 a = fract((lp - h) / 2.0f) - 0.5f;
        float2 b = fract((lp + h) / 2.0f) - 0.5f;
        float2 f = 2.0f * (abs(a) - abs(b)) / w;
        float contrib = 0.5f - 0.5f * f.x * f.y;
        xor_val += contrib;
        float2 dcdlp = -0.5f * float2(
            f.y * (sign(a.x) - sign(b.x)) / w.x,
            f.x * (sign(a.y) - sign(b.y)) / w.y
        );
        float2 dcdp = dcdlp * current_s;
        grad += dcdp;
        ldx *= 0.5f;
        ldy *= 0.5f;
        lp *= 0.5f;
        xor_val *= 0.5f;
        grad *= 0.5f;
        current_s *= 0.5f;
    }
    return float3(xor_val, grad);
}

interface ITonemapper
{
    static float3 from(float3 linear);
};

interface ISpace : ITonemapper
{
    static float3 linear(float3 space);

    static float3 mix(float3 a, float3 b, float t)
    {
        return This.linear(lerp(This.from(a), This.from(b), t));
    }
};

struct Identity : ISpace
{
    [ForceInline]
    static float3 from(float3 linear)
    {
        return linear;
    }

    [ForceInline]
    static float3 linear(float3 space)
    {
        return space;
    }
};

typealias Linear = Identity;

struct sRGB : ISpace
{
    // Taken from D3DX_DXGIFormatConvert.inl
    static float3 from(float3 linear)
    {
        return select(linear < 0.0031308,
                      linear * 12.92,
                      1.055 * pow(max(linear, 0.0), 1.0 / 2.4) - 0.055);
    }

    // Taken from D3DX_DXGIFormatConvert.inl
    // Technically this is not bit-exact - that requires a look-up table,
    // but it's accurate enough for our purposes here.
    static float3 linear(float3 sRGB)
    {
        return select(sRGB < 0.04045,
                      sRGB / 12.92,
                      pow(max((sRGB + 0.055) / 1.055, 0.0), 2.4));
    }
};

struct Log : ISpace
{
    [ForceInline]
    static float3 from(float3 linear)
    {
        return log(1.0 + linear);
    }

    [ForceInline]
    static float3 linear(float3 space)
    {
        return exp(space) - 1.0;
    }

    [ForceInline]
    static vector<float, N> from<let N: uint>(vector<float, N> linear)
    {
        return log(1.0 + linear);
    }

    [ForceInline]
    static vector<float, N> linear<let N: uint>(vector<float, N> space)
    {
        return exp(space) - 1.0;
    }
};

struct YCoCg : ISpace
{
    static float3 from(float3 linear)
    {
        let Y = dot(linear, float3(1, 2, 1)) * 0.25;
        let Co = dot(linear, float3(2, 0, -2)) * 0.25 + (0.5 * 256.0 / 255.0);
        let Cg = dot(linear, float3(-1, 2, -1)) * 0.25 + (0.5 * 256.0 / 255.0);
        return float3(Y, Co, Cg);
    }

    static float3 linear(float3 ycocg)
    {
        let Y = ycocg.x;
        let Co = ycocg.y - (0.5 * 256.0 / 255.0);
        let Cg = ycocg.z - (0.5 * 256.0 / 255.0);
        let R = Y + Co - Cg;
        let G = Y + Cg;
        let B = Y - Co - Cg;
        return float3(R, G, B);
    }
};

struct OKLab : ISpace
{
    static float3 from(float3 linear)
    {
        let l = 0.4122214708 * linear.r + 0.5363325363 * linear.g + 0.0514459929 * linear.b;
        let m = 0.2119034982 * linear.r + 0.6806995451 * linear.g + 0.1073969566 * linear.b;
        let s = 0.0883024619 * linear.r + 0.2817188376 * linear.g + 0.6299787005 * linear.b;

        let l_ = pow(max(l, 0.0), 1.0 / 3.0);
        let m_ = pow(max(m, 0.0), 1.0 / 3.0);
        let s_ = pow(max(s, 0.0), 1.0 / 3.0);

        return float3(
            0.2104542553 * l_ + 0.7936177850 * m_ - 0.0040720468 * s_,
            1.9779984951 * l_ - 2.4285922050 * m_ + 0.4505937099 * s_,
            0.0259040371 * l_ + 0.7827717662 * m_ - 0.8086757660 * s_);
    }

    static float3 linear(float3 oklab)
    {
        let l_ = oklab.x + 0.3963377774 * oklab.y + 0.2158037573 * oklab.z;
        let m_ = oklab.x - 0.1055613458 * oklab.y - 0.0638541728 * oklab.z;
        let s_ = oklab.x - 0.0894841775 * oklab.y - 1.2914855480 * oklab.z;

        let l = l_ * l_ * l_;
        let m = m_ * m_ * m_;
        let s = s_ * s_ * s_;

        return float3(
            4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
            -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
            -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s);
    }
};

enum AGXLook
{
    Default,
    Golden,
    Punchy,
};

struct AGX
{
    enum Look
    {
        Default,
        Golden,
        Punchy,
    };

    static float3 from(float3 linear, Look look)
    {
        var col = mul(float3x3(.842, .0423, .0424, .0784, .878, .0784, .0792, .0792, .879), linear);
        // Log2 space encoding
        col = saturate((log2(col) + 12.47393) / 16.5);
        // Apply sigmoid function approximation
        col = .5 + .5 * sin(((-3.11 * col + 6.42) * col - .378) * col - 1.44);
        // AgX look (optional)
        switch (look)
        {
        case Look.Golden:
            col = lerp(float3(dot(col, float3(.216, .7152, .0722))), col * float3(1., .9, .5), .8);
        case Look.Punchy:
            col = lerp(float3(dot(col, float3(.216, .7152, .0722))), pow(max(col, float3(0)), float3(1.35)), 1.4);
        default:
        }
        // Eotf
        col = mul(float3x3(1.2, -.053, -.053, -.1, 1.15, -.1, -.1, -.1, 1.15), col);
        // <-
        return col;
    }

    struct Default : ITonemapper
    {
        [ForceInline]
        static float3 from(float3 linear)
        {
            return AGX.from(linear, Look.Default);
        }
    };

    struct Golden : ITonemapper
    {
        [ForceInline]
        static float3 from(float3 linear)
        {
            return AGX.from(linear, Look.Golden);
        }
    };

    struct Punchy : ITonemapper
    {
        [ForceInline]
        static float3 from(float3 linear)
        {
            return AGX.from(linear, Look.Punchy);
        }
    };
};

struct SimpleReinhard : ISpace
{
    [ForceInline]
    static float3 from(float3 linear)
    {
        return saturate(linear / (linear + 1.0f));
    }

    [ForceInline]
    static float3 linear(float3 space)
    {
        return space / max((1.0f - space), M_EPSILON);
    }
};

float3 convert<DST : ISpace, SRC : ISpace>(float col)
{
    let linear = SRC.linear(col);
    return DST.from(linear);
}

// ============================================================================
// Encode/Decode RGB9E5 (E5B9G9R9_UFLOAT) color format
// ============================================================================
// Each color component (R,G,B) has 9-bit mantissa, and all share a 5-bit exponent
// Layout (least significant bit first):
// [ 9 bits Blue | 9 bits Green | 9 bits Red | 5 bits SharedExponent ]
// Reference: https://registry.khronos.org/OpenGL/extensions/EXT/EXT_texture_shared_exponent.txt
// ============================================================================

struct RGB9E5
{
    static const uint EXP_BIAS = 15u;
    static const uint MANTISSA_BITS = 9u;
    static const uint EXP_BITS = 5u;
    static const uint MAX_VALID_BIASED_EXP = 31u;
    static const uint MANTISSA_VALUES = 512u;

    // Maximum representable value: (511/512) * 2^16 = 65408
    static const float MAX_VALUE = 65408.0;

    // Minimum positive normalized value: 1/512 * 2^-15 ≈ 6e-8
    static const float MIN_NORMALIZED = 6.103515625e-8;

    uint32_t packed;

    [ForceInline]
    __init(uint32_t packed)
    {
        this.packed = packed;
    }

    [ForceInline]
    __init(float3 linear)
    {
        this.packed = pack(linear.r, linear.g, linear.b);
    }

    [ForceInline]
    __init(float r, float g, float b)
    {
        this.packed = pack(r, g, b);
    }

    property bool valid
    {
        [ForceInline]
        get
        {
            // Check that reserved bits are zero (if any)
            // For E5B9G9R9, all 32 bits are used, so this always returns true
            // But we can check for obviously invalid values
            let exp = (this.packed >> 27u) & 0x1Fu;
            return exp <= MAX_VALID_BIASED_EXP;
        }
    }

    property bool is_zero
    {
        [ForceInline]
        get { return this.packed == 0u; }
    }

    // Encode RGB color into E5B9G9R9_UFLOAT (returns packed u32)
    // Follows Khronos OpenGL EXT_texture_shared_exponent specification
    static uint32_t pack(float r, float g, float b)
    {
        // Clamp inputs to valid range [0, MAX_VALUE]
        // Use saturate first to handle NaN/Inf robustly
        let col = clamp(float3(r, g, b), 0.0, MAX_VALUE);

        // Handle NaN/Inf explicitly - saturate to max representable
        let col_safe = select(isfinite(col), col, float3(MAX_VALUE));

        // Find max channel
        let maxrgb = max(col_safe.r, max(col_safe.g, col_safe.b));

        // Early exit for zero/near-zero values
        if (maxrgb < MIN_NORMALIZED)
        {
            return 0u;
        }

        // Compute shared exponent using safe log2
        // log2(maxrgb) gives us the exponent we need
        let log2_max = log2(max(maxrgb, MIN_NORMALIZED));

        // Floor to get integer exponent, then add bias
        // The +1 accounts for the implicit leading bit in mantissa
        let exp_shared_unbounded = int(floor(log2_max)) + 1 + int(EXP_BIAS);

        // Clamp to valid biased exponent range [0, 31]
        let exp_clamped = uint(clamp(exp_shared_unbounded, 0, int(MAX_VALID_BIASED_EXP)));

        // Compute scale factor for mantissa quantization
        // This is 2^(exp - bias - mantissa_bits)
        let exponent_value = int(exp_clamped) - int(EXP_BIAS) - int(MANTISSA_BITS);

        // Use ldexp for robust power-of-2 computation (avoids overflow/underflow in exp2)
        let scale = ldexp(1.0, exponent_value);

        // Prevent division by zero or denormal
        let scale_safe = max(scale, MIN_NORMALIZED * 0.001);

        // Quantize each channel to 9-bit mantissa with rounding
        // The 0.5 provides round-to-nearest
        let scaled = col_safe / scale_safe;
        let qm_unclamped = uint32_t3(scaled + 0.5);

        // Clamp mantissas to [0, 511] range
        let qm = min(qm_unclamped, 511u);

        // Handle potential overflow: if all mantissas hit 511, we should increment exponent
        // This can happen due to rounding
        if (all(qm == 511u) && exp_clamped < MAX_VALID_BIASED_EXP)
        {
            // Increment exponent and halve mantissas
            let exp_incremented = exp_clamped + 1u;
            let qm_halved = qm >> 1u; // Divide by 2
            return (exp_incremented << 27u) | (qm_halved.b << 18u) | (qm_halved.g << 9u) | qm_halved.r;
        }

        // Pack into 32-bit integer: [5 bits exp][9 bits B][9 bits G][9 bits R]
        return (exp_clamped << 27u) | (qm.b << 18u) | (qm.g << 9u) | qm.r;
    }

    // Decode E5B9G9R9_UFLOAT back into linear RGB color
    // Follows Khronos OpenGL EXT_texture_shared_exponent specification
    float3 unpack()
    {
        // Early exit for zero
        if (this.packed == 0u)
            return float3(0);

        // Extract components with proper masking
        let exp = (this.packed >> 27u) & 0x1Fu;
        let rm = (this.packed >> 0u)  & 0x1FFu;
        let gm = (this.packed >> 9u)  & 0x1FFu;
        let bm = (this.packed >> 18u) & 0x1FFu;

        // Compute exponent value
        let exponent_value = int(exp) - int(EXP_BIAS) - int(MANTISSA_BITS);

        // Use ldexp for robust power-of-2 computation
        let scale = ldexp(1.0, exponent_value);

        // Convert mantissas to float and scale
        return float3(float(rm), float(gm), float(bm)) * scale;
    }
};

} //! namespace Color

//------------------------------------------------------------------------------------------
// - Manual sampling helpers
//------------------------------------------------------------------------------------------

namespace Sampling
{

struct Sample<let N: uint>
{
    vector<int, N> index;
    float weight;
};

typealias Sample1D = Sample<1>;
typealias Sample2D = Sample<2>;
typealias Sample3D = Sample<3>;

interface ISampler1D : IContainer<Sample1D>
{
    __init(float u);
};
interface ISampler2D : IContainer<Sample2D>
{
    __init(float2 uv);
};
interface ISampler3D : IContainer<Sample3D>
{
    __init(float3 uvw);
};

struct Linear : ISampler1D
{
    int base_index;
    float ratio;

    [ForceInline]
    __init(float u)
    {
        this.base_index = int(floor(u));
        this.ratio = fract(u);
    }

    property float2 weights
    {
        [ForceInline]
        get
        {
            return float2(ratio, 1.0 - ratio);
        }
    }

    typealias Iterator = IndexIterator;

    property uint count
    {
        [ForceInline]
        get { return 2u; }
    }

    [ForceInline]
    Iterator begin() { return Iterator(0u); }
    [ForceInline]
    Iterator end() { return Iterator(2u); }
    [ForceInline]
    Sample1D at(Iterator iter)
    {
        return Sample1D(this.base_index + int(iter.index), this.weights[iter.index]);
    }

};

struct Bilinear : ISampler2D
{
    int2 base_index;
    float2 ratio;

    [ForceInline]
    __init(float2 uv)
    {
        this.base_index = int2(floor(uv));
        this.ratio = fract(uv);
    }

    property float2 uv
    {
        [ForceInline]
        get
        {
            return float2(this.base_index) + this.ratio;
        }
    }

    property int2 nearest
    {
        [ForceInline]
        get
        {
            return int2(round(this.base_index + this.ratio));
        }
    }

    property Bilinear hermite
    {
        [ForceInline]
        get
        {
            var result = this;
            result.ratio = smoothHermite(result.ratio);
            return result;
        }
    }

    property Bicubic bicubic
    {
        [ForceInline]
        get
        {
            return Bicubic(this.uv);
        }
    }

    property float4 weights
    {
        [ForceInline]
        get
        {
            return float4((1.0 - ratio.x) * (1.0 - ratio.y),
                          ratio.x * (1.0 - ratio.y),
                          (1.0 - ratio.x) * ratio.y,
                          ratio.x * ratio.y);
        }
    }

    [ForceInline]
    int2 offset(uint index)
    {
        return int2(index & 1, int(index > 1));
    }

    typealias Iterator = IndexIterator;

    property uint count
    {
        [ForceInline]
        get { return 4u; }
    }

    [ForceInline]
    Iterator begin() { return Iterator(0u); }
    [ForceInline]
    Iterator end() { return Iterator(4u); }
    [ForceInline]
    Sample2D at(Iterator iter)
    {
        return Sample2D(this.base_index + this.offset(iter.index), this.weights[iter.index]);
    }

};

struct Trilinear : ISampler3D
{
    int3 base_index;
    float3 ratio;

    [ForceInline]
    __init(float3 uvw)
    {
        this.base_index = int3(floor(uvw));
        this.ratio = fract(uvw);
    }

    property float3 uvw
    {
        [ForceInline]
        get
        {
            return float3(this.base_index) + this.ratio;
        }
    }

    property int3 nearest
    {
        [ForceInline]
        get
        {
            return int3(round(float3(this.base_index) + this.ratio));
        }
    }

    // Helper to calculate the trilinear weight for a specific corner index (0-7)
    [ForceInline]
    float weight(uint index)
    {
        // bit 0: x, bit 1: y, bit 2: z
        float3 w = select((int3(index) & int3(1, 2, 4)) != 0, this.ratio, 1.0 - this.ratio);
        return w.x * w.y * w.z;
    }

    [ForceInline]
    int3 offset(uint index)
    {
        // Extract bits to form the corner offsets
        return int3(index & 1, (index >> 1) & 1, (index >> 2) & 1);
    }

    typealias Iterator = IndexIterator;

    property uint count
    {
        [ForceInline]
        get { return 8u; }
    }

    [ForceInline]
    Iterator begin() { return Iterator(0u); }
    [ForceInline]
    Iterator end() { return Iterator(8u); }

    [ForceInline]
    Sample3D at(Iterator iter)
    {
        return Sample3D(this.base_index + this.offset(iter.index), this.weight(iter.index));
    }
};

struct Cubic : ISampler1D
{
    int base_index;
    float ratio;

    [ForceInline]
    __init(float u)
    {
        this.ratio = fract(u);
        this.base_index = int(floor(u)) - 1;
    }

    // Standard Catmull-Rom (a=-0.5)
    property float4 catmull_rom
    {
        [ForceInline]
        get
        {
            let t = this.ratio;
            let t2 = t * t;
            let t3 = t2 * t;

            return float4(
                -0.5 * t3 + t2 - 0.5 * t,
                1.5 * t3 - 2.5 * t2 + 1.0,
                -1.5 * t3 + 2.0 * t2 + 0.5 * t,
                0.5 * t3 - 0.5 * t2
            );
        }
    }

    // Derived from standard separable kernel k(x)
    property float4 mitchell_netravali
    {
        [ForceInline]
        get
        {
            let t = this.ratio;
            let t2 = t * t;
            let t3 = t2 * t;

            // p0: x = 1 + t (Region 2)
            // p1: x = t     (Region 1)
            // p2: x = 1 - t (Region 1)
            // p3: x = 2 - t (Region 2)

            // Weights derived for B=1/3, C=1/3
            return float4(
                (1.0/18.0) * (-7.0*t3 + 12.0*t2 - 5.0*t),
                (1.0/18.0) * (21.0*t3 - 36.0*t2 + 16.0),
                (1.0/18.0) * (-21.0*t3 + 27.0*t2 + 9.0*t + 1.0),
                (1.0/18.0) * (7.0*t3 - 3.0*t2 - 4.0*t)
            );
        }
    }

    // FIXED: B-Spline
    property float4 b_spline
    {
        [ForceInline]
        get
        {
            let t = this.ratio;
            let t2 = t * t;
            let t3 = t2 * t;

            return float4(
                (1.0/6.0) * (1.0 - 3.0*t + 3.0*t2 - t3),
                (1.0/6.0) * (3.0*t3 - 6.0*t2 + 4.0),
                (1.0/6.0) * (-3.0*t3 + 3.0*t2 + 3.0*t + 1.0),
                (1.0/6.0) * t3
            );
        }
    }

    // Default to a safe filter (B-Spline is smoothest, MN is sharper)
    property float4 weights
    {
        [ForceInline]
        get
        {
            return this.mitchell_netravali;
        }
    }

    typealias Iterator = IndexIterator;

    property uint count
    {
        [ForceInline]
        get { return 4u; }
    }

    [ForceInline]
    Iterator begin() { return Iterator(0u); }
    [ForceInline]
    Iterator end() { return Iterator(4u); }
    [ForceInline]
    Sample1D at(Iterator iter)
    {
        return Sample1D(this.base_index + int(iter.index), this.weights[iter.index]);
    }
};

struct Bicubic : ISampler2D
{
    Cubic x;
    Cubic y;

    [ForceInline]
    __init(float2 uv)
    {
        this.x = Cubic(uv.x);
        this.y = Cubic(uv.y);
    }

    property int2 base_index
    {
        [ForceInline]
        get
        {
            return int2(x.base_index, y.base_index);
        }
    }

    [ForceInline]
    int2 offset(uint index)
    {
        return int2(index & 3u, index >> 2u);
    }

    typealias Iterator = IndexIterator;

    property uint count
    {
        [ForceInline]
        get { return 16u; }
    }

    [ForceInline]
    Iterator begin() { return Iterator(0u); }
    [ForceInline]
    Iterator end() { return Iterator(16u); }
    [ForceInline]
    Sample2D at(Iterator iter)
    {
        let off = this.offset(iter.index);
        return Sample2D(this.base_index + off, this.x.weights[off.x]*this.y.weights[off.y]);
    }

};

} //! namespace Sampling

//------------------------------------------------------------------------------------------
// - Signed distance functions
//------------------------------------------------------------------------------------------

// https://iquilezles.org/articles/distfunctions/
// https://iquilezles.org/articles/distfunctions2d/

namespace SDF
{

float sdCircle(float2 p, float r)
{
    return length(p) - r;
}

float sdRoundedBox(in float2 p, in float2 b, in float4 r)
{
    r.xy = (p.x > 0.0) ? r.xy : r.zw;
    r.x = (p.y > 0.0) ? r.x : r.y;
    float2 q = abs(p) - b + r.x;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r.x;
}

float sdChamferBox(in float2 p, in float2 b, in float chamfer)
{
    p = abs(p) - b;

    p = (p.y > p.x) ? p.yx : p.xy;
    p.y += chamfer;

    const float k = 1.0 - sqrt(2.0);
    if (p.y < 0.0 && p.y + p.x * k < 0.0)
        return p.x;

    if (p.x < p.y)
        return (p.x + p.y) * sqrt(0.5);

    return length(p);
}

float sdBox(in float2 p, in float2 b)
{
    float2 d = abs(p) - b;
    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0);
}

float sdSegment(in float2 p, in float2 a, in float2 b)
{
    float2 pa = p - a, ba = b - a;
    float h = saturate(dot(pa, ba) / dot(ba, ba));
    return length(pa - ba * h);
}

float sdRhombus(in float2 p, in float2 b)
{
    b.y = -b.y;
    p = abs(p);
    float h = saturate((dot(b, p) + b.y * b.y) / dot(b, b));
    p -= b * float2(h, h - 1.0);
    return length(p) * sign(p.x);
}

float sdEquilateralTriangle(in float2 p, in float r)
{
    const float k = sqrt(3.0);
    p.x = abs(p.x) - r;
    p.y = p.y + r / k;
    if (p.x + k * p.y > 0.0)
        p = float2(p.x - k * p.y, -k * p.x - p.y) / 2.0;
    p.x -= clamp(p.x, -2.0 * r, 0.0);
    return -length(p) * sign(p.y);
}

float sdCross(in float2 p, in float2 b, float r)
{
    p = abs(p);
    p = (p.y > p.x) ? p.yx : p.xy;
    float2 q = p - b;
    float k = max(q.y, q.x);
    float2 w = (k > 0.0) ? q : float2(b.y - p.x, -k);
    return sign(k) * length(max(w, 0.0)) + r;
}

float sdRoundedX(in float2 p, in float w, in float r)
{
    p = abs(p);
    return length(p - min(p.x + p.y, w) * 0.5) - r;
}

float opUnion(float d1, float d2)
{
    return min(d1, d2);
}
float opSubtraction(float d1, float d2)
{
    return max(-d1, d2);
}
float opIntersection(float d1, float d2)
{
    return max(d1, d2);
}
float opXor(float d1, float d2)
{
    return max(min(d1, d2), -max(d1, d2));
}

float opSmoothUnion(float d1, float d2, float k)
{
    k *= 4.0;
    float h = max(k - abs(d1 - d2), 0.0);
    return min(d1, d2) - h * h * 0.25 / k;
}

float opSmoothSubtraction(float d1, float d2, float k)
{
    return -opSmoothUnion(d1, -d2, k);

    // k *= 4.0;
    //  float h = max(k-abs(-d1-d2),0.0);
    //  return max(-d1, d2) + h*h*0.25/k;
}

float opSmoothIntersection(float d1, float d2, float k)
{
    return -opSmoothUnion(-d1, -d2, k);

    // k *= 4.0;
    //  float h = max(k-abs(d1-d2),0.0);
    //  return max(d1, d2) + h*h*0.25/k;
}

}; //! namespace SDF

//------------------------------------------------------------------------------------------
// - 2d canvas drawing
//------------------------------------------------------------------------------------------

namespace Canvas
{
struct Style
{
    float4 fill = float4(1.0, 1.0, 1.0, 0.5);
    float4 glow = float4(0.0, 0.0, 0.0, 0.2);
    float4 stroke = float4(0.0, 1.0, 1.0, 0.8);

    float glow_width = 5.0;
    float stroke_offset = 1.0;
    float stroke_width = 1.0;
    float feather = 0.5;
};

struct Context
{
    float2 origin = float2(0.0);
    float4 blit = float4(0.0);
    Style style = Style();

    float4 output(float3 background, float alpha)
    {
        return Color.alphaBlend(float4(this.blit.rgb, this.blit.a * alpha), float4(background, 1.0));
    }

    [mutating]
    void clear()
    {
        this.blit = float4(0.0);
        this.style = Style();
    }

    [mutating]
    float4 flush(float3 background, float alpha)
    {
        let result = this.output(background, alpha);
        this.clear();
        return result;
    }

    [mutating]
    float2 moveTo(float2 position)
    {
        let prev = this.origin;
        this.origin = position;
        return prev;
    }

    [ForceInline]
    [mutating]
    void drawColor(float4 color)
    {
        this.blit = Color.alphaBlend(color, this.blit);
    }

    [mutating]
    void drawSdf(float d, float ddxy_len)
    {
        // 1. Ensure we have a valid pixel scale.
        // If ddxy_len is 0 (e.g. constant value), AA is impossible.
        float unit = max(ddxy_len, 0.0001);

        // 2. Map distances to pixel space
        // This tells us how many pixels we are away from the edge.
        float p_dist = d / unit;

        // 3. Define AA width.
        // Standard AA is 1.0 pixels wide. Adding 'feather' allows for soft edges.
        float aa = 1.0 + this.style.feather;
        float half_aa = aa * 0.5;

        // --- Fill Calculation ---
        // Transitions from 1 (inside) to 0 (outside) over the edge
        float fill_alpha = smoothstep(half_aa, -half_aa, p_dist);

        // --- Stroke Calculation ---
        // Distance from the center of the stroke line in pixels
        float dist_to_stroke_center = abs(p_dist - this.style.stroke_offset);

        // Stroke alpha: 1 inside the width, 0 outside, with AA on both sides
        float stroke_alpha = smoothstep(this.style.stroke_width * 0.5 + half_aa,
                                        this.style.stroke_width * 0.5 - half_aa,
                                        dist_to_stroke_center);

        // Only draw stroke if width > 0
        stroke_alpha *= step(0.0001, this.style.stroke_width);

        // --- Glow Calculation ---
        // Glow is typically a much wider, softer transition
        float glow_alpha = smoothstep(this.style.glow_width, 0.0, dist_to_stroke_center);
        glow_alpha *= step(0.0001, this.style.glow_width);

        // --- Composite ---
        // Order matters: Glow (back) -> Fill -> Stroke (front)
        // We use drawColor which performs alpha blending
        if (this.style.glow.a > 0.0)
            this.drawColor(float4(this.style.glow.rgb, this.style.glow.a * glow_alpha));

        if (this.style.fill.a > 0.0)
            this.drawColor(float4(this.style.fill.rgb, this.style.fill.a * fill_alpha));

        if (this.style.stroke.a > 0.0)
            this.drawColor(float4(this.style.stroke.rgb, this.style.stroke.a * stroke_alpha));
    }

    [mutating]
    void drawCircle(float2 center, float radius, float ddxy_len)
    {
        this.drawSdf(SDF.sdCircle(center - this.origin, radius), ddxy_len);
    }

    [mutating]
    void drawSegment(float2 a, float2 b, float ddxy_len)
    {
        this.drawSdf(SDF.sdSegment(this.origin, a, b), ddxy_len);
    }

    [mutating]
    void drawSegment(float2 a, float2 b, float radius, float ddxy_len)
    {
        this.drawSdf(SDF.sdSegment(this.origin, a, b) - radius, ddxy_len);
    }

    [mutating]
    void drawBox(float2 center, float2 extents, float ddxy_len)
    {
        this.drawSdf(SDF.sdBox(center - this.origin, extents), ddxy_len);
    }

    [mutating]
    void drawEquilateralTriangle(float2 center, float radius, float ddxy_len)
    {
        this.drawSdf(SDF.sdEquilateralTriangle(center - this.origin, radius), ddxy_len);
    }

    [mutating]
    void drawRoundedX(float2 center, float width, float radius, float ddxy_len)
    {
        this.drawSdf(SDF.sdRoundedX(center - this.origin, width, radius), ddxy_len);
    }

    [mutating]
    void drawArrow(float2 start, float2 end, float radius, float ddxy_len)
    {
        let axis = normalizeSafe(end - start, float2(1, 0));
        let rot = float2x2(float2(-axis.y, axis.x), axis);
        let r = ((this.style.stroke_width * ddxy_len) + radius) * 2.0;
        let tip = end - axis * r;
        var d = SDF.sdSegment(this.origin, start, tip) - radius;
        d = min(d, SDF.sdEquilateralTriangle(mul(this.origin - tip, rot), r));
        this.drawSdf(d, ddxy_len);
    }
};

struct Transform
{
    static const float DDXY_LEN = 1.0;

    Context ctx = {};
    Camera::Model cam;

    __init(Camera::Model cam, float3 origin)
    {
        this.cam = cam;
        this.ctx.moveTo(this.project(origin));
    }

    property Style style
    {
        [ForceInline]
        get
        {
            return this.ctx.style;
        }
        [ForceInline]
        set
        {
            this.ctx.style = newValue;
        }
    }

    float2 project(float3 world_pos)
    {
        return this.cam.worldToScreen(world_pos).xy;
    }

    float4 output(float3 background, float alpha)
    {
        return this.ctx.output(background, alpha);
    }

    [mutating]
    void clear()
    {
        this.ctx.clear();
    }

    [mutating]
    float4 flush(float3 background, float alpha)
    {
        return this.ctx.flush(background, alpha);
    }

    [mutating]
    void moveTo(float3 world_pos)
    {
        this.ctx.moveTo(this.project(world_pos));
    }

    [mutating]
    void drawCircle(float3 world_pos, float world_radius)
    {
        let a = this.project(world_pos);
        let b = this.project(world_pos + this.cam.inv_view[0].xyz * world_radius);
        this.ctx.drawCircle(a, distance(a, b), DDXY_LEN);
    }

    [mutating]
    void drawSegment(float3 a, float3 b, float screen_radius = 0)
    {
        this.ctx.drawSegment(this.project(a), this.project(b), screen_radius, DDXY_LEN);
    }

    [mutating]
    void drawBox(float3 world_pos, float3 world_extents)
    {
        let a = this.project(world_pos - world_extents * float3(1, 1, 0));
        let b = this.project(world_pos + world_extents * float3(1, 1, 0));
        this.ctx.drawBox((a + b) * 0.5, abs(b - a) * 0.5, DDXY_LEN);
    }

    [mutating]
    void drawEquilateralTriangle(float3 world_pos, float world_radius)
    {
        let a = this.project(world_pos);
        let b = this.project(world_pos + this.cam.inv_view[0].xyz * world_radius);
        this.ctx.drawEquilateralTriangle(a, distance(a, b), DDXY_LEN);
    }

    [mutating]
    void drawArrow(float3 world_start, float3 world_end, float screen_radius)
    {
        let a = this.project(world_start);
        let b = this.project(world_end);
        this.ctx.drawArrow(a, b, screen_radius, DDXY_LEN);
    }
};

}; //! namespace Canvas

//------------------------------------------------------------------------------------------
// - Cook-Torrance BRDF
//------------------------------------------------------------------------------------------

namespace BxDF
{
    // Normal mapping will lead to an impossible surface where the view ray and normal dot product
    // is negative. Using PBR, this leads to negative radiance and black artefacts at detail
    // fringes. See "Microfacet-based Normal Mapping for Robust Monte Carlo Path Tracing" by
    // Schüssler et al. for a discussion of a physically correct solution.
    // We just clamp the dot product with a normal to some small value.
    static const float MIN_DOT = 1e-5;

    [ForceInline]
    float dot_c<let N : int>(vector<float, N> a, vector<float, N> b)
    {
        return max(dot(a, b), MIN_DOT);
    }

    [ForceInline]
    float pow_c_5(float x)
    {
        x = max(x, 0.0);
        let x2 = x * x;
        return x2 * x2 * x;
    }

    // -------------------------------------------------------------------------
    // INTEGRATION
    // -------------------------------------------------------------------------
    // Returns the halfway vector H sampled from the VNDF
    float3 importanceSampleVNDFAnisotropic(float2 Xi, float ax, float ay, float3 V)
    {
        // 1. Transform the view vector to the hemisphere configuration (stretch space)
        float3 V_stretched = normalize(float3(ax * V.x, ay * V.y, V.z));

        // 2. Build an orthonormal basis around the stretched view vector
        // Using a stable branchless method
        float3 T1 = (V_stretched.z < 0.999) ? normalize(cross(V_stretched, float3(0, 0, 1))) : float3(1, 0, 0);
        float3 T2 = cross(T1, V_stretched);

        // 3. Sample a point on the unit disk
        float r = sqrt(max(Xi.x, 0.0));
        float phi = M_TAU * Xi.y;
        float t1 = r * cos(phi);
        float t2 = r * sin(phi);

        // Project point onto the hemisphere
        float s = 0.5 * (1.0 + V_stretched.z);
        t2 = (1.0 - s) * sqrt(max(1.0 - t1 * t1, 0.0)) + s * t2;

        // 4. Compute the normal in the stretched space (NH)
        float3 NH = t1 * T1 + t2 * T2 + sqrt(max(0.0, 1.0 - t1 * t1 - t2 * t2)) * V_stretched;

        // 5. Transform back to the original anisotropic space
        return normalize(float3(ax * NH.x, ay * NH.y, max(0.0, NH.z)));
    }

    // Returns float2(Fc_Weighted_Vis, Unweighted_Vis)
    // This format matches your 'shade' function's reconstruction:
    // lerp(env.x, env.y, F0)
    //   = env.x * (1 - F0) + env.y * F0
    //   = (Integral_Fc_Vis) * (1 - F0) + (Integral_Vis) * F0
    float2 integrateBRDFAnisotropic(float NdotV, float roughness, float anisotropy, constexpr int sampleCount)
    {
        NdotV = max(NdotV, MIN_DOT);

        let at = max(roughness * (1.0 + anisotropy), 0.001);
        let ab = max(roughness * (1.0 - anisotropy), 0.001);

        // Tangent space View vector (phi = 0)
        let V = float3(sqrt(max(1.0 - NdotV * NdotV, 0.0)), 0.0, NdotV);

        var accFcVis = 0.0;
        var accVis   = 0.0;

        for(int i = 0; i < sampleCount; i++)
        {
            let Xi = hammersley(i, sampleCount);

            // --- NEW: VNDF SAMPLING ---
            let H = importanceSampleVNDFAnisotropic(Xi, at, ab, V);
            let L = normalizeSafe(reflect(-V, H), H);

            let NdotL = L.z; // Normal is (0,0,1)
            let VdotH = saturate(dot(V, H));

            if (NdotL > 0.0)
            {
                // When sampling VNDF, the weight is simplified to:
                // Weight = G2(V, L) / G1(V)
                // Where G2 is the correlated Smith term.

                let ToV = V.x; let BoV = V.y;
                let ToL = L.x; let BoL = L.y;

                // Note: smithsAnisotropic returns Vis = G2 / (4 * NoV * NoL)
                let Vis = smithsAnisotropic(at, ab, ToV, BoV, ToL, BoL, NdotV, NdotL);

                // G1 for the view vector
                let lambdaV = NdotL * length(float3(at * ToV, ab * BoV, NdotV));
                let lambdaL = NdotV * length(float3(at * ToL, ab * BoL, NdotL));
                let G2 = select(lambdaV + lambdaL > M_EPSILON, 0.5 / (lambdaV + lambdaL), 0.0); // This is actually the Vis term

                // Simplified weight for VNDF: (4 * NoV * NoL * Vis) / G1(V)
                // But usually, we just calculate the ratio of G terms:
                let weight = (4.0 * NdotL * Vis * NdotV) / Smith_G1(NdotV, at, ab, ToV, BoV);

                let Fc = pow_c_5(1.0 - VdotH);
                accFcVis += Fc * weight;
                accVis   += weight;
            }
        }

        return float2(accFcVis, accVis) / float(sampleCount);
    }

    // Helper for the weight calculation
    [ForceInline]
    float Smith_G1(float NoV, float at, float ab, float ToV, float BoV) {
        let a2 = length(float3(at * ToV, ab * BoV, NoV));
        return select(NoV + a2 > M_EPSILON, 2.0 * NoV / (NoV + a2), 0.0);
    }

    // -------------------------------------------------------------------------
    // RUNTIME EVALUATION
    // -------------------------------------------------------------------------
    [ForceInline]
    float3 diffuseLambert(float3 albedo)
    {
        return albedo / M_PI;
    }

    float3 diffuseDisney(float3 albedo, float NdotL, float NdotV, float LdotH, float roughness)
    {
        // From Disney's 2012 BRDF
        let FL = pow_c_5(1.0 - NdotL);
        let FV = pow_c_5(1.0 - NdotV);
        let Fd90 = 0.5 + 2.0 * LdotH * LdotH * roughness;
        let Fd = lerp(1.0, Fd90, FL) * lerp(1.0, Fd90, FV);
        return albedo * Fd / M_PI;
    }

    // Trowbridge-Reitz AKA GGX
    float distribution(float3 n, float3 h, float roughness)
    {
        let a_2 = roughness * roughness;
        let denom = M_PI * sqr(sqr(dot_c(n, h)) * (a_2 - 1.0) + 1.0);
        return select(denom > M_EPSILON, a_2 / denom, 0.0);
    }

    // Schlick-Beckmann
    [ForceInline]
    float geometry(float cosTheta, float k)
    {
        let denom = cosTheta * (1.0 - k) + k;
        return select(denom > M_EPSILON, (cosTheta) / denom, 0.0);
    }

    float smiths(float NdotV, float NdotL, float roughness)
    {
        let k = sqr(roughness + 1.0) / 8.0;
        return max(geometry(NdotV, k) * geometry(NdotL, k), 0.0);
    }

    // Anisotropic distribution and visibility functions from Filament
    // GGX
    float distributionAnisotropic(float NoH, float3 h, float3 t, float3 b, float at, float ab)
    {
        let ToH = dot(t, h);
        let BoH = dot(b, h);
        let a2 = at * ab;
        let v = float3(ab * ToH, at * BoH, a2 * NoH);
        let v2 = dot(v, v);
        let w2 = a2 / v2;
        return select(v2 > M_EPSILON, a2 * w2 * w2 * (1.0 / M_PI), 0.0);
    }

    // Returns the Visibility Term (Vis) = G2 / (4 * NoV * NoL)
    // Implicitly handles World-to-Tangent transform via dot products.
    float smithsAnisotropic(float at, float ab, float ToV, float BoV,
                            float ToL, float BoL, float NoV, float NoL)
    {
        // Heitz 2014 variant (High Quality)
        let lambdaV = NoL * length(float3(at * ToV, ab * BoV, NoV));
        let lambdaL = NoV * length(float3(at * ToL, ab * BoL, NoL));

        // The 0.5 factor here accounts for the 1/(4*NoV*NoL) term
        return select(lambdaV + lambdaL > M_EPSILON, 0.5 / (lambdaV + lambdaL), 0.0);
    }

    // Fresnel-Schlick
    float3 fresnel(float cosTheta, float3 F0)
    {
        return saturate(F0 + (1.0 - F0) * pow_c_5(1.0 - cosTheta));
    }

    float3 fresnelSchlickRoughness(float cosTheta, float3 F0, float roughness)
    {
        return saturate(F0 + (max(float3(1.0-roughness), F0) - F0) * pow_c_5(1.0 - cosTheta));
    }

    float3 cookTorranceBRDF(
        float3 normal,
        float3 viewDir, float3 lightDir,
        float3 albedo, float metalness, float roughness,
        float3 F0, float3 tangent, float3 bitangent,
        float anisotropy,
        float3 energyCompensation) // Parameter kept as requested
    {
        roughness = max(roughness, MIN_DOT);

        let h = normalizeSafe(viewDir + lightDir, lightDir);
        let NdotV = dot_c(normal, viewDir);
        let NdotL = dot_c(normal, lightDir);
        let NdotH = dot_c(normal, h);
        let LdotH = dot_c(lightDir, h);

        // 1. Diffuse Term (Disney)
        // Note: Disney diffuse is not strictly energy conserving with the specular term
        // without complex coupling, but standard industry practice accepts this.
        let diffuse = diffuseDisney(albedo, NdotL, NdotV, LdotH, roughness);

        // 2. Specular Term
        float D = 0.0;
        float Vis = 0.0;

#if 0
        // Optimization: Branch usually coherent across a draw call
        if (abs(anisotropy) <= M_EPSILON)
        {
            // Isotropic Path
            // distribution() returns D
            D = distribution(normal, h, roughness);

            // smiths() returns G (Shadowing), so we must divide to get Vis
            let G = smiths(NdotV, NdotL, roughness);
            Vis = G / max(4.0 * NdotV * NdotL, M_EPSILON);
        }
        else
#endif
        {
            // Anisotropic Path
            // Filament parameterization
            let at = max(roughness * (1.0 + anisotropy), 0.001);
            let ab = max(roughness * (1.0 - anisotropy), 0.001);

            D = distributionAnisotropic(NdotH, h, tangent, bitangent, at, ab);

            // smithsAnisotropic returns Vis directly (includes 1/4*N.V*N.L)
            Vis = smithsAnisotropic(at, ab,
                dot(tangent, viewDir), dot(bitangent, viewDir),
                dot(tangent, lightDir), dot(bitangent, lightDir),
                NdotV, NdotL);
        }

        let F = fresnelSchlickRoughness(LdotH, F0, roughness);

        // Cook-Torrance Specular = D * F * Vis
        var specular = D * F * Vis * un_radiance_directSpecular;

        // Apply Multiple Scattering Energy Compensation (Filament method)
        // This brightens the specular lobe to account for missing multiple scattering bounces
        specular *= energyCompensation;

        // 3. Combination
        let kD = max((1.0 - F) * (1.0 - metalness), float3(0)) * un_radiance_directDiffuse;

        return kD * diffuse + specular;
    }

} //! namespace BxDF

namespace BxDF::IntegrationMapBRDF
{
    static const uint ANISOTROPY_DIM = 16u;
    static const uint NDOTV_DIM = 64u;
    static const uint ROUGHNESS_DIM = 128u;

    static const uint SAMPLE_COUNT = 1024u;

    void precompute(uint3 id)
    {
        let uvw = (id + 0.5) / float3(NDOTV_DIM, ROUGHNESS_DIM, ANISOTROPY_DIM);
        let brdf = integrateBRDFAnisotropic(uvw.x, uvw.y, uvw.z*2-1, SAMPLE_COUNT);
        //let brdf = integrateBRDF(uvw.x, uvw.y, SAMPLE_COUNT);
        let packed = packHalf2x16(brdf);
        rw_brdf_integration.get().store(id, packed);
    }

    float2 sample(float NdotV, float roughness, float anisotropy)
    {
        let pixel = saturate(float3(NdotV, roughness, anisotropy*0.5+0.5)) *
                    float3(NDOTV_DIM, ROUGHNESS_DIM, ANISOTROPY_DIM);
        let triilinear = Sampling::Trilinear(pixel);

        var result = float2(0);
        for (var it = triilinear.begin(); it != triilinear.end(); it++)
        {
            let smp = triilinear.at(it);
            let packed = rw_brdf_integration.get().load(smp.index);
            result += unpackHalf2x16ToFloat(packed) * smp.weight;
        }
        return result;
    }

    [ForceInline]
    float2 sample(float3 normal, float3 view, float roughness, float anisotropy)
    {
        return sample(dot_c(normal, view), roughness, anisotropy);
    }
}

//------------------------------------------------------------------------------------------
// - Unified Physically Based Light Evaluation (Point + Spot Lights)
//------------------------------------------------------------------------------------------

namespace Shading
{

struct Fragment
{
    uint2 pixel_id;
    uint2 screen_size;

    property float2 coord
    {
        [ForceInline]
        get
        {
            return Camera.fragmentCoord(this.pixel_id, this.screen_size);
        }
    }

    property float2 screen_uv
    {
        [ForceInline]
        get
        {
            return (this.pixel_id + 0.5) / this.screen_size;
        }
    }

    property bool debug_flip
    {
        get
        {
            let phi = M_PI;//fract(Time.elapsed / 4.0) * M_TAU;
            let d = dot(this.screen_uv - 0.5, dir2(phi));
            return fract(d + Time.elapsed*0.1) < 0.5;
        }
    }
};

struct Geometry
{
    float3 surface_to_eye;

    float3 position;
    float3 position_ddx;
    float3 position_ddy;

    float3 tangent;
    float3 binormal;
    float3 normal;

    // https://google.github.io/filament/Filament.md.html#lighting/imagebasedlights/anisotropy
    float3 reflect(float3 v, float roughness, float anisotropy)
    {
        let anisotropyDirection = select(anisotropy >= 0.0, this.binormal, this.tangent);
        let anisotropicTangent  = cross(anisotropyDirection, -v);
        let anisotropicNormal   = cross(anisotropicTangent, anisotropyDirection);
        let bentNormal          = normalizeSafe(lerp(this.normal, anisotropicNormal, abs(anisotropy)), this.normal);
        return reflect(v, bentNormal);
    }

};

typealias PackedMaterial = uint32_t4;

struct Material
{
    float3 albedo;
    float metallic;
    float3 emissive;
    float roughness;
    float3 absorption;
    float anisotropy;
    float ior;
    float transmittance;

    static const float IOR_MIN = 1.0;
    static const float IOR_MAX = 3.0;

    property float3 F0
    {
        [ForceInline]
        get
        {
            let tint = float3(sqr(max(this.ior - 1.0, 0.0)) / sqr(this.ior + 1.0));
            return lerp(tint, this.albedo, this.metallic);
        }
    }

    float3 brdf(Geometry geo, float3 light_dir, float3 F0, float3 energy_compensation)
    {
        return BxDF::cookTorranceBRDF(
            geo.normal,
            geo.surface_to_eye,
            light_dir,
            this.albedo,
            this.metallic,
            this.roughness,
            F0,
            geo.tangent,
            geo.binormal,
            this.anisotropy,
            energy_compensation );
    }

    uint32_t4 pack()
    {
        var packed: uint32_t4;
        packed.x = packUnorm4x8(saturate(float4(
            this.albedo,
            this.metallic)));
        packed.y = packUnorm4x8(saturate(float4(
            this.anisotropy*0.5+0.5,
            linearStep(IOR_MIN, IOR_MAX, this.ior),
            this.roughness,
            this.transmittance)));
        packed.z = Color::RGB9E5(this.emissive).packed;
        packed.w = Color::RGB9E5(this.absorption).packed;
        return packed;
    }

    static Material unpack(uint32_t4 packed)
    {
        var unpacked: Material;
        let unpacked_x = unpackUnorm4x8ToFloat(packed.x);
        unpacked.albedo = unpacked_x.rgb;
        unpacked.metallic = unpacked_x.a;
        let unpacked_y = unpackUnorm4x8ToFloat(packed.y);
        unpacked.anisotropy = unpacked_y.x*2-1;
        unpacked.ior = lerp(IOR_MIN, IOR_MAX, unpacked_y.y);
        unpacked.roughness = unpacked_y.z;
        unpacked.transmittance = unpacked_y.w;
        unpacked.emissive = Color::RGB9E5(packed.z).unpack();
        unpacked.absorption = Color::RGB9E5(packed.w).unpack();
        return unpacked;
    }
};

float3 directLighting<TRayTracer : Collision::IRayTracer>(
    Geometry geo,
    Material mat,
    float3 F0,
    float3 energy_compensation,
    TRayTracer shadow)
{
    let lights = LightCulling.sample(Map.project(geo.position));
    let refl = geo.reflect(-geo.surface_to_eye, mat.roughness, mat.anisotropy);

    var radiance = float3(0);
    for (var iter = lights.begin(); iter != lights.end(); iter++)
    {
        var light = lights.at(iter);
        if (light.insideBounds(geo.position))
        {
            // For area lights, use representative point for specular direction
            // This gives more accurate specular highlights for extended light sources
            var Ld : float;
            let Ls = light.effectiveDirection(geo.position, refl, Ld);
            let Li = light.influence(geo.position, geo.normal);

            if (dot2(Li) > M_EPSILON_SQ)
            {
                let hit = shadow.trace(geo.position + Ls * 0.01, geo.position + Ls * Ld);
                if (!hit.valid)
                    radiance += Li * mat.brdf(geo, Ls, F0, energy_compensation);
            }
        }
    }

    return radiance;
}

interface IIndirect
{
    static float3 diffuseIrradiance(float3 position, float3 normal);
    static float3 specularRadiance(float3 position, float3 refl, float roughness);
};

struct NullIndirect : IIndirect
{
    [ForceInline]
    static float3 diffuseIrradiance(float3 position, float3 normal)
    {
        return float3(0);
    }

    [ForceInline]
    static float3 specularRadiance(float3 position, float3 refl, float roughness)
    {
        return float3(0);
    }
};

//
// Lights allocation and culling
//

interface ILight
{
    bool insideBounds(float3 pos);
    float3 effectiveDirection(float3 pos, float3 refl);
    float3 influence(float3 pos, float3 normal);
};

// Unified light representation (point, spot, or area/disk)
struct Light
{
    float3 position;
    float luminance;  // luminous intensity in cd/m^2
    float3 direction; // ignored for point lights
    float range;      // max distance of influence
    float3 color;
    float radius;     // physical radius of area light (0.0 for punctual)
    float inner_cone; // radians; 0.0 for point light
    float outer_cone; // radians; 0.0 for point light

    // point light, color.a is luminance in cd/m^2
    __init(float3 position, float radius, float range, float3 color, float luminance)
    {
        this.position = position;
        this.luminance = luminance;
        this.direction = float3(0, 0, 0);
        this.range = range;
        this.color = color.rgb;
        this.radius = radius;
        this.inner_cone = 0.0;
        this.outer_cone = 0.0;
    }

    // spot light, color.a is luminance in cd/m^2
    __init(float3 start, float radius, float3 end, float cone, float3 color, float luminance)
    {
        this.position = start;
        this.luminance = luminance;
        this.direction = end - start;
        this.range = max(length(direction), M_EPSILON);
        this.direction /= this.range;
        this.color = color.rgb;
        this.radius = radius;
        this.outer_cone = saturate(cone) * M_PI;
        this.inner_cone = this.outer_cone * 0.8;
    }

    property bool is_point_light
    {
        [ForceInline]
        get
        {
            return this.outer_cone <= 0;
        }
    }
    property bool is_spot_light
    {
        [ForceInline]
        get
        {
            return this.outer_cone > 0;
        }
    }

    [ForceInline]
    Collision::Sphere boundingSphere()
    {
        return Collision::Sphere(this.position, this.range + M_EPSILON);
    }

    Collision::RoundedCone boundingCone()
    {
        if (this.is_point_light || this.outer_cone > M_PI / 2 - 0.01)
        {
            // For a point light, RoundedCone is just a sphere (pa == pb)
            return Collision::RoundedCone(this.position, this.range, this.position, this.range);
        }

        // P_a is the light source
        float3 pa = this.position;
        float ra = this.radius;

        // P_b is at the reach of the light.
        // To be perfectly tight, P_b should be at the origin
        // because the light is a 'spherical sector'.
        // But for a RoundedCone, we treat it as a sphere at the end.
        float3 pb = this.position + this.direction * this.range;

        // In a spotlight, the radius at 'range' is range * sin(outer_cone).
        float rb = ra + this.range * sin(this.outer_cone);
        return Collision::RoundedCone(pa, ra, pb, rb);
    }

    bool insideBounds(float3 pos)
    {
        return this.boundingSphere().contains(pos);
    }

    // Evaluate unified light contribution (supports punctual and area lights)
    float3 influence(float3 pos, float3 normal)
    {
        let L = this.position - pos;
        let dist = length(L);
        let dir = select(dist > M_EPSILON, L / dist, this.direction);

        // Use area light attenuation if radius > 0, otherwise standard inverse-square
        var attenuation : float;
        if (radius > 0.0)
        {
            attenuation = areaLightAttenuation(dist, this.radius, this.range);
            // Normalize energy so larger lights don't appear brighter
            attenuation *= areaLightNormalization(this.radius, dist);
        }
        else
        {
            attenuation = inverseSquareFalloff(dist, this.range);
        }

        // Spot attenuation (if cone defined)
        if (this.outer_cone > 0.0)
        {
            let cosTheta = dot(-dir, this.direction);
            let cosInner = cos(this.inner_cone);
            let cosOuter = cos(this.outer_cone);

            attenuation *= smoothstep(cosOuter, cosInner, cosTheta);
        }

        // For a sphere: Intensity (cd) = Luminance (cd/m^2) * Projected Area (m^2)
        // Projected Area of a sphere is a disk: PI * r^2
        let intensity = M_PI * this.luminance * max(this.radius * this.radius, 1.0);

        // Final radiance contribution
        let radiance = this.color * intensity * attenuation;
        return radiance;
    }

    // Representative point for area light specular (closest point on light disk to reflection ray)
    // For 2D disk lights, this finds the point on the light disk that contributes most to specular
    float3 representativePoint(float3 pos, float3 R)
    {
        if (radius <= 0.0)
        {
            return this.position; // Punctual light - use center
        }

        // Vector from shading point to light center
        let L = this.position - pos;

        // Project light center onto reflection ray, then find perpendicular offset
        let centerToRay = dot(L, R) * R - L;
        let centerToRayLen = length(centerToRay);

        // Clamp to light disk radius
        let t = saturate(radius / max(centerToRayLen, M_EPSILON));

        // Representative point is light center moved toward the reflection ray
        return this.position + centerToRay * t;
    }

    // Compute effective light direction for area light (used for specular)
    float3 effectiveDirection(float3 pos, float3 refl, out float out_dist_to_light)
    {
        let rep_point = representativePoint(pos, refl);
        let dir_to_light = rep_point - pos;
        out_dist_to_light = length(dir_to_light);
        return select(out_dist_to_light > M_EPSILON, dir_to_light / out_dist_to_light, this.direction);
    }

    // Smooth inverse-square falloff (Unreal/Filament inspired)
    static float inverseSquareFalloff(float dist, float range)
    {
        // Epic Games / Frostbite windowed falloff
        let dist_sq = max(dist * dist, 0.0001); // Prevent divide by zero
        let range_sq = range * range;

        // The "window" that forces the light to 0 at the range limit
        let distance_ratio = dist_sq / (range_sq * range_sq); // (d/r)^4
        let window = saturate(1.0 - distance_ratio);

        return (window * window) / dist_sq;
    }

    // Attenuation for area lights - prevents singularity at close distances
    static float areaLightAttenuation(float dist, float radius, float range)
    {
        // Effective distance never less than light radius (prevents singularity)
        let effective_dist = max(dist, radius);
        let invSq = rcp(effective_dist * effective_dist);
        // Smooth falloff at range boundary (Filament-style)
        let rangeFade = pow(saturate(1.0 - dist / range), 2.0);
        return invSq * rangeFade;
    }

    // Energy normalization for area lights to maintain consistent perceived brightness
    static float areaLightNormalization(float radius, float dist)
    {
        // Area lights have larger solid angle at close range, normalize to match point light falloff
        // This keeps perceived brightness consistent regardless of light radius
        let r_over_d = radius / max(dist, M_EPSILON);
        return rcp(1.0 + M_PI * r_over_d * r_over_d);
    }

    PackedLight pack()
    {
        return PackedLight(this);
    }
};

#if 1
struct PackedLight
{
    // Word 0: Position XY (half2)
    uint32_t pos_xy;
    // Word 1: Position Z (half) | Range (half)
    uint32_t pos_z_range;
    // Word 2: Color (RGB9E5)
    uint32_t linear_color;
    // Word 3: Direction (16b Oct) | Radius (8b) | Cones (8b)
    uint32_t dir_misc;

    property uint32_t4 packed
    {
        [ForceInline]
        get
        {
            return uint32_t4(pos_xy, pos_z_range, linear_color, dir_misc);
        }
    }

    [ForceInline]
    __init(uint32_t4 packed)
    {
        this.pos_xy = packed.x;
        this.pos_z_range = packed.y;
        this.linear_color = packed.z;
        this.dir_misc = packed.w;
    }

    __init(Light light)
    {
        // 1. Position & Range (3x 16-bit + 1x 16-bit)
        this.pos_xy = packHalf2x16(light.position.xy);
        this.pos_z_range = packHalf2x16(float2(light.position.z, light.range));

        // 2. Color (32-bit Shared Exponent)
        this.linear_color = Color::RGB9E5(light.color * light.luminance).packed;

        // 3. Direction (16-bit Octahedral)
        // Octahedral.encode returns [0,1] float2 [cite: 111-114].
        // We quantize this to 8-bit UNORM (0..255).
        float2 oct = Octahedral.encode(normalizeSafe(light.direction, float3(0, 0, 1)));
        uint dir_x = uint(saturate(oct.x) * 255.0 + 0.5);
        uint dir_y = uint(saturate(oct.y) * 255.0 + 0.5);
        uint packed_dir = dir_x | (dir_y << 8);

        // 4. Radius (8-bit mini-float approximation or scaled unorm)
        // Storing as simple scaled fraction relative to range or a dedicated 8-bit float.
        // Here we take top 8 bits of the half-float representation for simplicity/range.
        uint packed_radius = (f32tof16(light.radius) >> 8) & 0xFF;

        // 5. Cones (8-bit packed: 4 bits outer, 4 bits inner ratio)
        // Outer: 0..PI mapped to 0..15
        uint outer_bits = uint(saturate(light.outer_cone / M_PI) * 15.0 + 0.5) & 0xF;
        // Inner: Ratio inner/outer mapped to 0..15
        float cone_ratio = (light.outer_cone > M_EPSILON) ? (light.inner_cone / light.outer_cone) : 1.0;
        uint inner_bits = uint(saturate(cone_ratio) * 15.0 + 0.5) & 0xF;
        uint packed_cones = outer_bits | (inner_bits << 4);

        // Combine Word 3
        this.dir_misc = packed_dir | (packed_radius << 16) | (packed_cones << 24);
    }

    Light unpack()
    {
        var light : Light;

        // 1. Position & Range
        let unpacked_xy = unpackHalf2x16ToFloat(this.pos_xy);
        let unpacked_z_rng = unpackHalf2x16ToFloat(this.pos_z_range);

        light.position = float3(unpacked_xy, unpacked_z_rng.x);
        light.range = unpacked_z_rng.y;

        // 2. Color
        let unpacked_color = Color::RGB9E5(this.linear_color).unpack();
        // Safe luminance extraction
        light.luminance = max(max(unpacked_color.r, unpacked_color.g), unpacked_color.b);
        light.color = select(light.luminance > M_EPSILON, unpacked_color / light.luminance, float3(0));

        // 3. Direction
        float2 oct;
        oct.x = float(this.dir_misc & 0xFF) / 255.0;
        oct.y = float((this.dir_misc >> 8) & 0xFF) / 255.0;
        light.direction = Octahedral.decode(oct); // [cite: 114]

        // 4. Radius (Reconstruct half from top 8 bits)
        uint half_radius = ((this.dir_misc >> 16) & 0xFF) << 8;
        light.radius = f16tof32(half_radius);

        // 5. Cones
        uint cones = (this.dir_misc >> 24) & 0xFF;
        light.outer_cone = (float(cones & 0xF) / 15.0) * M_PI;
        light.inner_cone = light.outer_cone * (float(cones >> 4) / 15.0);

        return light;
    }
};
#else
struct PackedLight
{
    Light light;

    __init(Light light)
    {
        this.light = light;
    }

    Light unpack()
    {
        return this.light;
    }
};
#endif

typealias LightMask = Bitmask<4>;

static const uint MAX_LIGHTS = LightMask::CAPACITY;

struct LightCulling : IContainer<Light>
{
    LightMask lights;

    [ForceInline]
    __init()
    {
        this.lights.bits = uint32_t4(LightMask::WORD_FULL);
    }

    [ForceInline]
    __init(LightMask lights)
    {
        this.lights = lights;
    }

    property uint count
    {
        get {
            return this.lights.invert().count;
        }
    }

    property bool full
    {
        get {
            return this.lights.empty;
        }
    }

    [mutating] uint allocate()
    {
        var idx = this.lights.allocate();
        if (idx < 0)
        {
            // recycle a random light
            let rnd = hash(uint4(Time.frame)).x;
            idx = (rnd & LightMask::WORD_MASK);
        }
        return uint(idx);
    }

    [mutating]
    void release(uint alloc)
    {
        this.lights.release(alloc);
    }

    static const uint GRID_TILE = 16;
    static const uint GRID_DIM = MAP_SIZE / GRID_TILE;

    [ForceInline]
    static uint linearIndex(uint2 id)
    {
        return id.y * GRID_DIM + id.x;
    }

    static LightCulling load(uint2 id)
    {
        var lights : LightCulling;
        lights.lights.bits = rw_light_culling.get().load(linearIndex(id));
        return lights;
    }

    static LightCulling sample(int2 map_id)
    {
        if (Map.outsideBounds(map_id))
        {
            return LightCulling();
        }
        return load(map_id / GRID_TILE);
    }

    [ForceInline]
    static void store(uint2 id, LightCulling lights)
    {
        rw_light_culling.get().store(linearIndex(id), lights.lights.bits);
    }

    [ForceInline]
    static Light light(uint alloc)
    {
        return PackedLight(rw_light_allocation.get().load(alloc)).unpack();
    }

    LightCulling cull(Collision::Aabb bounds)
    {
        let quickReject = bounds.boundingSphere();
        var culling = LightCulling();
        var iter = this.lights.invert();
        while (!iter.empty)
        {
            let light_idx = iter.allocate();
            let light = This.light(light_idx);

            if (light.boundingSphere().intersect(quickReject))
            {
                // more precise test if light is a spot
                if (light.is_spot_light && !light.boundingCone().intersect(bounds))
                    continue;

                culling.lights.erase(light_idx);
            }
        }
        return culling;
    }

    // IContainer<Light>

    struct Iterator : IIterator
    {
        LightMask mask;
        uint light_idx;

        [ForceInline]
        __init()
        {
            this.mask = LightMask(0);
            this.light_idx = LightMask::CAPACITY;
        }

        [ForceInline]
        __init(LightMask mask)
        {
            this.mask = mask;
            this.light_idx = this.mask.allocate();
        }

        [ForceInline]
        This next()
        {
            var result = this;
            result.light_idx = result.mask.allocate();
            return result;
        }

        [ForceInline]
        bool equals(This other)
        {
            return this.light_idx == other.light_idx;
        }
    };

    [ForceInline]
    Iterator begin()
    {
        return Iterator(this.lights.invert());
    }

    [ForceInline]
    Iterator end()
    {
        return Iterator();
    }

    [ForceInline]
    Light at(Iterator iter)
    {
        return light(iter.light_idx);
    }
};

namespace LightManager
{
static void reset()
{
    [ForceUnroll]
    for (int i = 0; i < 4; i++)
    {
        rw_light_manager[i].store(LightMask::WORD_FULL);
    }
}

static bool empty()
{
    [ForceUnroll]
    for (int i = 0; i < 4; i++)
    {
        if (rw_light_manager[i].load() != LightMask::WORD_FULL)
            return false;
    }
    return true;
}

[mutating]
static LightCulling culling()
{
    var result : LightCulling;
    [ForceUnroll]
    for (int i = 0; i < 4; i++)
    {
        result.lights.bits[i] = rw_light_manager[i].load();
    }
    return result;
}

[mutating]
static Optional<uint> allocate()
{
    // Try each of the 4 words (128 bits total)
    for (uint i = 0; i < 4; i++)
    {
        var mask = rw_light_manager[i].load();
        while (mask != 0)
        {
            // Find the first set bit (available light)
            let bit = firstbitlow(mask);
            let bitMask = 1u << bit;

            // Attempt to clear the bit (set it to 0 / occupied)
            let original = rw_light_manager[i].and(~bitMask);

            // If the bit was 1 before we ANDed it, we successfully claimed it!
            if ((original & bitMask) != 0)
            {
                return i * LightMask::WORD_BITS + bit;
            }

            // Someone else beat us to it, update mask and try again
            mask = original & (~bitMask);
        }
    }
    return none; // Full
}

[mutating]
static Optional<uint> allocate(Light light)
{
    if (let alloc = allocate())
    {
        rw_light_allocation.get().store(alloc, PackedLight(light).packed);
        return alloc;
    }
    return none;
}

[mutating]
static void release(uint alloc)
{
    let set = alloc / LightMask::WORD_BITS;
    let mask = 1u << (alloc % LightMask::WORD_BITS);
    rw_light_manager[set].or(mask);
}
};

} //! namespace Shading

[playground::ZEROS(4)]
RWStructuredBuffer<Atomic<uint32_t>> rw_light_manager;

//------------------------------------------------------------------------------------------
// - Global illumination (2D, world space)
//------------------------------------------------------------------------------------------

#define use_radiance_supersampling 1 // %_NOCOMMIT%

namespace Radiance
{
// Sky integral formula taken from
// Analytic Direct Illumination - Mathis
// https://www.shadertoy.com/view/NttSW7
float3 radianceIntegrateSky_(float2 angle)
{
    // Sky radiance helper function
    let a1 = angle.y;
    let a0 = angle.x;
    let SkyColor = float3(0.2, 0.5, 1.);
    let SunColor = float3(1., 0.5, 0.1) * 20.;
    let SunA = 5.0;
    let SunS = 64.0;
    let SSunS = sqrt(SunS);
    let ISSunS = 1. / SSunS;
    var SI = SkyColor * (a1 - a0 - 0.5 * (cos(a1) - cos(a0)));
    SI += SunColor * (atan(SSunS * (SunA - a0)) - atan(SSunS * (SunA - a1))) * ISSunS;
    return SI / 6.0;
}

float3 radianceIntegrateSky(float2 angle)
{
    // Integrate the radiance from the sky over an interval of directions
    if (angle.y < M_TAU)
    {
        return radianceIntegrateSky_(angle);
    }
    return (radianceIntegrateSky_(float2(angle.x, M_TAU)) +
            radianceIntegrateSky_(float2(0.0, angle.y - M_TAU)));
}

struct CircularHarmonics<let ORDER: uint>
{
    static const uint ARITY = 1 + ORDER * 2;
    float3 coeffs[ARITY] = {};

    This lerp(This other, float f)
    {
        var result: This;
        [ForceUnroll]
        for (var i = 0u; i < ARITY; i++)
        {
            result.coeffs[i] = lerp(this.coeffs[i], other.coeffs[i], f);
        }
        return result;
    }

    // Project radiance into the CH basis
    [mutating]
    void project(float2 dir, float3 radiance, float delta_theta)
    {
        let theta = atan2(dir.y, dir.x);

        // Orthonormal constants
        let Y0 = 1.0 / sqrt(M_TAU);
        let Ym = 1.0 / sqrt(M_PI);

        // Apply Hann window to the radiance directly ONLY if you want to pre-filter
        // Otherwise, it's better to window the coefficients during evaluation.
        let weight = radiance * delta_theta;

        coeffs[0] += weight * Y0;

        [unroll]
        for (uint m = 1; m <= ORDER; m++)
        {
            coeffs[2*m - 1] += weight * Ym * cos(float(m) * theta);
            coeffs[2*m]     += weight * Ym * sin(float(m) * theta);
        }
    }

    // Standard Reconstruction (Radiance)
    float3 evaluate(float3 dir)
    {
        let theta = atan2(dir.y, dir.x);
        let Y0 = 1.0 / sqrt(M_TAU);
        let Ym = 1.0 / sqrt(M_PI);

        var result = coeffs[0] * Y0;

        [unroll]
        for (uint m = 1; m <= ORDER; m++)
        {
            // Apply windowing here to reduce Gibbs ringing (Windowing per order)
            result += Ym * (coeffs[2*m - 1] * cos(float(m) * theta) +
                            coeffs[2*m]     * sin(float(m) * theta));
        }
        return max(result, 0.0);
    }

    // Standard Reconstruction (Radiance)
    float3 evaluateWindowed(float3 dir)
    {
        let theta = atan2(-dir.y, -dir.x);
        let Y0 = 1.0 / sqrt(M_TAU);
        let Ym = 1.0 / sqrt(M_PI);

        var result = coeffs[0] * Y0;

        [unroll]
        for (uint m = 1; m <= ORDER; m++)
        {
            // Apply windowing here to reduce Gibbs ringing (Windowing per order)
            let window = exp(-0.1 * float(m * m));
            result += window * Ym * (coeffs[2*m - 1] * cos(float(m) * theta) +
                                     coeffs[2*m]     * sin(float(m) * theta));
        }
        return max(result, 0.0);
    }

    // Diffuse Irradiance (Cosine convolution)
    float3 diffuseIrradiance(float3 normal_dir)
    {
        let theta_n = atan2(normal_dir.y, normal_dir.x);
        let anisotropy = length(normal_dir.xy); // Weight by how much is in the plane

        // For 2D, the cosine lobe integration factors:
        // m=0: 2/sqrt(2pi) -> when multiplied by Y0 (1/sqrt(2pi)), it gives 1/pi * integration
        // Here we use simplified transfer factors for the 2D plane:
        let A0 = 1.0;
        var irr = coeffs[0] * (1.0 / sqrt(M_TAU));

        [unroll]
        for (uint m = 1; m <= ORDER; m++)
        {
            // Cosine kernel coefficients in CH: Am = sinc(m * pi / 2) is for 3D,
            // for 2D diffuse we can use a sharp decay:
            let Am = 1.0 / (1.0 + float(m*m));
            let Ym = 1.0 / sqrt(M_PI);

            let cos_mt = cos(float(m) * theta_n);
            let sin_mt = sin(float(m) * theta_n);
            irr += Am * anisotropy * Ym * (coeffs[2*m - 1] * cos_mt + coeffs[2*m] * sin_mt);
        }

        return max(irr * M_PI, 0.0); // Multiply by PI for Lambertian BRDF
    }

    float3 specularRadiance(float3 refl_dir, float roughness)
    {
        let theta_r = atan2(refl_dir.y, refl_dir.x);

        let Y0 = 1.0 / sqrt(M_TAU);
        let Ym = 1.0 / sqrt(M_PI);
        var rad = coeffs[0] * Y0;

        let sigma = roughness * roughness * 5.0;
        [unroll]
        for (uint m = 1; m <= ORDER; m++)
        {
            let decay = exp(-float(m*m) * sigma);
            rad += decay * Ym * (coeffs[2*m - 1] * cos(float(m) * theta_r) +
                                 coeffs[2*m]     * sin(float(m) * theta_r));
        }

        return max(rad, 0.0);
    }

    struct Streamer
    {
        // Accumulates Sum(Radiance * Weight * Basis)
        float3 coeffs[ARITY] = {};

        // Accumulates Sum(Weight * Basis).
        // This is required to subtract the 'Average' term correctly during resolve.
        // It is scalar because basis functions and weights are achromatic.
        float normalization[ARITY] = {};

        float total_weight = 0.0;

        [mutating]
        void add(float2 dir, float3 radiance, float weight)
        {
            weight = max(weight, M_EPSILON);

            // 1. Accumulate DC (Average Radiance) components
            this.coeffs[0] += radiance * weight;
            this.total_weight += weight;

            let theta = atan2(dir.y, dir.x);

            // 2. Accumulate AC components
            [unroll]
            for (uint m = 1; m <= ORDER; m++)
            {
                let phase = float(m) * theta;
                let c = cos(phase);
                let s = sin(phase);

                // Projection of L (Radiance)
                // We pre-multiply by weight here to match the Riemann sum
                coeffs[2*m - 1] += c * radiance * weight;
                coeffs[2*m]     += s * radiance * weight;

                // Projection of 1 (Geometry)
                // This tracks how much the basis functions correlate with the sample density
                // used to remove the "Average" leakage from the AC terms later.
                normalization[2*m - 1] += c * weight;
                normalization[2*m]     += s * weight;
            }
        }

        // Resolve to match CircularHarmonics<>::fit
        CircularHarmonics<ORDER> resolve()
        {
            CircularHarmonics<ORDER> result;

            // Step 1: Calculate the exact Average (DC)
            // fit(): coeffs[0] = average_radiance / Y0;
            let average_radiance = this.coeffs[0] / max(this.total_weight, M_EPSILON);
            let Y0 = 1.0 / sqrt(M_TAU);

            result.coeffs[0] = average_radiance / Y0;

            // Step 2: Higher orders
            let Ym = 1.0 / sqrt(M_PI);

            // We do not need an extra global weight factor here because
            // the 'weight' passed to add() (delta_theta) already accounts for the integration area.

            [unroll]
            for (uint m = 1; m <= ORDER; m++)
            {
                // Lanczos-style windowing
                let x = float(m) * M_PI / float(ORDER + 1);
                let window = (sin(x) / x);

                // The fit() function projects (Radiance - Average).
                // Linearity allows us to expand this:
                // Proj(L - Avg) = Proj(L) - Proj(Avg)
                //               = Proj(L) - Avg * Proj(1)

                let L_cos = this.coeffs[2*m - 1];
                let L_sin = this.coeffs[2*m];

                let G_cos = this.normalization[2*m - 1];
                let G_sin = this.normalization[2*m];

                let corrected_cos = L_cos - average_radiance * G_cos;
                let corrected_sin = L_sin - average_radiance * G_sin;

                result.coeffs[2*m - 1] = corrected_cos * Ym * window;
                result.coeffs[2*m]     = corrected_sin * Ym * window;
            }
            return result;
        }
    };
};

interface ICascadeTracer
{
    float4 trace(float2 start, float2 end);
};

struct Cascade : Shading::IIndirect
{
    static const uint   NUM_CASCADES    = 6;
    static const uint2  DIM_2D          = Map::DIM_2D;
    static const uint   DIM_1D          = Map::DIM_1D;

    static const uint   UPDATE_FREQ     = 2; // 1: every probe, 2: 1 over 4 probe updated per frame
    static const uint   DISPATCH        = MAP_SIZE >> (UPDATE_FREQ - 1);

    typealias Probe = float4;

    struct PackedProbe
    {
        uint32_t2 packed;

        [ForceInline]
        __init(uint32_t x, uint32_t y)
        {
            this.packed.x = x;
            this.packed.y = y;
        }

        [ForceInline]
        __init(uint32_t2 packed)
        {
            this.packed = packed;
        }

        [ForceInline]
        __init(Probe probe)
        {
            this.packed.x = packHalf2x16(probe.rg);
            this.packed.y = packHalf2x16(probe.ba);
        }

        [ForceInline]
        float4 unpack()
        {
            let unpacked_x = unpackHalf2x16ToFloat(this.packed.x);
            let unpacked_y = unpackHalf2x16ToFloat(this.packed.y);
            return Probe(unpacked_x, unpacked_y);
        }
    };

    uint level;

    property Cascade parent
    {
        [ForceInline]
        get { return Cascade(min(this.level + 1, NUM_CASCADES - 1)); }
    }

    property Cascade child
    {
        [ForceInline]
        get { return Cascade(select(this.level > 0, this.level - 1, 0)); }
    }

    property uint offset
    {
        [ForceInline]
        get { return this.level * DIM_1D; }
    }

    property uint probe_size
    {
        [ForceInline]
        get { return 2u << this.level; }
    }

    property uint num_dirs
    {
        [ForceInline]
        get { return 4u << (2u * this.level); }
    }

    property float2 depth_interval
    {
        [ForceInline]
        get { return float2(intervalStart(this.level), intervalStart(this.level+1)); }
    }

    static float intervalStart(uint cascade)
    {
#if 1
        let cascade_0_len = DIM_2D.x * un_radiance_penumbra_width * 1e-3;
        let cascade_scale = select(cascade > 0, 1 << (2u * cascade - 1), 1e-3);
        return cascade_0_len * cascade_scale;
#else
        return select(cascade == 0u, 0.0, pow(2.0, 2u*cascade));
#endif
    }

    [ForceInline]
    float2 probeDir(int2 probe, uint frame, uint phi)
    {
#if use_radiance_supersampling
        //let jitter = Noise.interleavedGradient(probe, Time.frame+7);
        //let jitter = Noise.interleavedGradient(probe, frame);
        //let ratio = (float(phi) + jitter) / float(this.num_dirs);
        let angular_offset = fract(float(Time.frame) * 0.6180339887); // golden ratio stepping
        let ratio = (float(phi) + angular_offset) / float(this.num_dirs);
#else
        let ratio = (float(phi) + 0.5) / float(this.num_dirs);
#endif
        return dir2(saturate(ratio) * M_TAU);
    }

    [ForceInline]
    float2 probeCenter(int2 probe, uint frame)
    {
#if use_radiance_supersampling && 0
        return (float2(probe) + 0.5) * float(this.probe_size) + (hammersley(frame, 16) - 0.5);
#else
        return (float2(probe) + 0.5) * float(this.probe_size);
#endif
    }

    [ForceInline]
    Sampling::Bilinear probeSample(float2 world_pos)
    {
        var relative_pos = world_pos / float(this.probe_size) - 0.5;
        return Sampling::Bilinear(relative_pos);
    }

    [ForceInline]
    static float4 merge(float4 near, float4 far)
    {
        return float4(near.rgb + near.a * far.rgb, near.a * far.a);
    }

    struct ProbeLocation
    {
        int2    probe;
        uint    phi;
        Cascade cascade;
    };

    ProbeLocation pixelToProbeLocation(int2 pixel)
    {
        pixel = min(pixel, DIM_2D-1);
        let probe_size = this.probe_size;
        let probe_index = pixel / probe_size;
        let probe_pos = probe_index * probe_size;
        let phi_rel = pixel - probe_pos;
        let phi = phi_rel.y * probe_size + phi_rel.x;
        return ProbeLocation(probe_index, phi, this);
    }

    [ForceInline]
    static uint probeFrame(int2 probe, uint frame)
    {
        const uint K = UPDATE_FREQ * UPDATE_FREQ;
        let quad = probe / UPDATE_FREQ;
        let seed = hash(quad.xy).x % K;
        let linear_index = (probe.x % UPDATE_FREQ) + (probe.y % UPDATE_FREQ) * UPDATE_FREQ;
        return (frame / K + linear_index + seed) % 16;
    }

    [ForceInline]
    static int2 probeToUpdate(uint2 quad, uint frame)
    {
        const uint K = UPDATE_FREQ * UPDATE_FREQ;
        uint seed = hash(quad).x % K; // Ensure hash returns uint

        uint cycle = (frame / K) % K;

        // Solve: (cycle + linear_index + seed) % K == frame % K
        // linear_index = (frame - cycle - seed) % K
        uint target_linear_index = (frame + 2 * K - cycle - seed) % K;

        return int2(quad * UPDATE_FREQ + uint2(
            target_linear_index % UPDATE_FREQ,
            target_linear_index / UPDATE_FREQ
        ));
    }

    [ForceInline]
    static int2 probeLocationToPixel(ProbeLocation location)
    {
        let probe_size = location.cascade.probe_size;
        let probe_pos = clamp(location.probe, int2(0), int2(DIM_2D / probe_size) - 1) * probe_size;
        //let probe_pos = location.probe * probe_size;
        let phi_rel = int2(location.phi % probe_size, location.phi / probe_size);
        return phi_rel + probe_pos;
    }

    static uint probeLocationToPixelIndex(ProbeLocation location)
    {
        var phi_pos = probeLocationToPixel(location);
        phi_pos = clamp(phi_pos, int2(0), int2(DIM_2D) - 1);
        let probe_linear_offset = phi_pos.y * DIM_2D.x + phi_pos.x;
        return location.cascade.offset + probe_linear_offset;
    }

    [ForceInline]
    static Probe load(ProbeLocation location)
    {
        let idx = probeLocationToPixelIndex(location);
        return rw_radiance_cascades[idx].unpack();
    }

    [ForceInline]
    static void store(ProbeLocation location, Probe probe)
    {
        let idx = probeLocationToPixelIndex(location);
        rw_radiance_cascades[idx] = PackedProbe(probe);
    }

    static Probe computeLastLevel<let USE_SKY_RADIANCE: bool>(ProbeLocation location, ICascadeTracer map)
    {
        let probe_frame = Cascade.probeFrame(location.probe, Time.frame);
        let probe_center = location.cascade.probeCenter(location.probe, probe_frame);
        let ray_interval = location.cascade.depth_interval;

        let parent_num_dirs = location.cascade.parent.num_dirs;
        let avg_num_dirs =  parent_num_dirs / location.cascade.num_dirs;

        let time_of_day = fract(Time.game / 24.0);

        var radiance = float4(0);
        for (var phi = 0u; phi < avg_num_dirs; phi++)
        {
            let probe_dir = location.cascade.probeDir(location.probe, probe_frame, location.phi * avg_num_dirs + phi);

            var radiance_far = float4(0,0,0,1);

            [flatten]
            if (USE_SKY_RADIANCE)
            {
                // If we are the top-level cascade, then there's no other
                // cascade to merge with, so instead merge with the sky radiance
                let parent_phi = float(location.phi * avg_num_dirs + phi) + 0.5;
                var angle = float2(parent_phi, parent_phi + 1) / float(parent_num_dirs);

#if 0
                angle *= M_TAU;
#else
                angle += time_of_day;

                angle *= M_TAU;
                angle.x = normalizeAnglePositive(angle.x);
                angle.y = normalizeAnglePositive(angle.y);

                angle = float2(
                    min(angle.x, angle.y),
                    max(angle.x, angle.y)
                );
#endif

                // radianceIntegrateSky already handles wrapping internally
                let norm = M_TAU * un_radiance_sky_intensity / (angle.y - angle.x);
                radiance_far = float4(radianceIntegrateSky(angle) * norm, 0);
            }

            let ray_start = probe_center + probe_dir * ray_interval.x;
            let ray_end = probe_center + probe_dir * ray_interval.y;

            var radiance_near = map.trace(ray_start, ray_end);

            radiance += merge(radiance_near, radiance_far);
        }
        radiance /= float(avg_num_dirs);

        return Probe(radiance.rgb, radiance.a);
    }

    static Probe computeLevelAndMergeParent(ProbeLocation location, ICascadeTracer map)
    {
        var parent : ProbeLocation;
        parent.cascade = location.cascade.parent;

        let probe_frame = Cascade.probeFrame(location.probe, Time.frame);
        let probe_center = location.cascade.probeCenter(location.probe, probe_frame);
        let probe_dir = location.cascade.probeDir(location.probe, probe_frame, location.phi);
        let ray_interval = location.cascade.depth_interval;

        let ray_start = probe_center + probe_dir * ray_interval.x;

        static const uint num_samples = 4u;
        static const uint avg_num_dirs = 4u;//parent.cascade.num_dirs / location.cascade.num_dirs;

        let bilinear = parent.cascade.probeSample(probe_center);
        let weights = bilinear.weights;

        var radiance = float4(0);
        [ForceUnroll]
        for (var sp = 0u; sp < num_samples; sp++)
        {
            parent.probe = bilinear.base_index + bilinear.offset(sp);

            let parent_probe_frame = Cascade.probeFrame(parent.probe, Time.frame);
            let parent_probe_center = parent.cascade.probeCenter(parent.probe, parent_probe_frame);
            let ray_end = parent_probe_center + probe_dir * ray_interval.y;

            var radiance_near = map.trace(ray_start, ray_end);

            parent.phi = location.phi * avg_num_dirs;
            let parent_base_index = probeLocationToPixelIndex(parent);

            var radiance_sp = float4(0);
            [ForceUnroll]
            for (var phi = 0u; phi < avg_num_dirs; phi++)
            {
                var radiance_far = rw_radiance_cascades[parent_base_index+phi].unpack();
                radiance_sp += merge(radiance_near, radiance_far);
            }
            radiance += radiance_sp * weights[sp];
        }
        radiance /= float(avg_num_dirs);

        return Probe(radiance.rgb, radiance.a);
    }

    [ForceInline]
    static void computeLevelAndStore<let LEVEL: uint>(uint2 dispatch_id, ICascadeTracer trace)
    {
        const Cascade cascade = Cascade(LEVEL);
        const uint probe_size = cascade.probe_size;

        // 1. Identify which "quad" (block of 2x2 probes) this thread belongs to.
        // Since each probe has 'probe_size' pixels in the atlas:
        let quad_id = dispatch_id / probe_size;

        // 2. Identify which direction (phi) within a probe this thread is responsible for.
        let phi_coord = dispatch_id % probe_size;
        let phi = phi_coord.y * probe_size + phi_coord.x;

        // 3. Determine which specific probe within the 2x2 quad to update this frame.
        // This uses your existing probeToUpdate logic.
        let target_probe = probeToUpdate(quad_id, Time.frame);

        // 4. Construct the ProbeLocation using the target probe but the FULL phi range.
        var location = ProbeLocation(target_probe, phi, cascade);

        var radiance: Probe;
        [flatten]
        if (LEVEL < NUM_CASCADES - 1)
        {
            radiance = computeLevelAndMergeParent(location, trace);
        }
        else
        {
            radiance = computeLastLevel<true>(location, trace);
        }

        store(location, radiance);
    }

    float3 eval(uint2 probe_id, float3 normal)
    {
        var location: ProbeLocation;
        location.cascade = this;
        location.probe = probe_id;

        let num_dirs = location.cascade.num_dirs;
        let probe_frame = probeFrame(location.probe, Time.frame);

        var radiance = float3(0);
        for (var phi = 0u; phi < num_dirs; phi++)
        {
            location.phi = phi;

            let probe_idx = probeLocationToPixelIndex(location);
            var radiance_far = rw_radiance_cascades[probe_idx].unpack();

            let probe_dir = location.cascade.probeDir(location.probe, probe_frame, location.phi);

            let nDotL = max(dot(normal.xy, probe_dir), abs(normal.z));
            radiance += radiance_far.rgb * nDotL;
        }

        return radiance * (M_PI / float(num_dirs));
    }

    static float3 irradiance(float2 map_pos, float3 normal)
    {
        var location: ProbeLocation;
        location.cascade = Cascade(0);

        let bilinear = location.cascade.probeSample(map_pos);
        let weights = bilinear.weights;
        let num_dirs = location.cascade.num_dirs;

        static const uint num_samples = 4u;

        var radiance = float3(0);
        for (var sp = 0u; sp < num_samples; sp++)
        {
            location.probe = bilinear.base_index + bilinear.offset(sp);
            let probe_frame = Cascade.probeFrame(location.probe, Time.frame);

            var radiance_sp = float3(0);
            for (var phi = 0u; phi < num_dirs; phi++)
            {
                location.phi = phi;

                let probe_idx = probeLocationToPixelIndex(location);
                var radiance_far = rw_radiance_cascades[probe_idx].unpack();

                let probe_dir = location.cascade.probeDir(location.probe, probe_frame, location.phi);

                let nDotL = max(dot(normal.xy, probe_dir), abs(normal.z));
                radiance_sp += radiance_far.rgb * nDotL;
            }
            radiance += radiance_sp * weights[sp];
        }

        return radiance * (M_PI / float(num_dirs));
    }

    [ForceInline]
    static float3 diffuseIrradiance(float3 map_pos, float3 normal)
    {
        return irradiance(map_pos.xy, normal);
    }

    [ForceInline]
    static float3 specularRadiance(float3 map_pos, float3 reflection, float roughness)
    {
        return irradiance(map_pos.xy, reflection);
    }

};


namespace Packing
{
    // A container for 8 packed uint32s
    struct Radiance9
    {
        uint data[8];
    };

    // Constants for RGB9E5 math
    static const float  RGB9E5_EXP_BIAS = 15.0;
    static const float  RGB9E5_MANTISSA_BITS = 9.0;
    static const uint   MAX_MANTISSA = 511; // (1 << 9) - 1

    // ------------------------------------------------------------------
    // PACK: 9 x float3 -> 8 x uint32
    // ------------------------------------------------------------------
    Radiance9 packRadiance9(float3 coefficients[9])
    {
        Radiance9 output;

        // 1. Find the global maximum absolute value across all 27 scalars
        float maxVal = 0.0;
        [unroll]
        for (int i = 0; i < 9; ++i)
        {
            float3 v = abs(coefficients[i]);
            maxVal = max(maxVal, max(v.x, max(v.y, v.z)));
        }

        // 2. Calculate Shared Exponent (Standard RGB9E5 bias logic)
        // exp_shared = ceil(log2(maxVal))
        // We add a small epsilon to maxVal to handle exact powers of 2 correctly
        // Clamp to [0, 31]
        float fLog = log2(max(maxVal, 1e-6));
        int iExp = int(ceil(fLog));
        uint sharedExp = uint(clamp(iExp + int(RGB9E5_EXP_BIAS), 0, 31));

        // 3. Calculate Inverse Scale for Quantization
        // Value = Mantissa * 2^(Exp - Bias - 9)
        // Mantissa = Value * 2^(-(Exp - Bias - 9))
        // Mantissa = Value * 2^(Bias + 9 - Exp)
        float scale = exp2(RGB9E5_EXP_BIAS + RGB9E5_MANTISSA_BITS - float(sharedExp));

        // Initialize bit stream
        [unroll] for(int k=0; k<8; k++) output.data[k] = 0;

        // Write 5-bit Exponent at the start of uint[0]
        // Current bit cursor
        uint bitOffset = 0;

        // Helper macro to write bits spanning across uint boundaries
        // Note: In Slang/HLSL, we manually manage the bit shifting for performance

        // Write Exponent (5 bits)
        output.data[0] = sharedExp;
        bitOffset += 5;

        // 4. Quantize and Write all 27 components
        [unroll]
        for (int i = 0; i < 9; ++i)
        {
            // Quantize vector
            // +0.5 for rounding, clamp to max mantissa
            uint3 mantissaVec = uint3(clamp(coefficients[i] * scale + 0.5, 0.0, float(MAX_MANTISSA)));

            // We need to write 3 components: x, y, z
            uint components[3] = { mantissaVec.x, mantissaVec.y, mantissaVec.z };

            [unroll]
            for (int c = 0; c < 3; ++c)
            {
                uint value = components[c];

                // Determine which uint index and bit-shift to use
                uint uintIdx = bitOffset >> 5;      // divide by 32
                uint bitShift = bitOffset & 31;     // modulo 32

                // Write lower part
                output.data[uintIdx] |= (value << bitShift);

                // If the value spans across the 32-bit boundary, write upper part to next uint
                // 9 bits can at most span 2 integers
                if (bitShift + 9 > 32)
                {
                    output.data[uintIdx + 1] |= (value >> (32 - bitShift));
                }

                bitOffset += 9;
            }
        }

        return output;
    }

    // ------------------------------------------------------------------
    // UNPACK: 8 x uint32 -> 9 x float3
    // ------------------------------------------------------------------
    void unpackRadiance9(Radiance9 packed, out float3 coefficients[9])
    {
        // 1. Read Shared Exponent (First 5 bits)
        uint sharedExp = packed.data[0] & 31; // Mask 0x1F

        // 2. Calculate Scale Factor
        // Scale = 2^(Exp - Bias - 9)
        float scale = exp2(float(sharedExp) - RGB9E5_EXP_BIAS - RGB9E5_MANTISSA_BITS);

        uint bitOffset = 5;

        // 3. Read and Dequantize all 27 components
        [unroll]
        for (int i = 0; i < 9; ++i)
        {
            float3 v;

            [unroll]
            for (int c = 0; c < 3; ++c)
            {
                uint uintIdx = bitOffset >> 5;
                uint bitShift = bitOffset & 31;

                // Read 32 bits from current slot
                uint raw = packed.data[uintIdx];

                // Extract bits based on shift
                // We need to handle the boundary crossing case
                uint mantissa = raw >> bitShift;

                if (bitShift + 9 > 32)
                {
                    // Or in the bits from the next integer
                    uint rawNext = packed.data[uintIdx + 1];
                    mantissa |= (rawNext << (32 - bitShift));
                }

                // Mask off the 9 bits we want
                mantissa &= MAX_MANTISSA;

                // Assign to vector component (0=x, 1=y, 2=z)
                if(c == 0) v.x = float(mantissa);
                if(c == 1) v.y = float(mantissa);
                if(c == 2) v.z = float(mantissa);

                bitOffset += 9;
            }

            coefficients[i] = v * scale;
        }
    }
}

#if use_radiance_supersampling
struct SuperSampling : Shading::IIndirect
{
    typealias ProbeLocation = Cascade::ProbeLocation;

    typealias PackedProbe = Packing::Radiance9;
    typealias Probe = CircularHarmonics<4>;

    // 7 coeffs
    // 7 half4

    static const uint probe_size = 2;
    static const uint DIM = MAP_SIZE/probe_size;
    static const uint DIM_1D = DIM*DIM;
    static const uint DISPATCH = DIM;

    [ForceInline]
    static float2 probeCenter(int2 probe)
    {
        return (probe + 0.5) * probe_size;
    }

    [ForceInline]
    static float2 probeDir(uint phi)
    {
        let ratio = (float(phi) + 0.5) / 4.0;
        return dir2(ratio * M_TAU);
    }

    [ForceInline]
    static Sampling::Bilinear probeSample(float2 map_pos)
    {
        return Cascade(0).probeSample(map_pos);
    }

    [ForceInline]
    static Probe load(int2 probe)
    {
        probe = clamp(probe, int2(0), int2(DIM-1));
        let packed = rw_radiance_supersampling[probe.x + probe.y * DIM];
        var probe: Probe;
        Packing::unpackRadiance9(packed, probe.coeffs);
        return probe;
    }

    [ForceInline]
    static void store(Probe ch, uint2 probe)
    {
        let packed = Packing::packRadiance9(ch.coeffs);
        rw_radiance_supersampling[probe.x + probe.y * DIM] = packed;
    }

    static void update2(int2 probe, ICascadeTracer map)
    {
        constexpr Cascade cascade = Cascade(0u);
        constexpr float2 ray_interval = cascade.depth_interval;

        let probe_center = probeCenter(probe) + Game.state.taa_offset - 0.5;

        constexpr uint num_samples = 4u;
        constexpr uint avg_num_dirs = 4u;//parent.cascade.num_dirs / location.cascade.num_dirs;

        let bilinear = cascade.parent.probeSample(probe_center);
        let weights = bilinear.weights;

        var streamer = Probe::Streamer();

        for (var ph = 0u; ph < avg_num_dirs; ph++)
        {
            [ForceUnroll]
            for (var sp = 0u; sp < num_samples; sp++)
            {
                var parent: ProbeLocation;
                parent.cascade = cascade.parent;
                parent.probe = bilinear.base_index + bilinear.offset(sp);
                parent.phi = avg_num_dirs * ph;

                let parent_probe_frame = Cascade.probeFrame(parent.probe, Time.frame);
                let parent_probe_center = cascade.parent.probeCenter(parent.probe, parent_probe_frame);
                let parent_base_index = Cascade.probeLocationToPixelIndex(parent);

                [ForceUnroll]
                for (var phi = 0u; phi < avg_num_dirs; phi++)
                {
                    let radiance_far = rw_radiance_cascades[parent_base_index+phi].unpack();
                    let probe_dir = parent.cascade.probeDir(parent.probe, parent.phi + phi, parent_probe_frame);

                    let ray_start = probe_center + probe_dir * ray_interval.x;
                    let ray_end = parent_probe_center + probe_dir * ray_interval.y;

                    let radiance_near = map.trace(ray_start, ray_end);
                    let radiance = Cascade.merge(radiance_near, radiance_far);

                    constexpr float delta_theta = M_TAU / float(cascade.parent.num_dirs);
                    streamer.add(probe_dir, radiance.rgb, weights[sp] * delta_theta);
                }
            }
        }

        let adaptation_rate = select(Time.frame > 0u, saturate(exp(-2.0 * Time.delta * un_radiance_learning_rate)), 1.0);
        var prev_ch = load(probe);
        if (Time.frame < 1u) prev_ch = Probe(0);

        var ch = streamer.resolve();
        ch = ch.lerp(prev_ch, adaptation_rate);

        store(ch, probe);
    }

    static void update(int2 probe, ICascadeTracer map)
    {
        static const Cascade cascade = Cascade(1u);
        let probe_center = probeCenter(probe);// + (Game.state.taa_offset - 0.5);
        let probe_box = probe_center.xyxy + (float2(-1, 1) * cascade.probe_size).xxyy;
        let sampler = cascade.probeSample(probe_center);

        var streamer = Probe::Streamer();

        for (var it = sampler.begin(); it != sampler.end(); it++)
        {
            let smp = sampler.at(it);
            let parent_probe_frame = Cascade.probeFrame(smp.index, Time.frame);
            let parent_probe_center = cascade.probeCenter(smp.index, parent_probe_frame);

            for (var phi = 0u; phi < cascade.num_dirs; phi++)
            {
                let location = ProbeLocation(smp.index, phi, cascade);
                let pixel_index = Cascade.probeLocationToPixelIndex(location);

                var radiance_far = rw_radiance_cascades[pixel_index].unpack();

                let parent_probe_dir = cascade.probeDir(smp.index, parent_probe_frame, phi);
                let probe_hit = Collision.intersectRayAABBInside(parent_probe_center, parent_probe_dir, probe_box);
                let probe_dir = normalizeSafe(parent_probe_center + parent_probe_dir * probe_hit - probe_center, parent_probe_dir);

                var radiance_near = map.trace(
                    probe_center + probe_dir * 1e-3,
                    parent_probe_center + parent_probe_dir * probe_hit);
                let radiance_merged = Cascade.merge(radiance_near, radiance_far);

                let delta_theta = M_TAU / float(cascade.num_dirs);
                streamer.add(probe_dir, radiance_merged.rgb, smp.weight * delta_theta);
            }
        }

        let adaptation_rate = select(Time.frame > 0u, saturate(exp(-2.0 * Time.delta * un_radiance_learning_rate)), 1.0);
        var prev_ch = load(probe);
        if (Time.frame < 1u) prev_ch = Probe(0);

        var ch = streamer.resolve();
        ch = ch.lerp(prev_ch, adaptation_rate);

        store(ch, probe);
    }

    [ForceInline]
    static float3 eval(Probe probe, float3 normal)
    {
        return probe.evaluate(normal);
    }

    [ForceInline]
    static float3 diffuseIrradiance(float3 map_pos, float3 normal)
    {
        let sampler = probeSample(map_pos.xy);

        var radiance = float3(0);
        for (var it = sampler.begin(); it != sampler.end(); it++)
        {
            let smp = sampler.at(it);
            let probe = load(smp.index);
            radiance += probe.diffuseIrradiance(normal) * smp.weight;
        }

        return radiance;
    }

    [ForceInline]
    static float3 specularRadiance(float3 map_pos, float3 refl, float roughness)
    {
        let sampler = probeSample(map_pos.xy);

        var radiance = float3(0);
        for (var it = sampler.begin(); it != sampler.end(); it++)
        {
            let smp = sampler.at(it);
            let probe = load(smp.index);
            radiance += probe.specularRadiance(refl, roughness) * smp.weight;
        }

        return radiance;
    }

}; //! struct SuperSampling
#else
typealias SuperSampling = Shading::NullIndirect;
#endif //!use_radiance_supersampling

#if use_radiance_supersampling
typealias GI = SuperSampling;
#else
typealias GI = Cascade;
#endif

} //! namespace Radiance

[playground::SLIDER(1.0, 0.0, 2.0)]
uniform float un_radiance_learning_rate;
[playground::SLIDER(0.5, 0.1, 2.0)]
uniform float un_radiance_penumbra_width;
[playground::SLIDER(5.0, 0.0, 20.0)]
uniform float un_radiance_sky_intensity;

// #TODO: move to RWTexture2DArray

[playground::ZEROS((Radiance::Cascade::NUM_CASCADES) * Radiance::Cascade::DIM_1D)]
RWStructuredBuffer<Radiance::Cascade::PackedProbe> rw_radiance_cascades;

#if use_radiance_supersampling
[playground::ZEROS(Radiance::SuperSampling::DIM_1D)]
RWStructuredBuffer<Radiance::SuperSampling::PackedProbe> rw_radiance_supersampling;
#endif

//==========================================================================================
// Simulation
//__________________________________________________________________________________________
//==========================================================================================

namespace Sim
{
struct Material
{
    float molar_mass;           // g/mol
    float density;              // kg/m^3
    float stiffness;            // GPa
    float viscosity;            // Pa·s
    float heat_capacity;        // J/(g·K)
    float thermal_conductivity; // W/(m·K)
    float surface_tension;      // N/m
    float friction;             // [0-1]
};

struct PackedMaterial
{
    // Word 0: molar_mass (16b) | density (16b)
    uint32_t mass_density;
    // Word 1: stiffness (16b) | viscosity (16b)
    uint32_t stiff_visc;
    // Word 2: heat_capacity (16b) | thermal_conductivity (16b)
    uint32_t heat_thermal;
    // Word 3: surface_tension (16b) | friction (8b) | padding (8b)
    uint32_t tension_friction;

    __init(Material p)
    {
        const float FP16_MAX = 65504.0;

        // Clamp to prevent Neutronium overflow
        float2 md = min(float2(p.molar_mass, p.density), FP16_MAX);
        float2 sv = min(float2(p.stiffness, p.viscosity), FP16_MAX);
        float2 ht = min(float2(p.heat_capacity, p.thermal_conductivity), FP16_MAX);

        this.mass_density = packHalf2x16(md);
        this.stiff_visc = packHalf2x16(sv);
        this.heat_thermal = packHalf2x16(ht);

        uint tension = f32tof16(min(p.surface_tension, FP16_MAX));
        uint friction = uint(saturate(p.friction) * 255.0 + 0.5);
        this.tension_friction = tension | (friction << 16);
    }

    Map::Element::Physics unpack()
    {
        let md = unpackHalf2x16ToFloat(this.mass_density);
        let sv = unpackHalf2x16ToFloat(this.stiff_visc);
        let ht = unpackHalf2x16ToFloat(this.heat_thermal);

        let tension = f16tof32(uint16_t(this.tension_friction & 0xFFFF));
        let friction = float((this.tension_friction >> 16) & 0xFF) / 255.0;

        Map::Element::Physics p;
        p.molar_mass = md.x;
        p.density = md.y;
        p.stiffness = sv.x;
        p.viscosity = sv.y;
        p.heat_capacity = ht.x;
        p.thermal_conductivity = ht.y;
        p.surface_tension = tension;
        p.friction = friction;
        return p;
    }
};

}; //! namespace Sim

//==========================================================================================
// Gameplay map
//__________________________________________________________________________________________
//==========================================================================================

namespace Map
{

//------------------------------------------------------------------------------------------
// Map elements
//------------------------------------------------------------------------------------------

enum Phase
{
    Gas,
    Liquid,
    Solid,
    Translucent,
    Movable,
};

#define ELEMENT_PACK(phase, id) \
    (((uint(phase) & 0x7u) | uint(id << 3u)) & 0xFFu)

enum Element : uint
{
    VACUUM = 0,
    INVALID = ELEMENT_PACK(Phase::Solid, 0xFF),

    // --- Gases (tend to rise or sink based on molar_mass) ---
    STEAM = ELEMENT_PACK(Phase::Gas, 1),
    OXYGEN = ELEMENT_PACK(Phase::Gas, 2),
    CARBON_DIOXIDE = ELEMENT_PACK(Phase::Gas, 3),
    HYDROGEN = ELEMENT_PACK(Phase::Gas, 4),         // Lightest gas, colorless
    HELIUM = ELEMENT_PACK(Phase::Gas, 5),           // Very light, colorless
    CHLORINE = ELEMENT_PACK(Phase::Gas, 6),         // Yellow-green, toxic
    NITROGEN = ELEMENT_PACK(Phase::Gas, 7),         // Colorless, inert
    METHANE = ELEMENT_PACK(Phase::Gas, 8),          // Colorless, flammable
    HYDROGEN_SULFIDE = ELEMENT_PACK(Phase::Gas, 9), // Rotten egg smell, yellowish

    // --- Liquids (incompressible, flow based on viscosity) ---
    WATER = ELEMENT_PACK(Phase::Liquid, 1),
    OIL = ELEMENT_PACK(Phase::Liquid, 2),

    // --- Solids (immovable, high density and stiffness) ---
    DIRT = ELEMENT_PACK(Phase::Solid, 1),
    STONE = ELEMENT_PACK(Phase::Solid, 2),
    GRANITE = ELEMENT_PACK(Phase::Solid, 3),
    URANIUM = ELEMENT_PACK(Phase::Solid, 4),
    NEUTRONIUM = ELEMENT_PACK(Phase::Solid, 5),  // Indestructible
    COBALT = ELEMENT_PACK(Phase::Solid, 6),      // Metallic blue
    SULFUR = ELEMENT_PACK(Phase::Solid, 7),      // Yellow crystalline
    BASALT = ELEMENT_PACK(Phase::Solid, 8),      // Volcanic rock
    MARBLE = ELEMENT_PACK(Phase::Solid, 9),      // Metamorphic limestone
    IRON = ELEMENT_PACK(Phase::Solid, 10),       // Iron metal
    GOLD = ELEMENT_PACK(Phase::Solid, 11),       // Precious metal
    COPPER = ELEMENT_PACK(Phase::Solid, 12),     // Red metal
    CLAY = ELEMENT_PACK(Phase::Solid, 13),       // Fine-grained soil
    ABYSSALITE = ELEMENT_PACK(Phase::Solid, 14), // Destructible
    WAX = ELEMENT_PACK(Phase::Solid, 15),        // Subsurface scattering material

    // --- Transparent Solids (translucent materials) ---
    EMERALD = ELEMENT_PACK(Phase::Translucent, 0),  // Green gem
    SAPPHIRE = ELEMENT_PACK(Phase::Translucent, 1), // Blue gem
    DIAMOND = ELEMENT_PACK(Phase::Translucent, 2),  // Clear/white gem
    OPAL = ELEMENT_PACK(Phase::Translucent, 3),     // Iridescent gem
    RUBY = ELEMENT_PACK(Phase::Translucent, 4),     // Red gem
    QUARTZ = ELEMENT_PACK(Phase::Translucent, 5),   // Clear/white crystal
    JADE = ELEMENT_PACK(Phase::Translucent, 6),     // Green stone
    ICE = ELEMENT_PACK(Phase::Translucent, 7),      // Frozen water
    GLASS = ELEMENT_PACK(Phase::Translucent, 8),    // Transparent glass material

    // --- Movable Solids (granular, subject to friction) ---
    SAND = ELEMENT_PACK(Phase::Movable, 1),
    SALT = ELEMENT_PACK(Phase::Movable, 2),
    EMBER = ELEMENT_PACK(Phase::Movable, 3),      // Hot, glowing sand
    TERRACOTTA = ELEMENT_PACK(Phase::Movable, 4), // Reddish-brown ceramic
    SNOW = ELEMENT_PACK(Phase::Movable, 5),       // Snow (powdery / compact depending on mass)
    LAVA = ELEMENT_PACK(Phase::Movable, 6),
};

#undef ELEMENT_PACK

extension Element
{
    static const uint MAX_NUM = 256;

    typealias Shading = Shading::Material;
    typealias Physics = Sim::Material;

    property uint packed
    {
        [ForceInline]
        get
        {
            return uint(this);
        }
    }
    property uint id
    {
        [ForceInline]
        get
        {
            return packed >> 3u;
        }
    }
    property Phase phase
    {
        [ForceInline]
        get
        {
            return reinterpret<Phase>(packed & 0x7u);
        }
    }
    property bool is_interior
    {
        get {
            switch (this.phase)
            {
            case Phase::Movable:
            case Phase::Solid:
            case Phase::Translucent:
                return true;
            case Phase::Gas:
            case Phase::Liquid:
            default:
                return false;
            }
        }
    }

    Shading shading()
    {
        return ::Shading::Material.unpack(rw_element_properties[packed]);
    }

    Physics physics()
    {
        return bit_cast<::Sim::PackedMaterial>(rw_element_properties[MAX_NUM + packed]).unpack();
    }
};

void initElementShading(Element elt, float3 albedo, float metallic, float3 emissive, float roughness, float3 absorption, float transmittance, float ior, float anisotropy)
{
    rw_element_properties[uint(elt)] = Shading::Material(albedo, metallic, emissive, roughness, absorption, anisotropy, ior, transmittance).pack();
}
void initElementPhysics(Element elt, float molar_mass, float density, float stiffness, float viscosity, float heat_capacity, float thermal_conductivity, float surface_tension, float friction)
{
    rw_element_properties[Element::MAX_NUM + uint(elt)] = bit_cast<uint32_t4>(Sim::PackedMaterial(Sim::Material(molar_mass, density, stiffness, viscosity, heat_capacity, thermal_conductivity, surface_tension, friction)));
}

void initElementTables()
{
    // -------------------------------------------------------------------------------------------------------------------------------------------
    //                                                      Material(albedo)               metallic   emissive(rgb)               roughness  absorption(rgb)                transmittance  ior     anisotropy
    //                                                      [—]                            [0-1]      [linear RGB]                [0-1]      [per unit]                     [0-1]          [>1]    [-1-1]
    // -------------------------------------------------------------------------------------------------------------------------------------------
    initElementShading(Element::VACUUM, float3(1.00, 1.00, 1.00), 0.00, float3(0.00, 0.00, 0.00), 1.00, float3(0.00, 0.00, 0.00), 1.00, 1.00, 0.00);  // Vacuum (fully transparent, no refraction)
    initElementShading(Element::INVALID, float3(1.00, 0.00, 0.00), 1.00, float3(1.00, 0.00, 0.00), 0.00, float3(0.00, 0.00, 0.00), 0.00, 1.50, 0.00); // INVALID (opaque metallic)

    // --- Gases --- (tend to rise or sink based on molar_mass, mostly transparent with low IOR, no anisotropy)
    initElementShading(Element::STEAM, float3(0.70, 0.70, 0.80), 0.00, float3(0.00, 0.00, 0.00), 1.00, float3(0.01, 0.01, 0.01), 0.95, 1.00, 0.00);            // Steam (slightly opaque for visibility, low absorption)
    initElementShading(Element::OXYGEN, float3(0.95, 0.97, 1.00), 0.00, float3(0.00, 0.00, 0.00), 1.00, float3(0.001, 0.001, 0.001), 0.99, 1.00, 0.00);     // Oxygen (nearly transparent)
    initElementShading(Element::CARBON_DIOXIDE, float3(0.50, 0.50, 0.50), 0.00, float3(0.00, 0.00, 0.00), 1.00, float3(0.02, 0.02, 0.02), 0.90, 1.00, 0.00);   // CO2 (slightly hazy)
    initElementShading(Element::HYDROGEN, float3(0.95, 0.98, 1.00), 0.00, float3(0.00, 0.00, 0.00), 1.00, float3(0.001, 0.001, 0.001), 0.99, 1.00, 0.00);   // Hydrogen (nearly transparent)
    initElementShading(Element::HELIUM, float3(0.90, 0.95, 1.00), 0.00, float3(0.00, 0.00, 0.00), 1.00, float3(0.001, 0.001, 0.001), 0.99, 1.00, 0.00);     // Helium (nearly transparent)
    initElementShading(Element::CHLORINE, float3(0.70, 0.90, 0.30), 0.00, float3(0.00, 0.00, 0.00), 1.00, float3(0.05, 0.08, 0.02), 0.85, 1.00, 0.00);         // Chlorine (hazy yellow-green)
    initElementShading(Element::NITROGEN, float3(0.85, 0.90, 0.95), 0.00, float3(0.00, 0.00, 0.00), 1.00, float3(0.001, 0.001, 0.001), 0.99, 1.00, 0.00);   // Nitrogen (nearly transparent)
    initElementShading(Element::METHANE, float3(0.80, 0.85, 0.90), 0.00, float3(0.00, 0.00, 0.00), 1.00, float3(0.01, 0.01, 0.01), 0.95, 1.00, 0.00);          // Methane (slightly hazy)
    initElementShading(Element::HYDROGEN_SULFIDE, float3(0.90, 0.85, 0.60), 0.00, float3(0.00, 0.00, 0.00), 1.00, float3(0.03, 0.03, 0.02), 0.90, 1.00, 0.00); // H2S (slightly hazy yellowish)

    // --- Liquids --- (higher IOR, some transmittance for translucency)
    initElementShading(Element::WATER, float3(0.20, 0.40, 0.90), 0.00, float3(0.00, 0.00, 0.00), 0.02, float3(0.00, 0.00, 0.00), 0.95, 1.33, 0.00); // Water (highly transmissive, water IOR)
    initElementShading(Element::OIL, float3(0.10, 0.10, 0.00), 0.00, float3(0.00, 0.00, 0.00), 0.35, float3(0.00, 0.00, 0.00), 0.80, 1.47, 0.00);   // Oil (somewhat transmissive, oil IOR)

    // --- Solids --- (mostly opaque, low transmittance, varied IOR)
    initElementShading(Element::DIRT, float3(0.40, 0.20, 0.10), 0.00, float3(0.00, 0.00, 0.00), 0.90, float3(0.00, 0.00, 0.00), 0.00, 1.50, 0.00);       // Dirt (opaque, matte)
    initElementShading(Element::STONE, float3(0.50, 0.50, 0.50), 0.00, float3(0.00, 0.00, 0.00), 0.80, float3(0.00, 0.00, 0.00), 0.00, 1.50, 0.00);      // Stone (opaque)
    initElementShading(Element::GRANITE, float3(0.60, 0.50, 0.50), 0.00, float3(0.00, 0.00, 0.00), 0.70, float3(0.00, 0.00, 0.00), 0.00, 1.50, 0.00);    // Granite (opaque)
    initElementShading(Element::BASALT, float3(0.20, 0.20, 0.25), 0.00, float3(0.00, 0.00, 0.00), 0.75, float3(0.00, 0.00, 0.00), 0.00, 1.50, 0.00);     // Basalt (opaque)
    initElementShading(Element::MARBLE, float3(0.90, 0.90, 0.85), 0.00, float3(0.00, 0.00, 0.00), 0.50, float3(0.00, 0.00, 0.00), 0.10, 1.50, 0.00);     // Marble (slight translucency)
    initElementShading(Element::IRON, float3(0.45, 0.45, 0.50), 1.00, float3(0.00, 0.00, 0.00), 0.20, float3(0.00, 0.00, 0.00), 0.00, 2.50, 0.20);       // Iron (metallic, slight anisotropy for brushed look)
    initElementShading(Element::GOLD, float3(1.00, 0.84, 0.00), 1.00, float3(0.00, 0.00, 0.00), 0.15, float3(0.00, 0.00, 0.00), 0.00, 2.50, 0.10);       // Gold (metallic, low anisotropy)
    initElementShading(Element::COPPER, float3(0.80, 0.40, 0.20), 1.00, float3(0.00, 0.00, 0.00), 0.25, float3(0.00, 0.00, 0.00), 0.00, 2.50, 0.15);     // Copper (metallic, medium anisotropy)
    initElementShading(Element::CLAY, float3(0.60, 0.35, 0.25), 0.00, float3(0.00, 0.00, 0.00), 0.85, float3(0.00, 0.00, 0.00), 0.00, 1.50, 0.00);       // Clay (opaque)
    initElementShading(Element::URANIUM, float3(0.20, 0.90, 0.20), 0.30, float3(0.20, 0.60, 0.10), 0.40, float3(0.00, 0.00, 0.00), 0.00, 2.00, 0.10);    // Uranium (semi-metallic, emissive)
    initElementShading(Element::NEUTRONIUM, float3(0.01, 0.01, 0.03), 1.00, float3(0.00, 0.00, 0.00), 0.02, float3(0.00, 0.00, 0.00), 0.00, 3.00, 0.00); // Neutronium (hypothetical, high IOR)
    initElementShading(Element::ABYSSALITE, float3(0.02, 0.02, 0.08), 0.80, float3(0.00, 0.00, 0.00), 0.05, float3(0.00, 0.00, 0.00), 0.00, 2.50, 0.00); // Abyssalite (hypothetical metallic)
    initElementShading(Element::COBALT, float3(0.00, 0.20, 0.80), 1.00, float3(0.00, 0.00, 0.00), 0.20, float3(0.00, 0.00, 0.00), 0.00, 2.50, 0.20);     // Cobalt (metallic, anisotropy for texture)
    initElementShading(Element::SULFUR, float3(1.00, 0.90, 0.10), 0.00, float3(0.00, 0.00, 0.00), 0.70, float3(0.00, 0.00, 0.00), 0.05, 1.80, 0.00);     // Sulfur (slight translucency)
    initElementShading(Element::WAX, float3(0.90, 0.85, 0.70), 0.00, float3(0.00, 0.00, 0.00), 0.60, float3(0.02, 0.02, 0.01), 0.20, 1.45, 0.00);        // Wax (translucent, wax IOR)

    // --- Movable Solids --- (mostly opaque, some with slight translucency)
    initElementShading(Element::SAND, float3(0.90, 0.80, 0.50), 0.00, float3(0.00, 0.00, 0.00), 0.80, float3(0.00, 0.00, 0.00), 0.00, 1.50, 0.00);       // Sand (opaque)
    initElementShading(Element::SALT, float3(0.90, 0.90, 0.95), 0.00, float3(0.00, 0.00, 0.00), 0.70, float3(0.00, 0.00, 0.00), 0.10, 1.50, 0.00);       // Salt (slight translucency)
    initElementShading(Element::EMBER, float3(0.90, 0.50, 0.20), 0.00, float3(1.00, 0.20, 0.00), 0.50, float3(0.00, 0.00, 0.00), 0.00, 1.50, 0.00);      // Ember (opaque, emissive)
    initElementShading(Element::TERRACOTTA, float3(0.80, 0.40, 0.20), 0.00, float3(0.00, 0.00, 0.00), 0.85, float3(0.00, 0.00, 0.00), 0.00, 1.50, 0.00); // Terracotta (opaque)
    initElementShading(Element::SNOW, float3(0.95, 0.95, 1.00), 0.00, float3(0.00, 0.00, 0.00), 0.15, float3(0.00, 0.00, 0.00), 0.20, 1.31, 0.00);       // Snow (translucent, ice-like IOR)
    initElementShading(Element::LAVA, float3(0.80, 0.20, 0.00), 0.00, float3(1.00, 0.30, 0.00), 0.40, float3(0.00, 0.00, 0.00), 0.10, 1.50, 0.00);       // Lava (slight translucency, emissive)

    // --- Transparent Solids (Gems and Glass) --- (high transmittance, varied IOR for refraction, some anisotropy)
    initElementShading(Element::EMERALD, float3(0.00, 0.80, 0.40), 0.00, float3(0.00, 0.00, 0.00), 0.10, float3(0.01, 0.02, 0.01), 0.90, 1.57, 0.05);        // Emerald (transmissive, emerald IOR, slight anisotropy)
    initElementShading(Element::SAPPHIRE, float3(0.20, 0.40, 0.90), 0.00, float3(0.00, 0.00, 0.00), 0.10, float3(0.01, 0.01, 0.02), 0.90, 1.77, 0.05);       // Sapphire (transmissive, sapphire IOR)
    initElementShading(Element::DIAMOND, float3(0.95, 0.95, 1.00), 0.00, float3(0.00, 0.00, 0.00), 0.05, float3(0.001, 0.001, 0.001), 0.95, 2.42, 0.00);  // Diamond (highly transmissive, diamond IOR)
    initElementShading(Element::OPAL, float3(0.90, 0.70, 0.90), 0.00, float3(0.00, 0.00, 0.00), 0.15, float3(0.01, 0.01, 0.01), 0.85, 1.45, 0.10);           // Opal (translucent, opal IOR, anisotropy for iridescence)
    initElementShading(Element::RUBY, float3(0.90, 0.10, 0.20), 0.00, float3(0.00, 0.00, 0.00), 0.10, float3(0.02, 0.01, 0.01), 0.90, 1.77, 0.05);           // Ruby (transmissive, ruby IOR)
    initElementShading(Element::QUARTZ, float3(0.90, 0.90, 0.95), 0.00, float3(0.00, 0.00, 0.00), 0.08, float3(0.001, 0.001, 0.001), 0.95, 1.54, 0.00);   // Quartz (highly transmissive, quartz IOR)
    initElementShading(Element::JADE, float3(0.30, 0.70, 0.50), 0.00, float3(0.00, 0.00, 0.00), 0.12, float3(0.01, 0.02, 0.01), 0.80, 1.60, 0.00);           // Jade (translucent, jade IOR)
    initElementShading(Element::ICE, float3(0.85, 0.90, 1.00), 0.00, float3(0.00, 0.00, 0.00), 0.05, float3(0.005, 0.005, 0.003), 0.90, 1.31, 0.00);        // Ice (transmissive, ice IOR)
    initElementShading(Element::GLASS, float3(0.95, 0.95, 1.00), 0.00, float3(0.00, 0.00, 0.00), 0.02, float3(0.0005, 0.0005, 0.0005), 0.98, 1.50, 0.00); // Glass (highly transmissive, glass IOR)

    // -------------------------------------------------------------------------------------------------------------------------------------------
    //                                                      Physics(molar_mass)         density     stiffness  viscosity  heat_cap      conductivity   surf_tens    friction
    //                                                      [g/mol]                     [kg/m^3]    [GPa]      [Pa·s]     [J/(g·K)]     [W/(m·K)]      [N/m]        [—]
    // -------------------------------------------------------------------------------------------------------------------------------------------
    initElementPhysics(Element::VACUUM, 0.0, 0.00, 0.00, 0.00, 0.0, 0.00, 0.0, 0.0);  // Vacuum
    initElementPhysics(Element::INVALID, 0.0, 0.00, 0.00, 0.00, 0.0, 0.00, 0.0, 0.0); // INVALID

    // --- Gases ---
    initElementPhysics(Element::STEAM, 18.0, 0.60, 0.10, 0.10, 2.0, 0.02, 0.0, 0.0);             // Steam
    initElementPhysics(Element::OXYGEN, 32.0, 1.40, 0.20, 0.20, 0.9, 0.02, 0.0, 0.0);            // Oxygen
    initElementPhysics(Element::CARBON_DIOXIDE, 44.0, 1.90, 0.20, 0.30, 0.8, 0.01, 0.0, 0.0);    // CO2
    initElementPhysics(Element::HYDROGEN, 2.0, 0.09, 0.05, 0.05, 14.3, 0.18, 0.0, 0.0);          // Hydrogen (very light)
    initElementPhysics(Element::HELIUM, 4.0, 0.18, 0.05, 0.05, 5.2, 0.15, 0.0, 0.0);             // Helium (very light)
    initElementPhysics(Element::CHLORINE, 71.0, 3.20, 0.15, 0.25, 0.5, 0.008, 0.0, 0.0);         // Chlorine (heavy, toxic)
    initElementPhysics(Element::NITROGEN, 28.0, 1.25, 0.15, 0.18, 1.0, 0.025, 0.0, 0.0);         // Nitrogen (standard)
    initElementPhysics(Element::METHANE, 16.0, 0.72, 0.10, 0.12, 2.2, 0.034, 0.0, 0.0);          // Methane (light, flammable)
    initElementPhysics(Element::HYDROGEN_SULFIDE, 34.0, 1.54, 0.15, 0.20, 1.0, 0.015, 0.0, 0.0); // H2S (toxic)

    // --- Liquids ---
    initElementPhysics(Element::WATER, 18.0, 1000.0, 3.00, 1.00, 4.2, 0.60, 0.07, 0.0); // Water
    initElementPhysics(Element::OIL, 90.0, 920.0, 2.50, 20.00, 2.0, 0.15, 0.03, 0.0);   // Oil

    // --- Solids ---
    initElementPhysics(Element::DIRT, 50.0, 1500.0, 10.00, 0.00, 0.8, 0.20, 0.00, 0.6);             // Dirt
    initElementPhysics(Element::STONE, 180.0, 2400.0, 20.00, 0.00, 0.9, 1.80, 0.00, 0.8);           // Stone
    initElementPhysics(Element::GRANITE, 190.0, 2700.0, 50.00, 0.00, 0.8, 2.80, 0.00, 0.9);         // Granite
    initElementPhysics(Element::BASALT, 200.0, 3000.0, 40.00, 0.00, 0.8, 2.10, 0.00, 0.85);         // Basalt
    initElementPhysics(Element::MARBLE, 100.0, 2700.0, 35.00, 0.00, 0.9, 2.50, 0.00, 0.8);          // Marble
    initElementPhysics(Element::IRON, 56.0, 7870.0, 60.00, 0.00, 0.4, 80.00, 0.00, 0.9);            // Iron
    initElementPhysics(Element::GOLD, 197.0, 19300.0, 75.00, 0.00, 0.1, 320.00, 0.00, 0.9);         // Gold
    initElementPhysics(Element::COPPER, 63.5, 8960.0, 50.00, 0.00, 0.4, 400.00, 0.00, 0.85);        // Copper
    initElementPhysics(Element::CLAY, 100.0, 1750.0, 12.00, 0.00, 0.9, 0.25, 0.00, 0.7);            // Clay
    initElementPhysics(Element::URANIUM, 238.0, 19100.0, 150.00, 0.00, 0.1, 27.00, 0.00, 1.0);      // Uranium
    initElementPhysics(Element::NEUTRONIUM, 1000.0, 1.0e9, 1000000.00, 0.00, 0.0, 0.00, 0.00, 1.0); // Neutronium (indestructible)
    initElementPhysics(Element::ABYSSALITE, 1000.0, 1000., 300.00, 0.00, 0.0, 0.00, 0.00, 1.0);     // Abyssalite (indestructible)
    initElementPhysics(Element::COBALT, 59.0, 8900.0, 60.00, 0.00, 0.4, 16.00, 0.00, 0.85);         // Cobalt
    initElementPhysics(Element::SULFUR, 32.0, 2070.0, 8.00, 0.00, 0.7, 0.50, 0.00, 0.6);            // Sulfur
    initElementPhysics(Element::WAX, 50.0, 900.0, 2.00, 0.00, 2.5, 0.25, 0.00, 0.3);                // Wax (subsurface scattering)

    // --- Movable Solids ---
    initElementPhysics(Element::SAND, 60.0, 1600.0, 5.00, 0.00, 0.8, 0.30, 0.00, 0.5);        // Sand
    initElementPhysics(Element::SALT, 58.0, 1200.0, 4.00, 0.00, 0.9, 0.40, 0.00, 0.4);        // Salt
    initElementPhysics(Element::EMBER, 65.0, 1500.0, 5.00, 0.00, 1.0, 0.50, 0.00, 0.55);      // Ember
    initElementPhysics(Element::TERRACOTTA, 55.0, 2200.0, 7.00, 0.00, 0.9, 0.90, 0.00, 0.65); // Terracotta
    initElementPhysics(Element::SNOW, 18.0, 300.0, 1.00, 0.00, 2.1, 0.50, 0.00, 0.35);        // Snow
    initElementPhysics(Element::LAVA, 150.0, 2700.0, 8.00, 1000.00, 1.2, 2.00, 0.40, 0.0);    // Lava

    // --- Transparent Solids (Gems and Glass) ---
    initElementPhysics(Element::EMERALD, 50.0, 2750.0, 45.00, 0.00, 0.7, 0.50, 0.00, 0.8);      // Emerald
    initElementPhysics(Element::SAPPHIRE, 102.0, 4000.0, 200.00, 0.00, 0.8, 42.00, 0.00, 0.9);  // Sapphire (very hard)
    initElementPhysics(Element::DIAMOND, 12.0, 3520.0, 1000.00, 0.00, 0.5, 2000.00, 0.00, 1.0); // Diamond (hardest)
    initElementPhysics(Element::OPAL, 60.0, 2100.0, 15.00, 0.00, 0.8, 0.50, 0.00, 0.6);         // Opal (softer)
    initElementPhysics(Element::RUBY, 102.0, 4000.0, 200.00, 0.00, 0.8, 40.00, 0.00, 0.9);      // Ruby (very hard)
    initElementPhysics(Element::QUARTZ, 60.0, 2650.0, 30.00, 0.00, 0.7, 1.50, 0.00, 0.75);      // Quartz
    initElementPhysics(Element::JADE, 60.0, 3300.0, 50.00, 0.00, 0.8, 1.20, 0.00, 0.8);         // Jade
    initElementPhysics(Element::ICE, 18.0, 917.0, 9.00, 0.00, 2.1, 2.20, 0.00, 0.4);            // Ice
    initElementPhysics(Element::GLASS, 60.0, 2500.0, 70.00, 0.00, 0.8, 1.00, 0.00, 0.6);        // Glass
}

} //! namespace Map

[playground::ZEROS(Map::Element::MAX_NUM * 2)]
RWStructuredBuffer<uint32_t4> rw_element_properties;

//------------------------------------------------------------------------------------------
// Map cells 2D
//------------------------------------------------------------------------------------------

namespace Map
{
static const int2 DIM_2D = int2(MAP_SIZE);
static const int DIM_1D = MAP_SIZE * MAP_SIZE;

[ForceInline]
uint linearIndex(int2 id)
{
    //id = clamp(id, int2(0), DIM_2D - 1);
    return uint(id.x + id.y * MAP_SIZE);
}

[ForceInline]
uint linearIndexSafe(int2 id)
{
    id = clamp(id, int2(0), int2(DIM_2D) - 1);
    return uint(id.x + id.y * MAP_SIZE);
}

[ForceInline]
int2 mapIndex(uint linear_index)
{
    linear_index = min(linear_index, DIM_1D - 1);
    return int2(linear_index % MAP_SIZE, linear_index / MAP_SIZE);
}

[ForceInline]
bool insideBounds(int2 id)
{
    return all((id >= 0) & (id < DIM_2D));
}

[ForceInline]
bool outsideBounds(int2 id)
{
    return !insideBounds(id);
}

[ForceInline]
uint4 linearIndex4(int4 x, int4 y)
{
    return uint4(x + y * MAP_SIZE);
}

[ForceInline]
bool4 outsideBounds4(int4 x, int4 y)
{
    return any((x < 0) | (y < 0) | (x >= MAP_SIZE) | (y >= MAP_SIZE));
}

struct Cell
{
    static const uint MAX_ELEM = 0xFFu;
    static const uint MAX_MASS = 0xFFFu;
    static const uint MAX_TEMP = 0xFFFu;

    static const Cell VACUUM = Cell(0);
    static const Cell INVALID = Cell(Element::INVALID, 0, 0);
    static const Cell ABYSSALITE = Cell(Element::ABYSSALITE, MAX_MASS, 0);

    uint32_t packed = 0;

    __init(uint packed)
    {
        this.packed = packed;
    }

    __init(Element elem, uint mass, uint temp)
    {
        packed = uint32_t(elem.packed |
                          ((mass & MAX_MASS) << 8u) |
                          ((temp & MAX_TEMP) << 20u));
    }

    property Element elem
    {
        [ForceInline]
        get
        {
            return Element(packed & MAX_ELEM);
        }
        set
        {
            packed = (packed & ~MAX_ELEM) | (newValue & MAX_ELEM);
        }
    }
    property uint mass
    {
        [ForceInline]
        get
        {
            return (packed >> 8u) & MAX_MASS;
        }
        set
        {
            packed = (packed & ~(MAX_MASS << 8u)) | ((newValue & MAX_MASS) << 8u);
        }
    }
    property uint temp
    {
        [ForceInline]
        get
        {
            return (packed >> 20u) & MAX_TEMP;
        }
        set
        {
            packed = (packed & ~(MAX_TEMP << 20u)) | ((newValue & MAX_TEMP) << 20u);
        }
    }
    property bool is_interior
    {
        [ForceInline]
        get
        {
            return this.elem.is_interior;
        }
    }

    static Cell load(int2 id)
    {
        return Cell(rw_map_cells.Load(id));
    }

    static void store(int2 id, Cell cell)
    {
        rw_map_cells.Store(id, cell.packed);
    }

    Shading::Material shading()
    {
        var mat = this.elem.shading();

        // shift albedo & emmissive hue based of mass
        let hue_shift = cos(float(this.mass) / M_TAU) * 0.1;
        mat.albedo = Color.hueShift(mat.albedo, hue_shift);
        mat.emissive = Color.hueShift(mat.emissive, hue_shift);

        // black body emissive temperature
        mat.emissive += Color::BlackBody.lumen(this.temp);

        if (this.elem == Element::INVALID)
        {
            mat.emissive += Color::Error.anim();
        }

        return mat;
    }

    static Collision::Aabb boundingBox(uint2 map_min, uint2 map_max)
    {
        let p_min = Map.volume.objectToWorld(float3((float2(map_min) - 0.5) / Map::DIM_2D, 0.0));
        let p_max = Map.volume.objectToWorld(float3((float2(map_max) + 0.5) / Map::DIM_2D, 1.0));
        return Collision::Aabb.fromPoints(p_min, p_max);
    }
};

} //! namespace Map

[playground::BLACK(MAP_SIZE, MAP_SIZE)]
[format("r32ui")]
RWTexture2D<uint> rw_map_cells;

//------------------------------------------------------------------------------------------
// Map biomes with element distributions
//------------------------------------------------------------------------------------------

namespace Map
{

enum Biome
{
    VANILLA,
    DESERT,
    FOREST,
    MOUNTAIN,
    VOLCANIC,
    ICEFIELD,
    METALLIC,
    ANOMALY,
    GLASS_CAVE,
    ORGANIC,
    GEM_CAVE,
    CRYSTAL_CAVERN,
    SAUNA,

    _count
};

extension Biome
{
    static const uint NUM_BIOMES = uint(Biome::_count);
    static const uint NUM_CDF = 256;

    struct Sample
    {
        uint32_t value;
        float weight = 1.0;
        float random_jitter = 0.1;

        __init(uint32_t value)
        {
            this.value = value;
        }

        __init(uint32_t value, float weight, float random_jitter)
        {
            this.value = value;
            this.weight = weight;
            this.random_jitter = saturate(random_jitter);
        }
    };

    typealias Distribution = FixedSizeContainer<Sample, int(Biome::_count)>;

    [ForceInline]
    static Cell jitter(Cell cell, float2 jitter)
    {
        cell.mass = uint(clamp(int(round(cell.mass * (1.0 + jitter.x))), 0, int(Map::Cell::MAX_MASS)));
        cell.temp = uint(clamp(int(round(cell.temp * (1.0 + jitter.x))), 0, int(Map::Cell::MAX_TEMP)));
        return cell;
    }

    struct CDF // Cumulative Distribution Function
    {
        Biome biome;

        uint32_t urn[NUM_CDF];

        __init(Biome biome)
        {
            this.biome = biome;
        }

        Cell sample<RNG : Rand.IPRNG>(inout RNG rng)
        {
            let location = int2(min(int(biome), NUM_BIOMES - 1), rng.rand(NUM_CDF));
            let packed = rw_inverse_cdf.get().load(location);
            return Cell(packed);
        }

        Cell sample(uint r)
        {
            let location = int2(min(int(biome), NUM_BIOMES - 1), r % NUM_CDF);
            let packed = rw_inverse_cdf.get().load(location);
            return Cell(packed);
        }

        Cell sample(float f)
        {
            return sample(uint(floor(fract(f) * NUM_CDF)));
        }

        static uint32_t cellJitter(Sample x, float2 r)
        {
            let cell = Map::Cell(x.value);
            let rjit = x.random_jitter * (r - 0.5);
            return jitter(cell, rjit).packed;
        }

        static uint32_t[256] compute<RNG : Rand.IPRNG>(inout RNG rng, uint cdf_idx, Distribution samples, IFunc<uint32_t, Sample, float2> randomizer, out uint32_t urn[NUM_CDF])
        {
            // Calculate Total Weight and prepare normalization
            float total_weight = 0;
            for (int i = 0; i < samples.count; i++)
                total_weight += max(samples[i].weight, M_EPSILON);

            // Avoid division by zero if all weights are 0
            let oo_total_weight = (total_weight > M_EPSILON) ? rcp(total_weight) : 0.0;

            // Fill the Urn (Quantized Inverse CDF)
            // We map the cumulative weight to the integer range [0, 256].
            float accumulated_weight = 0.0;
            int current_slot = 0;

            for (int i = 0; i < samples.count; i++)
            {
                // Calculate probability of this sample
                float weight = max(samples[i].weight, M_EPSILON);

                // Move the accumulator forward
                accumulated_weight += weight;

                // Determine the target boundary for this sample in the 0..256 range.
                // Using round() ensures we minimize quantization error.
                int target_slot = int(round((accumulated_weight * oo_total_weight) * float(NUM_CDF)));

                // Clamp to ensure we don't write out of bounds
                target_slot = min(target_slot, int(NUM_CDF));

                // Fill all slots assigned to this sample
                while (current_slot < target_slot)
                {
                    urn[current_slot] = randomizer(samples[i], float2(rng.randFloat(), rng.randFloat()));
                    current_slot++;
                }
            }

            // Safety: If precision errors left any slots at the very end, fill with the last valid sample.
            while (current_slot < int(NUM_CDF))
            {
                if (samples.count > 0)
                    urn[current_slot] = samples[samples.count - 1].value;
                else
                    urn[current_slot] = {}; // Default/Vacuum if list was empty
                current_slot++;
            }

            // Shuffle the Urn (Fisher-Yates)
            for (int i = int(NUM_CDF) - 1; i > 0; i--)
            {
                // Pick a random index j where 0 <= j <= i
                let j = rng.rand(i);

                // Swap urn[i] and urn[j]
                let temp = urn[i];
                urn[i] = urn[j];
                urn[j] = temp;
            }

            return urn;
        }
    };

    void distribution(inout Distribution samples)
    {
        const uint defMass = 800;
        const uint gasMass = 1500;
        let biome = this;
        switch (biome)
        {
        case Map.Biome.VANILLA:
            samples.push(Sample(Map::Cell(Map.Element.DIRT, defMass, 285).packed, 0.25, 0.1));
            samples.push(Sample(Map::Cell(Map.Element.STONE, defMass, 285).packed, 0.20, 0.05));
            samples.push(Sample(Map::Cell(Map.Element.SAND, defMass, 285).packed, 0.15, 0.1));
            samples.push(Sample(Map::Cell(Map.Element.GRANITE, defMass, 285).packed, 0.10, 0.05));
            samples.push(Sample(Map::Cell(Map.Element.IRON, defMass, 285).packed, 0.05, 0.05));

            samples.push(Sample(Map::Cell(Map.Element.OXYGEN, gasMass, 285).packed, 0.10, 0.05));
            break;
        case Map.Biome.DESERT:
            samples.push(Sample(Map::Cell(Map.Element.SAND, defMass, 315).packed, 0.35, 0.1));
            samples.push(Sample(Map::Cell(Map.Element.TERRACOTTA, defMass, 315).packed, 0.15, 0.05));
            samples.push(Sample(Map::Cell(Map.Element.SALT, defMass, 315).packed, 0.15, 0.05));
            samples.push(Sample(Map::Cell(Map.Element.STONE, defMass, 315).packed, 0.15, 0.02));
            samples.push(Sample(Map::Cell(Map.Element.CLAY, defMass, 315).packed, 0.10, 0.1));

            samples.push(Sample(Map::Cell(Map.Element.NITROGEN, gasMass, 315).packed, 0.15, 0.1));
            samples.push(Sample(Map::Cell(Map.Element.OXYGEN, gasMass, 315).packed, 0.10, 0.1));
            samples.push(Sample(Map::Cell(Map.Element.HELIUM, gasMass, 315).packed, 0.05, 0.1));
            break;
        case Map.Biome.FOREST:
            samples.push(Sample(Map::Cell(Map.Element.DIRT, defMass, 295).packed, 0.25, 0.2));
            samples.push(Sample(Map::Cell(Map.Element.CLAY, defMass, 295).packed, 0.15, 0.1));
            samples.push(Sample(Map::Cell(Map.Element.STONE, defMass, 295).packed, 0.15, 0.02));
            samples.push(Sample(Map::Cell(Map.Element.COPPER, defMass, 295).packed, 0.10, 0.05));
            samples.push(Sample(Map::Cell(Map.Element.SAND, defMass, 295).packed, 0.05, 0.1));
            samples.push(Sample(Map::Cell(Map.Element.EMERALD, defMass, 295).packed, 0.02, 0.01));

            samples.push(Sample(Map::Cell(Map.Element.NITROGEN, gasMass, 295).packed, 0.15, 0.1));
            samples.push(Sample(Map::Cell(Map.Element.OXYGEN, gasMass, 295).packed, 0.10, 0.1));
            samples.push(Sample(Map::Cell(Map.Element.METHANE, gasMass, 295).packed, 0.05, 0.1));
            samples.push(Sample(Map::Cell(Map.Element.CARBON_DIOXIDE, gasMass, 295).packed, 0.03, 0.1));
            break;
        case Map.Biome.MOUNTAIN:
            samples.push(Sample(Map::Cell(Map.Element.STONE, defMass, 265).packed, 0.25, 0.05));
            samples.push(Sample(Map::Cell(Map.Element.GRANITE, defMass, 265).packed, 0.20, 0.02));
            samples.push(Sample(Map::Cell(Map.Element.BASALT, defMass, 265).packed, 0.15, 0.02));
            samples.push(Sample(Map::Cell(Map.Element.MARBLE, defMass, 265).packed, 0.10, 0.02));
            samples.push(Sample(Map::Cell(Map.Element.QUARTZ, defMass, 265).packed, 0.20, 0.05));
            samples.push(Sample(Map::Cell(Map.Element.COPPER, defMass, 265).packed, 0.10, 0.05));

            samples.push(Sample(Map::Cell(Map.Element.NITROGEN, gasMass, 265).packed, 0.15, 0.1));
            samples.push(Sample(Map::Cell(Map.Element.OXYGEN, gasMass, 265).packed, 0.10, 0.1));
            samples.push(Sample(Map::Cell(Map.Element.HELIUM, gasMass, 265).packed, 0.05, 0.1));
            break;
        case Map.Biome.VOLCANIC:
            samples.push(Sample(Map::Cell(Map.Element.LAVA, defMass, 1100).packed, 0.35, 0.3));
            samples.push(Sample(Map::Cell(Map.Element.BASALT, defMass, 800).packed, 0.20, 0.1));
            samples.push(Sample(Map::Cell(Map.Element.SULFUR, defMass, 400).packed, 0.15, 0.2));
            samples.push(Sample(Map::Cell(Map.Element.COBALT, defMass, 600).packed, 0.10, 0.05));
            samples.push(Sample(Map::Cell(Map.Element.EMBER, defMass, 900).packed, 0.10, 0.4));

            samples.push(Sample(Map::Cell(Map.Element.CHLORINE, gasMass, 600).packed, 0.15, 0.1));
            samples.push(Sample(Map::Cell(Map.Element.CARBON_DIOXIDE, gasMass, 400).packed, 0.10, 0.1));
            samples.push(Sample(Map::Cell(Map.Element.STEAM, gasMass, 900).packed, 0.05, 0.1));
            samples.push(Sample(Map::Cell(Map.Element.HYDROGEN_SULFIDE, gasMass, 500).packed, 0.03, 0.1));
            break;
        case Map.Biome.ICEFIELD:
            samples.push(Sample(Map::Cell(Map.Element.ICE, defMass, 240).packed, 0.55, 0.1));
            samples.push(Sample(Map::Cell(Map.Element.SNOW, 200, 240).packed, 0.30, 0.3));
            samples.push(Sample(Map::Cell(Map.Element.STONE, defMass, 240).packed, 0.10, 0.02));
            samples.push(Sample(Map::Cell(Map.Element.SALT, defMass, 240).packed, 0.05, 0.1));

            samples.push(Sample(Map::Cell(Map.Element.NITROGEN, gasMass, 240).packed, 0.15, 0.1));
            samples.push(Sample(Map::Cell(Map.Element.OXYGEN, gasMass, 240).packed, 0.09, 0.1));
            samples.push(Sample(Map::Cell(Map.Element.HELIUM, gasMass, 240).packed, 0.05, 0.1));
            break;
        case Map.Biome.METALLIC:
            samples.push(Sample(Map::Cell(Map.Element.IRON, defMass, 400).packed, 0.30, 0.05));
            samples.push(Sample(Map::Cell(Map.Element.COPPER, defMass, 400).packed, 0.25, 0.05));
            samples.push(Sample(Map::Cell(Map.Element.GOLD, defMass, 400).packed, 0.15, 0.02));
            samples.push(Sample(Map::Cell(Map.Element.COBALT, defMass, 400).packed, 0.15, 0.05));
            samples.push(Sample(Map::Cell(Map.Element.URANIUM, defMass, 400).packed, 0.15, 0.1));

            samples.push(Sample(Map::Cell(Map.Element.NITROGEN, gasMass, 400).packed, 0.15, 0.1));
            samples.push(Sample(Map::Cell(Map.Element.HELIUM, gasMass, 400).packed, 0.10, 0.1));
            break;
        case Map.Biome.ANOMALY:
            samples.push(Sample(Map::Cell(Map.Element.NEUTRONIUM, Map.Cell.MAX_MASS, 3000).packed, 0.02, 0.0));
            samples.push(Sample(Map::Cell(Map.Element.DIAMOND, defMass, 1500).packed, 0.32, 0.1));
            samples.push(Sample(Map::Cell(Map.Element.URANIUM, defMass, 1200).packed, 0.32, 0.2));
            samples.push(Sample(Map::Cell(Map.Element.MARBLE, defMass, 1000).packed, 0.34, 0.1));

            samples.push(Sample(Map::Cell(Map.Element.HELIUM, gasMass, 1000).packed, 0.15, 0.1));
            samples.push(Sample(Map::Cell(Map.Element.HYDROGEN, gasMass, 1100).packed, 0.10, 0.1));
            samples.push(Sample(Map::Cell(Map.Element.METHANE, gasMass, 1200).packed, 0.05, 0.1));
            break;
        case Map.Biome.GLASS_CAVE:
            samples.push(Sample(Map::Cell(Map.Element.GLASS, defMass, 240).packed, 0.40, 0.05));
            samples.push(Sample(Map::Cell(Map.Element.QUARTZ, defMass, 240).packed, 0.30, 0.1));
            samples.push(Sample(Map::Cell(Map.Element.ICE, defMass, 240).packed, 0.20, 0.1));
            samples.push(Sample(Map::Cell(Map.Element.MARBLE, defMass, 240).packed, 0.10, 0.05));

            samples.push(Sample(Map::Cell(Map.Element.NITROGEN, gasMass, 240).packed, 0.15, 0.1));
            samples.push(Sample(Map::Cell(Map.Element.HELIUM, gasMass, 240).packed, 0.08, 0.1));
            break;
        case Map.Biome.ORGANIC:
            samples.push(Sample(Map::Cell(Map.Element.WAX, defMass, 300).packed, 0.30, 0.2));
            samples.push(Sample(Map::Cell(Map.Element.CLAY, defMass, 300).packed, 0.30, 0.1));
            samples.push(Sample(Map::Cell(Map.Element.DIRT, defMass, 300).packed, 0.25, 0.1));
            samples.push(Sample(Map::Cell(Map.Element.SAND, defMass, 300).packed, 0.15, 0.1));

            samples.push(Sample(Map::Cell(Map.Element.NITROGEN, gasMass, 300).packed, 0.15, 0.1));
            samples.push(Sample(Map::Cell(Map.Element.OXYGEN, gasMass, 300).packed, 0.08, 0.1));
            samples.push(Sample(Map::Cell(Map.Element.METHANE, gasMass, 300).packed, 0.05, 0.1));
            break;
        case Map.Biome.GEM_CAVE:
            samples.push(Sample(Map::Cell(Map.Element.EMERALD, defMass, 300).packed, 0.25, 0.05));
            samples.push(Sample(Map::Cell(Map.Element.SAPPHIRE, defMass, 300).packed, 0.20, 0.05));
            samples.push(Sample(Map::Cell(Map.Element.RUBY, defMass, 300).packed, 0.20, 0.05));
            samples.push(Sample(Map::Cell(Map.Element.DIAMOND, defMass, 300).packed, 0.15, 0.02));
            samples.push(Sample(Map::Cell(Map.Element.OPAL, defMass, 300).packed, 0.20, 0.1));

            samples.push(Sample(Map::Cell(Map.Element.NITROGEN, gasMass, 300).packed, 0.15, 0.1));
            samples.push(Sample(Map::Cell(Map.Element.HELIUM, gasMass, 300).packed, 0.07, 0.1));
            break;
        case Map.Biome.CRYSTAL_CAVERN:
            samples.push(Sample(Map::Cell(Map.Element.QUARTZ, defMass, 250).packed, 0.40, 0.1));
            samples.push(Sample(Map::Cell(Map.Element.OPAL, defMass, 250).packed, 0.30, 0.2));
            samples.push(Sample(Map::Cell(Map.Element.JADE, defMass, 250).packed, 0.20, 0.05));
            samples.push(Sample(Map::Cell(Map.Element.MARBLE, defMass, 250).packed, 0.10, 0.02));

            samples.push(Sample(Map::Cell(Map.Element.NITROGEN, gasMass, 250).packed, 0.15, 0.1));
            samples.push(Sample(Map::Cell(Map.Element.OXYGEN, gasMass, 250).packed, 0.10, 0.1));
            break;
        case Map.Biome.SAUNA:
            samples.push(Sample(Map::Cell(Map.Element.STEAM, gasMass, 750).packed, 0.15, 0.1));
            samples.push(Sample(Map::Cell(Map.Element.OXYGEN, gasMass, 650).packed, 0.10, 0.1));
            samples.push(Sample(Map::Cell(Map.Element.CARBON_DIOXIDE, gasMass, 700).packed, 0.02, 0.1));
            break;
        default:
            return;
        }
    }

    float pdf(float2 p, float d, float2 center, float scale, uint octaves)
    {
        var mode : int = 0; // no post-process
        switch (this)
        {
        default:
        case Biome::VANILLA:
        case Biome::ORGANIC:
        case Biome::FOREST:
        case Biome::SAUNA:
            mode = 0; // default fbm
            break;

        case Biome::MOUNTAIN:
        case Biome::GEM_CAVE:
        case Biome::GLASS_CAVE:
            mode = 1; // turbulence
            break;

        case Biome::ICEFIELD:
        case Biome::DESERT:
        case Biome::VOLCANIC:
            mode = 2; // ridged
            break;

        case Biome::METALLIC:
            mode = 4; // Linf/max-norm
            break;
        case Biome::ANOMALY:
            mode = 5; // Linf/max-norm + turbulence
            break;
        case Biome::CRYSTAL_CAVERN:
            mode = 6; // Linf/max-norm + ridged
            break;
        }

        let n = Noise.fbm(p * scale, octaves, (float2 x) =>
                                              {
                                                  var n = Noise.simplex(x);
                                                  n = select((mode & 1) == 1, Noise.turbulence(n), n);
                                                  n = select((mode & 2) == 2, Noise.ridged(n), n);
                                                  return n;
                                              });

        if ((mode & 4) == 4)
        {
            d = maxNorm(p - center) / scale;
        }

        return (lerp(n * M_PHI, d, 0.4) * scale * scale); // * scale;
    }
};

} //! namespace Map

[shader("compute")]
[numthreads(Map::Biome::NUM_BIOMES + 1 /*last for biomes*/, 1, 1)]
[playground::CALL::ONCE]
[playground::CALL(1, 1, 1)]
void passBiomePrecomputeInverseCDF(uint2 dispatchThreadID: SV_DispatchThreadID)
{
    let biome = Map::Biome(dispatchThreadID.x);

    typealias Sample = Map::Biome::Sample;
    var samples : Map::Biome::Distribution;

    var rng = Rand.create(uint4(biome));

    uint32_t urn[Map::Biome::NUM_CDF];

    if (biome < Map::Biome::NUM_BIOMES)
    {
        biome.distribution(samples);

        Map::Biome::CDF.compute(rng, uint(biome), samples, (Sample x, float2 r) => Map::Biome::CDF.cellJitter(x, r), urn);
    }
    else
    {
        // Biome spawnng CDF
        samples.push(Sample(uint32_t(Map::Biome::VANILLA), 1.0, 0.1));
        samples.push(Sample(uint32_t(Map::Biome::DESERT), 0.8, 0.1));
        samples.push(Sample(uint32_t(Map::Biome::FOREST), 1.0, 0.1));
        samples.push(Sample(uint32_t(Map::Biome::MOUNTAIN), 0.9, 0.1));
        samples.push(Sample(uint32_t(Map::Biome::VOLCANIC), 0.5, 0.1));
        samples.push(Sample(uint32_t(Map::Biome::ICEFIELD), 0.5, 0.1));
        samples.push(Sample(uint32_t(Map::Biome::METALLIC), 0.6, 0.1));
        samples.push(Sample(uint32_t(Map::Biome::ANOMALY), 0.1, 0.1));
        samples.push(Sample(uint32_t(Map::Biome::GLASS_CAVE), 0.2, 0.1));
        samples.push(Sample(uint32_t(Map::Biome::ORGANIC), 0.4, 0.1));
        samples.push(Sample(uint32_t(Map::Biome::GEM_CAVE), 0.2, 0.1));
        samples.push(Sample(uint32_t(Map::Biome::CRYSTAL_CAVERN), 0.3, 0.1));
        samples.push(Sample(uint32_t(Map::Biome::SAUNA), 0.01, 0.1));

        Map::Biome::CDF.compute(rng, uint(biome), samples, (Sample x, float2 r) => x.value, urn);
    }

    let inverse_cdf = Map::Biome::rw_inverse_cdf.get();
    for (uint i = 0u; i < Map::Biome::NUM_CDF; i++)
    {
        inverse_cdf.store(uint2(uint(biome), i), urn[i]);
    }
}

//------------------------------------------------------------------------------------------
// Map procedural generation
//------------------------------------------------------------------------------------------

#define use_rand_seed 0

namespace Map
{

struct Generator
{
    Rand.IPRNG rng;

    __init(uint user_seed)
    {
#if use_rand_seed
        let program_seed = float2(Time.delta, Time.elapsed).xyxy;
#else
        let program_seed = float4(1012, 2029, 3037, 5039);
#endif
        let seed = hash(uint4(user_seed) ^ bit_cast<uint4>(program_seed));
        this.rng = Rand.create(seed);
    }

    [mutating]
    Cell sandbox(int2 id)
    {
        let p = float2(id) + 0.5 - DIM_2D * 0.5;

        var biome = Biome::_count;

        if (maxNorm(p) > MAP_SIZE / 2 - 20)
        {
            var nd = (atan2(p.y, p.x) + M_PI + M_PI / 4) / M_TAU;

            if (fract(nd * 16) < 0.95)
                return Cell::ABYSSALITE;
        }
        else if (dot2(p) < sqr(100))
        {
            biome = Biome::GEM_CAVE;
        }
        else if (abs(SDF.sdBox(p, float2(MAP_SIZE) / 5)) - 20 < 0)
        {
            // let sb = Array<Biome,4>(Biome::VOLCANIC,Biome::VANILLA,Biome::ANOMALY,Biome::FOREST);
            let sb = Array<Biome, 4>(Biome::VOLCANIC, Biome::METALLIC, Biome::ANOMALY, Biome::VANILLA);
            var nd = (atan2(p.y, p.x) + M_PI + M_PI / 4) / M_TAU;
            nd = fract(nd);

            let nb = uint(floor(nd * sb.getCount()));

            biome = sb[nb];
        }
        else if (abs(SDF.sdBox(p, M_PHI * float2(MAP_SIZE) / 5)) - 30 < 0)
        {
            let sb = Array<Biome, 8>(Biome::CRYSTAL_CAVERN, Biome::ORGANIC, Biome::GEM_CAVE, Biome::FOREST, Biome::METALLIC, Biome::ICEFIELD, Biome::GLASS_CAVE, Biome::MOUNTAIN,);
            var nd = (atan2(p.y, p.x) + M_PI + M_PI / 4) / M_TAU;
            nd = fract(nd);

            let nb = uint(floor(nd * 64));

            biome = sb[(nb/2) % sb.getCount()];
            if ((nb % 2) == 1)
                biome = Biome::SAUNA;
        }

        if (biome == Biome::_count)
            return Cell::VACUUM;

        let cdf = Map::Biome::CDF(biome);

        const float pdf_scale = 1 / (M_PI);
        let pdf = biome.pdf(p, length(p), float2(0), pdf_scale, 8);

        var cell = cdf.sample(pdf);

        return cell;
    }

    [mutating]
    Biome sector(int2 id)
    {
        let p = (float2(id) + 0.5) / DIM_2D;

        const float voro_scale = 8.0;
        const float disp_scale = voro_scale;
        // let displace = Noise::fbmD(p*disp_scale, 4, (float2 x) => Noise.valueD(x));
        let displace = Noise.valueD(p * disp_scale);

        let disp_p = p + displace.xy / (M_PHI * voro_scale);

        var voro_seed : float2;
        voro_seed.x = this.rng.randFloat();
        voro_seed.y = this.rng.randFloat();

        let voro = Noise::voronoi(disp_p * voro_scale, voro_seed, Map::Biome::NUM_BIOMES);

        return Map::Biome(int(voro.w));
    }

    [mutating]
    Optional<Cell> salt(int2 id, Biome::CDF cdf, float amount)
    {
        var local_rng = Rand.create(uint2(
                                        this.rng.randUint() ^ bit_cast<uint>(id.x),
                                        this.rng.randUint() ^ bit_cast<uint>(id.y))
                                        .xyxy);
        if (local_rng.randFloat() < amount)
        {
            return cdf.sample(local_rng);
        }
        return none;
    }

    [mutating]
    Cell terrain(int2 id)
    {
        let p = (float2(id) + 0.5) / DIM_2D;

        const float voro_scale = 8.0;
        const float disp_scale = voro_scale;
        // let displace = Noise::fbmD(p*disp_scale, 4, (float2 x) => Noise.valueD(x));
        let displace = Noise.valueD(p * disp_scale);

        let disp_p = p + displace.xy / (M_PHI * voro_scale);

        var voro_seed : float2;
        voro_seed.x = this.rng.randFloat();
        voro_seed.y = this.rng.randFloat();

        let voro = Noise::voronoi(disp_p * voro_scale, voro_seed, Map::Biome::NUM_BIOMES);
        let dual = Noise::voronoiDual(disp_p * voro_scale, voro_seed, Map::Biome::NUM_BIOMES);

        let biome = Map::Biome(int(voro.w));

        const float pdf_scale = 1 / (M_PI);
        let rot = Transform.rotate2D(this.rng.randFloat() * M_TAU);
        let pdf0 = biome.pdf(p * voro_scale, voro.x, voro.yz, pdf_scale, 8);
        let pdf1 = biome.pdf(mul(rot, p * voro_scale * M_PHI), voro.x, voro.zy, pdf_scale, 8);

        let cdf = Map::Biome::CDF(biome);
        var cell = cdf.sample(select(this.rng.randFloat() > 0.5, pdf0, pdf1));

        // cell edges
        if (voro.x < 2.5 * voro_scale / MAP_SIZE)
        {
            cell = Cell(Element::ABYSSALITE, Cell::MAX_MASS, 0);
        }
        // tunnels
        if (dual.x < 2.5 * voro_scale / MAP_SIZE)
        {
            cell = Cell::VACUUM;
        }

        if (let salty = this.salt(id, cdf, 0.4))
        {
            cell = salty;
        }
        return cell;
    }

    // use cellular automata to apply a median filter on cells
    [mutating]
    void median(int2 id)
    {
        var hist_cells : Array<Cell, 9>;
        var hist_count : Array<uint, 9>;
        var num_candidates = 0u;

        // Randomize iteration order based on cell position and iteration
        let offset = Rand.vec(this.rng, int2(1));

        for (var j = -1; j <= 1; j++)
            for (var i = -1; i <= 1; i++)
            {
                if (i == 0 && j == 0)
                    continue;
                let iteration = (j + 1) * 3 + i + 1;

                let grad = select(((int2(i, j) ^ offset) & int2(1)) == int2(0), int2(i, j), -int2(i, j));
                let p = id + grad;

                if (insideBounds(p))
                {
                    num_candidates++;

                    let cell = Cell.load(p);
                    var hist_idx = 0u;
                    for (; hist_idx < num_candidates; hist_idx++)
                    {
                        if (hist_cells[hist_idx].elem == cell.elem)
                            break;
                    }

                    let r = float2(this.rng.randFloat(), this.rng.randFloat());
                    hist_cells[hist_idx] = Map::Biome.jitter(cell, (r - 0.5) * 0.05);
                    hist_count[hist_idx]++;
                }
            }

        var winner_idx = 0;
        for (var i = 1; i < 9; i++)
        {
            if (hist_count[i] > hist_count[winner_idx])
                winner_idx = i;
        }

        GroupMemoryBarrierWithGroupSync();

        let cell = Cell.load(id);
        if (hist_cells[winner_idx].elem != cell.elem)
        {
            if (this.rng.randFloat() <= 0.5)
                Cell.store(id, hist_cells[winner_idx]);
        }
    }

    [mutating]
    Optional<Cell> landmarks(int2 id)
    {
        static const uint NUM_LANDMARKS = 3;
        let p = (float2(id) + 0.5) / DIM_2D;

        // Tweak: Slightly non-uniform scale breaks grid repetition better
        const float voro_scale = 7.333;

        var voro_seed : float2;
        voro_seed.x = this.rng.randFloat();
        voro_seed.y = this.rng.randFloat();

        let p_voro = p * voro_scale;
        let voro = Noise::voronoi(p_voro, voro_seed, NUM_LANDMARKS);

        // Enhancement: Hash the seed to prevent visual artifacts if voro.yz is similar
        this.rng = Rand.create(hash(bit_cast<uint2>(voro.yz).xyxy));

        if (this.rng.randFloat() < 0.5)
            return none;

        var radius = this.rng.rand(16.0, 32.0);
        radius *= voro_scale / MAP_SIZE;

        if (distance(p_voro, voro.yz) > radius)
            return none;

        // Use Vectors for averaging, not Angles (Fixes wrap-around bug)
        var light_dir_sum = float2(0);
        var light_end = float3(0);

        var sdf = float3(float.maxValue);

        for (int iter = 0; iter < NUM_LANDMARKS; iter++)
        {
            let radius_local = radius * this.rng.rand(0.3, 0.4);
            let rot = Transform.rotate2D(this.rng.rand(-M_PI, M_PI));

            // Jitter the shapes slightly away from center for better spread
            let offset_dir = Rand.vec(this.rng, float2(1)) - 0.5;
            let c_local = voro.yz + offset_dir * radius * 0.8;
            let p_local = mul(rot, p_voro - c_local);

            // Accumulate vectors pointing FROM shape TO center
            // This pushes the light towards the "open" side
            light_dir_sum += normalize(voro.yz - c_local) * radius_local;

            // Accumulate weighted centroid for the target
            light_end += float3(c_local, 1) * radius_local;

            var local_sdf : float;
            switch (this.rng.rand(2))
            {
            default:
            case 0:
                local_sdf = SDF.sdEquilateralTriangle(p_local, radius_local);
                break;
            case 1:
                local_sdf = SDF.sdRoundedX(p_local, radius_local, radius_local * 0.2);
                break;
            case 2:
                local_sdf = SDF.sdChamferBox(p_local, float2(radius_local), 0.3 * radius_local);
                break;
            }

            if (this.rng.randFloat() < 0.5)
            {
                local_sdf = abs(local_sdf) - this.rng.rand(0.1, 0.2) * radius_local;
            }

            let biome = Map::Biome(this.rng.rand(Map::Biome::NUM_BIOMES - 1));

            if (local_sdf < sdf.x)
            {
                sdf = float3(local_sdf, int(biome) + 0.5, sdf.z);
            }
            sdf.z = select(iter == 0, local_sdf, SDF.opSmoothUnion(sdf.z, local_sdf, 0.15 * radius_local));
        }

        // Finalize Light Positions
        light_end.xy /= light_end.z; // Centroid

        // Normalize the vector sum to get the average direction
        // Safe normalize in case shapes are perfectly symmetric
        float2 best_light_dir = normalizeSafe(light_dir_sum, float2(1, 0));

        // Position light on the rim
        var light_start = voro.yz + best_light_dir * radius * 0.9;

        // --- GENERATE CELL ---
        let biome = Map::Biome(int(floor(sdf.y)));
        let cdf = Map::Biome::CDF(biome);

        var cell : Optional<Cell> = none;

        // 1. Shapes
        if (sdf.x < 0)
        {
            cell = cdf.sample(sdf.x);
        }
        // 2. The "Halo" (Main Cavern)
        // Using smooth subtraction for organic walls?
        // Here we just keep your hard carve which works for crystals.
        else if (SDF.opSmoothUnion(sdf.z, SDF.sdSegment(p_voro, light_start, light_end.xy), 0.15 * radius) - radius * 0.3 < 0)
        {
            cell = Cell::VACUUM;
        }

        // --- LIGHT SPAWNING ---

        // Convert to Pixel Coordinates for strict equality check
        float2 px_light_start = light_start * DIM_2D / voro_scale;
        float2 px_light_end = light_end.xy * DIM_2D / voro_scale;

        if (all(int2(floor(px_light_start)) == id))
        {
            // Spawn the actual light entity
            if (let light_idx = Shading::LightManager.allocate())
            {
                let light = Shading::Light(
                    Map.mapToWorld(float3(px_light_start, Map::volume.extent.z * 0.8)),
                    0.5,                                                               // physical radius
                    Map.mapToWorld(float3(px_light_end, Map::volume.extent.z * -0.5)), // Look down
                    this.rng.rand(0.1, 0.9),                                           // cone width
                    Color::Pastel.rgb(this.rng.randFloat()),
                    300, // luminance
                );
                Shading::rw_light_allocation.get().store(light_idx, light.pack().packed);
            }
        }

        return cell;
    }
};

}; //! namespace Map

[shader("compute")]
[numthreads(16, 16, 1)]
[playground::CALL::ONCE]
[playground::CALL::SIZE_OF("rw_map_cells")]
void passMapGenerator0(uint2 dispatchThreadID: SV_DispatchThreadID)
{
    let id = int2(dispatchThreadID.xy);

    var generator = Map::Generator(0);
#if 0
    let cell = generator.terrain(id);
#else
    let cell = generator.sandbox(id);
#endif

    if (Map.insideBounds(id))
    {
        Map::Cell.store(dispatchThreadID.xy, cell);
    }
}

#if 1
[shader("compute")]
[numthreads(16, 16, 1)]
[playground::CALL::ONCE]
[playground::CALL::SIZE_OF("rw_map_cells")]
void passMapGenerator1(uint2 dispatchThreadID: SV_DispatchThreadID)
{
    let id = int2(dispatchThreadID.xy);

    var generator = Map::Generator(hash(dispatchThreadID.xyxy).x);
    for (int iter = 0; iter < 21; iter++)
    {
        GroupMemoryBarrierWithGroupSync();

        generator.median(id);
    }

    GroupMemoryBarrierWithGroupSync();

    generator = Map::Generator(1337);
    if (let cell = generator.landmarks(id))
    {
        Map::Cell.store(id, cell);
    }
}
#endif

//------------------------------------------------------------------------------------------
// Visibility bit masks 2D
//------------------------------------------------------------------------------------------

namespace Map
{

#define use_visibilitycache_debug 0 //%_NOCOMMIT%

#if !use_visibilitycache_debug
#define if_visibilitycache_debug(_CODE)
#else
#define if_visibilitycache_debug(_CODE) _CODE
struct VisibilityDebug
{
    Canvas.Context canvas = {};
    float ddxy_len = 1.0;
    bool enabled = false;

    [ForceInline]
    [mutating]
    void enableDebugDraw(float2 origin, float2 origin_ddx, float2 origin_ddy)
    {
        this.canvas = Canvas.Context();
        this.canvas.moveTo(origin);
        this.ddxy_len = ddxyLen(origin_ddx, origin_ddy);
        this.enabled = true;
    }

    [ForceInline]
    [mutating]
    void onSamplePosition(uint2 map_pos, uint level, bool result)
    {
        if (this.enabled)
        {
            let block_map_radius = level;

            this.canvas.style.stroke_width = this.ddxy_len;
            this.canvas.style.glow_width = 0.0;
            this.canvas.style.stroke = float4(Color::Pastel.rgb(level * M_PHI), 0.8);
            this.canvas.style.fill = float4(this.canvas.style.stroke.rgb, 0.5) * select(result, 1.0, 0.3);
            this.canvas.drawEquilateralTriangle(map_pos + 0.5, block_map_radius, this.ddxy_len);
        }
    }

    [ForceInline]
    [mutating]
    void onSampleBlock(uint2 block_pos, uint level, Visibility block)
    {
        if (this.enabled)
        {
            let block_map_size = Visibility::BLOCK << level;
            let block_map_center = (block_pos + 0.5) * block_map_size;
            let block_map_extent = block_map_size * 0.5;

            this.canvas.style.stroke_width = 0.0;
            this.canvas.style.glow_width = 0.0;
            this.canvas.style.stroke = float4(Color::Pastel.rgb(level * M_PHI), 0.8);
            this.canvas.style.fill = float4(this.canvas.style.stroke.rgb, 0.1);
            this.canvas.drawBox(block_map_center, block_map_extent, this.ddxy_len);
        }
    }
};
static VisibilityDebug g_vis_debug;
#endif

struct Visibility
{
    static const uint BLOCK_X = 8;
    static const uint BLOCK_Y = 8;
    static const uint2 BLOCK = uint2(BLOCK_X, BLOCK_Y);
    static const uint NUM_WORDS = (BLOCK_X * BLOCK_Y + 31) / 32;

    typealias Mask = Bitmask<NUM_WORDS>;

    Mask mask = Mask(false);

    [UnscopedEnum]
    enum Containment
    {
        Miss    = 0,
        Hit     = 1,
        Skip    = 2,
    };

    [ForceInline]
    Containment contains(uint2 rel_pos)
    {
        let bit = blockLinearIndex(rel_pos);
        return select(this.mask.empty, Skip,
            Containment(int(this.mask.contains(bit))) );
    }

    [ForceInline]
    static uint blockLinearIndex(uint2 rel_pos)
    {
        return rel_pos.x + rel_pos.y * BLOCK_X;
    }

    static const uint LVL0_ROW_PITCH = MAP_SIZE / Visibility::BLOCK_X;
    static const uint LVL0_COL_PITCH = MAP_SIZE / Visibility::BLOCK_Y;

    static const uint NUM_LEVELS = 3;

    [ForceInline]
    static Containment contains(uint2 map_pos, uint level)
    {
        let level_pos = min(map_pos >> level, (DIM_2D >> level) - 1);
        let block_pos = level_pos / BLOCK;

        var temp: Visibility;
        temp.mask.bits = rw_map_visiblity::get().load(block_pos, level);

        let block_bit = blockLinearIndex(level_pos % BLOCK);

        return select(temp.mask.empty, Skip,
            Containment(int(temp.mask.contains(block_bit))) );
    }

    // --------------------------------------------------------
    // Mip Generation: Merges 4 children (16x16px) into 1 parent (8x8 bits)
    // --------------------------------------------------------
    static Visibility mipMax(uint2 c00, uint2 c10, uint2 c01, uint2 c11)
    {
        // Helper: Downsample 8x8 (64-bit) -> 4x4 (16-bit packed)
        // Returns a 16-bit uint where bits 0-3 are Row 0, 4-7 Row 1, etc.
        let downsample8x8 = (uint2 b) =>
        {
            // 1. Horizontal Reduce: Combine neighbor columns (0|1 -> 0, 2|3 -> 1...)
            // x | (x >> 1) combines (2n) and (2n+1).
            // We mask 0x5555... to keep even bits (which now hold the union).
            let h_low  = (b.x | (b.x >> 1u)) & 0x55555555u;
            let h_high = (b.y | (b.y >> 1u)) & 0x55555555u;

            // 2. Vertical Reduce: Combine neighbor rows (0|1 -> 0, 2|3 -> 1...)
            // Shift by 8 aligns Row 1 with Row 0.
            let v_low  = (h_low  | (h_low  >> 8u)) & 0x00FF00FFu;
            let v_high = (h_high | (h_high >> 8u)) & 0x00FF00FFu;

            // 3. Compact: Extract the sparse valid bits (0, 2, 4, 6) into (0, 1, 2, 3)
            let pack_nibble = (uint v) =>
            {
                return ((v >> 0u) & 1u) |
                       ((v >> 2u) & 1u) << 1u |
                       ((v >> 4u) & 1u) << 2u |
                       ((v >> 6u) & 1u) << 3u;
            };

            // v_low contains merged rows 0+1 (Byte 0) and 2+3 (Byte 2)
            let r0 = pack_nibble(v_low);
            let r1 = pack_nibble(v_low >> 16u);

            // v_high contains merged rows 4+5 (Byte 0) and 6+7 (Byte 2)
            let r2 = pack_nibble(v_high);
            let r3 = pack_nibble(v_high >> 16u);

            // Pack into 16-bit integer (Row Major)
            return r0 | (r1 << 4u) | (r2 << 8u) | (r3 << 12u);
        };

        let q00 = downsample8x8(c00); // Top-Left
        let q10 = downsample8x8(c10); // Top-Right
        let q01 = downsample8x8(c01); // Bottom-Left
        let q11 = downsample8x8(c11); // Bottom-Right

        Visibility res;

        // 4. Merge Quadrants: Construct 8x8 bitmask
        // Low Word (Rows 0-3) = [Right Nibble] [Left Nibble] per row
        let row0 = (q00 & 0xFu) | ((q10 & 0xFu) << 4u);
        let row1 = ((q00 >> 4u) & 0xFu) | ((q10 >> 4u) & 0xFu) << 4u;
        let row2 = ((q00 >> 8u) & 0xFu) | ((q10 >> 8u) & 0xFu) << 4u;
        let row3 = ((q00 >> 12u) & 0xFu) | ((q10 >> 12u) & 0xFu) << 4u;

        // High Word (Rows 4-7)
        let row4 = (q01 & 0xFu) | ((q11 & 0xFu) << 4u);
        let row5 = ((q01 >> 4u) & 0xFu) | ((q11 >> 4u) & 0xFu) << 4u;
        let row6 = ((q01 >> 8u) & 0xFu) | ((q11 >> 8u) & 0xFu) << 4u;
        let row7 = ((q01 >> 12u) & 0xFu) | ((q11 >> 12u) & 0xFu) << 4u;

        res.mask.bits.x = row0 | (row1 << 8u) | (row2 << 16u) | (row3 << 24u);
        res.mask.bits.y = row4 | (row5 << 8u) | (row6 << 16u) | (row7 << 24u);

        return res;
    }

};

} //! namespace Map

[shader("compute")]
[numthreads(16, 16, 1)]
[playground::CALL(MAP_SIZE/2, MAP_SIZE/2, 1)]
void passMapVisibility(uint2 groupThreadID: SV_GroupThreadID, uint2 groupID: SV_GroupID)
{
    using namespace Map;

    // LDS: Stores Mip 0 (32x32 pixels = 4x4 blocks of 8x8)
    // Using Atomic<uint> to safely scatter bits from multiple threads.
    static groupshared Atomic<uint> s_vis_low[4][4];
    static groupshared Atomic<uint> s_vis_high[4][4];

    // 1. Initialize LDS to 0
    // We only need the first 16 threads (4x4) to clear the blocks.
    if (groupThreadID.x < 4 && groupThreadID.y < 4)
    {
        s_vis_low[groupThreadID.x][groupThreadID.y].store(0, MemoryOrder::Relaxed);
        s_vis_high[groupThreadID.x][groupThreadID.y].store(0, MemoryOrder::Relaxed);
    }
    GroupMemoryBarrierWithGroupSync();

    // 2. Scatter Pixels (Mip 0 Construction)
    // Group covers 32x32 pixels. Threads 16x16. Each thread does 4 pixels.
    let group_pixel_origin = groupID * 32u;

    [unroll]
    for (uint i = 0; i < 4; ++i)
    {
        // Offsets cover the 4 quadrants: (0,0), (16,0), (0,16), (16,16)
        let offset = uint2((i & 1u) * 16u, (i >> 1u) * 16u);
        let local_pos = groupThreadID.xy + offset;

        // Load Cell Data (Boundary Check implicit in Cell.load usually, but good to be safe)
        let cell = Cell.load(group_pixel_origin + local_pos);

        if (cell.is_interior)
        {
            // Determine which 8x8 block (0..3)
            let block_x = local_pos.x / Visibility::BLOCK_X;
            let block_y = local_pos.y / Visibility::BLOCK_Y;

            // Determine bit index within the block (0..63)
            // x + y * 8
            let in_block_pos = local_pos % Map::Visibility::BLOCK;
            let lin_idx = Map::Visibility::blockLinearIndex(in_block_pos);

            // Calculate bitmask
            let bit = 1u << (lin_idx % 32u);

            // Atomic OR into shared memory
            // Indices 0-31 -> Low Word (Row 0-3)
            // Indices 32-63 -> High Word (Row 4-7)
            if (lin_idx < 32u)
                s_vis_low[block_x][block_y].or(bit);
            else
                s_vis_high[block_x][block_y].or(bit);
        }
    }

    // Ensure all scatters are done before reading
    GroupMemoryBarrierWithGroupSync();

    // 3. Store Mip 0 (Global Memory)
    // 4x4 blocks = 16 threads needed.
    if (groupThreadID.x < 4 && groupThreadID.y < 4)
    {
        let bx = groupThreadID.x;
        let by = groupThreadID.y;

        let low = s_vis_low[bx][by].load(MemoryOrder::Relaxed);
        let high = s_vis_high[bx][by].load(MemoryOrder::Relaxed);
        let val = uint2(low, high);

        // Global Block Pos = GroupOffset + LocalBlockOffset
        uint2 global_block_pos = (groupID * 4u) + uint2(bx, by);

        Visibility::rw_map_visiblity::get().store(global_block_pos, 0u, val);
    }

    // Ensure Mip 0 stores are visible/safe before reusing LDS?
    // Actually we just need to sync before the next step which READS from LDS.
    GroupMemoryBarrierWithGroupSync();

    // 4. Reduce & Store Mip 1 (From 32x32 -> 16x16 bits)
    // We produce 2x2 blocks of output (from the 4x4 input blocks).
    // Threads 0,0 to 1,1 active.
    if (groupThreadID.x < 2u && groupThreadID.y < 2u)
    {
        // Source indices (stride 2)
        let bx = groupThreadID.x * 2u;
        let by = groupThreadID.y * 2u;

        // Load 4 neighbors (2x2) from LDS
        let c00 = uint2(s_vis_low[bx][by].load(MemoryOrder::Relaxed), s_vis_high[bx][by].load(MemoryOrder::Relaxed));
        let c10 = uint2(s_vis_low[bx + 1u][by].load(MemoryOrder::Relaxed), s_vis_high[bx + 1u][by].load(MemoryOrder::Relaxed));
        let c01 = uint2(s_vis_low[bx][by + 1u].load(MemoryOrder::Relaxed), s_vis_high[bx][by + 1u].load(MemoryOrder::Relaxed));
        let c11 = uint2(s_vis_low[bx + 1u][by + 1u].load(MemoryOrder::Relaxed), s_vis_high[bx + 1u][by + 1u].load(MemoryOrder::Relaxed));

        let m1 = Visibility.mipMax(c00, c10, c01, c11);
        let m1_bits = m1.mask.bits;

        // Store Mip 1 Global
        let mip1_pos = (groupID * 2u) + groupThreadID.xy;
        Visibility::rw_map_visiblity::get().store(mip1_pos, 1u, m1_bits);

        // WRITE BACK TO LDS for Mip 2 generation.
        // We reuse the first 2x2 slots of s_vis arrays.
        // NOTE: This is safe because we are inside a barrier-protected region where only these threads are active
        // and we have finished reading the input data (c00...c11).
        s_vis_low[groupThreadID.x][groupThreadID.y].store(m1_bits.x, MemoryOrder::Relaxed);
        s_vis_high[groupThreadID.x][groupThreadID.y].store(m1_bits.y, MemoryOrder::Relaxed);
    }

    // Wait for Mip 1 LDS writes to finish
    GroupMemoryBarrierWithGroupSync();

    // 5. Reduce & Store Mip 2 (From 16x16 -> 8x8 bits)
    // 1 thread produces 1 block.
    if (all(groupThreadID == 0u))
    {
        // Load the 4 results from Mip 1 (now stored in 0,0 to 1,1 of LDS)
        let c00 = uint2(s_vis_low[0u][0u].load(MemoryOrder::Relaxed), s_vis_high[0u][0u].load(MemoryOrder::Relaxed));
        let c10 = uint2(s_vis_low[1u][0u].load(MemoryOrder::Relaxed), s_vis_high[1u][0u].load(MemoryOrder::Relaxed));
        let c01 = uint2(s_vis_low[0u][1u].load(MemoryOrder::Relaxed), s_vis_high[0u][1u].load(MemoryOrder::Relaxed));
        let c11 = uint2(s_vis_low[1u][1u].load(MemoryOrder::Relaxed), s_vis_high[1u][1u].load(MemoryOrder::Relaxed));

        let m2 = Visibility.mipMax(c00, c10, c01, c11);

        // Store Mip 2 Global
        Visibility::rw_map_visiblity::get().store(groupID, 2u, m2.mask.bits);
    }
}

[playground::BLACK(MAP_SIZE, MAP_SIZE)]
[format("r32ui")]
RWTexture2D<uint> rw_map_skip_dist;

[shader("compute")]
[numthreads(16, 16, 1)]
[playground::CALL(MAP_SIZE, MAP_SIZE, 1)]
void passMapSkipDist(uint2 dispatchThreadID: SV_DispatchThreadID)
{
    using namespace Map;
    let pos = dispatchThreadID.xy;
    let level0 = Visibility.contains(pos, 0u);

    if (level0 == Visibility::Hit)
    {
        rw_map_skip_dist.Store(pos, 0u);
        return;
    }

    let level2 = Visibility.contains(pos, 2u);
    let level1 = Visibility.contains(pos, 1u);

    uint skip_dist = 1u;
    skip_dist = select(level1 == Visibility::Miss, 2u, skip_dist);
    skip_dist = select(level2 == Visibility::Miss, 4u, skip_dist);
    skip_dist = select(level0 == Visibility::Skip, 8u, skip_dist);
    skip_dist = select(level1 == Visibility::Skip, 16u, skip_dist);
    skip_dist = select(level2 == Visibility::Skip, 32u, skip_dist);

    rw_map_skip_dist.Store(pos, uint(skip_dist));
}

//==========================================================================================
//------------------------------------------------------------------------------------------
//==========================================================================================

namespace Voxel
{
    struct DDA
    {
        int2 pos;     // Current integer grid position
        int2 step;    // Step direction (-1 or 1)
        float2 side;  // Accumulated fraction (0-1) to next boundary
        float2 delta; // Fraction (0-1) per grid step
        float t;      // Current normalized t (0-1)
        int axis;     // Last axis stepped (0 for X, 1 for Y)

        __init(float2 p0, float2 p1)
        {
            let dir = p1 - p0;
            let is_neg = dir < 0.0;
            this.step = select(is_neg, int2(-1), int2(1));
            this.delta = abs(1.0 / (dir + M_EPSILON));

            let grid_pos = floor(p0);
            this.pos = int2(grid_pos);

            // Initial distance to edge
            let dist_to_edge = select(is_neg, p0 - grid_pos, grid_pos + 1.0 - p0);
            this.side = dist_to_edge * this.delta;

            this.t = 0.0;
            this.axis = -1;
        }

        // Standard single-pixel step
        [mutating]
        void next()
        {
            let step_mask = this.side.x < this.side.y;
            this.axis = select(step_mask, 0, 1);
            this.t = select(step_mask, this.side.x, this.side.y);
            this.side += select(step_mask, float2(this.delta.x, 0), float2(0, this.delta.y));
            this.pos += select(step_mask, int2(this.step.x, 0), int2(0, this.step.y));
        }

        // Optimization: Skip to the next block boundary
        [mutating]
        void skip(uint2 block_size, float2 start_pos, float2 dir_inv)
        {
            // 1. Calculate the next block boundary coordinate
            let mask = int2(block_size) - 1;
            let next_bound = select(this.step > 0,
                                    (this.pos | mask) + 1,
                                    (this.pos & ~mask));

            // 2. Calculate time to hit boundaries
            let t_bounds = (float2(next_bound) - start_pos) * dir_inv;
            let skip_x = t_bounds.x < t_bounds.y;

            // 3. Update t and axis
            this.t = min(t_bounds.x, t_bounds.y);
            this.axis = select(skip_x, 0, 1);

            // 4. Update integer position
            // For the non-hit axis, we use standard floor of the current hit position
            let hit_pos = start_pos + this.t / dir_inv;
            let floor_pos = int2(floor(hit_pos));

            // For the hit axis, we snap to the boundary + step nudge
            // This is the "Zero-Branch Nudge" that prevents infinite loops
            let target_coord = next_bound + select(this.step > 0, 0, -1);
            this.pos = select(skip_x,
                            int2(target_coord.x, floor_pos.y),
                            int2(floor_pos.x, target_coord.y));

            // 5. Re-sync 'side'
            // Side is the 't' at which the ray will hit the NEXT integer boundary
            // We calculate this from the new snapped 'pos'
            let next_pixel_bound = float2(this.pos + select(this.step > 0, 1, 0));
            this.side = (next_pixel_bound - start_pos) * dir_inv;
        }

        float3 normal()
        {
            let n = float2(select(this.axis == 0, -float(this.step.x), 0.0),
                        select(this.axis == 1, -float(this.step.y), 0.0));
            return float3(n, 0);
        }
    };

#if 1
    Collision::HitResult trace(float2 map_start, float2 map_end)
    {
        using namespace Map;

        let dir = map_end - map_start;

        let step = sign(dir);
        let dir_inv = rcp(select(abs(dir) < M_EPSILON, step * M_EPSILON, dir));

        var t = 0.0;
        var pos = int2(floor(map_start));
        var axis = -1;

        [loop]
        for (uint steps = 0; t < 1.0 && steps < 128; steps++)
        {
            //if (Map.outsideBounds(pos)) break;

            let skip_dist = rw_map_skip_dist.Load(pos);
            if (skip_dist == 0u)
            {
                var res: Collision::HitResult;
                res.t.x = t;
                res.normal = float3(
                    select(axis == 0, -float(step.x), 0.0),
                    select(axis == 1, -float(step.y), 0.0),
                    float(axis == -1) );
                res.id = int(Map::linearIndex(pos));
                //dda.next();
                res.t.y = t;
                return res;
            }

            // 1. Calculate the next block boundary coordinate
            let mask = skip_dist - 1u;
            let next_bound = select(step > 0, (pos | mask) + 1, (pos & ~mask));

            // 2. Calculate time to hit boundaries
            let t_bounds = (float2(next_bound) - map_start) * dir_inv;
            let skip_x = t_bounds.x < t_bounds.y;

            // 3. Update t and axis
            t = min(t_bounds.x, t_bounds.y);
            axis = select(skip_x, 0, 1);

            // 4. Update integer position
            // For the non-hit axis, we use standard floor of the current hit position
            let hit_pos = map_start + t * dir;
            let floor_pos = int2(floor(hit_pos));

            // For the hit axis, we snap to the boundary + step nudge
            // This is the "Zero-Branch Nudge" that prevents infinite loops
            let target_coord = next_bound + select(step > 0, 0, -1);
            pos = select(skip_x,
                            int2(target_coord.x, floor_pos.y),
                            int2(floor_pos.x, target_coord.y));
        }

        return Collision::HitResult();
    }
#else
    Collision::HitResult trace(float2 map_start, float2 map_end)
    {
        using namespace Map;
        static const uint MAX_STEPS = MAP_SIZE;
        var dda = DDA(map_start, map_end);

        let dir = map_end - map_start;
        let dir_inv = 1.0 / (select(abs(dir) < M_EPSILON, sign(dir) * M_EPSILON, dir));

        // Starting cell check
        if (Visibility.contains(uint2(dda.pos), 0) == Visibility.Hit)
        {
            var result : Collision::HitResult;
            result.t.x = 0.0;
            result.normal = float3(0, 0, -1);
            result.id = int(Map::linearIndex(uint2(dda.pos)));
            dda.next();
            result.t.y = dda.t;
            return result;
        }

        static const uint2 SKIP_L2 = Map::Visibility::BLOCK * 4;
        static const uint2 SKIP_L1 = Map::Visibility::BLOCK * 2;

        for (uint steps = 0; dda.t <= 1.0 && steps < MAX_STEPS; steps++)
        {
            if (outsideBounds(dda.pos))
                break;

            if_visibilitycache_debug(g_vis_debug.canvas.style.glow = float4(Color.Temperature.rgb(max(dda.t, (steps + 0.5) / MAX_STEPS)), 0.2);)

            // Hierarchical skip: each level guards the next
            if (Visibility.isBlockEmpty(dda.pos, 2))
            {
                dda.skip(SKIP_L2, map_start, dir_inv);
            }
            else if (Visibility.isBlockEmpty(dda.pos, 1))
            {
                dda.skip(SKIP_L1, map_start, dir_inv);
            }
            else if (Visibility.contains(dda.pos, 2) == Visibility.Miss)
            {
                dda.skip(uint2(4), map_start, dir_inv);
            }
            else if (Visibility.isBlockEmpty(dda.pos, 0))
            {
                dda.skip(Map::Visibility::BLOCK, map_start, dir_inv);
            }
            else if (Visibility.contains(dda.pos, 1) == Visibility.Miss)
            {
                dda.skip(uint2(2), map_start, dir_inv);
            }
            else if (Visibility.contains(dda.pos, 0) == Visibility.Hit)
            {
                // Hit! Build result inline
                var result : Collision::HitResult;
                result.t.x = dda.t;
                result.normal = dda.normal();
                result.id = int(Map::linearIndex(dda.pos));
                dda.next();
                result.t.y = dda.t;
                return result;
            }
            else
            {
                dda.next();
            }
        }

        return Collision::HitResult();
    }
#endif

    struct HitResult4
    {
        float4 t;
        float4x3 normal; // Each row or column represents a normal
        int4 id;
    };

    HitResult4 trace4(float2x4 map_starts, float2x4 map_ends)
    {
        using namespace Map;

        let dir_x = map_ends[0] - map_starts[0];
        let dir_y = map_ends[1] - map_starts[1];

        // Use fast_sign or custom sign to avoid branching
        let step_x = select(dir_x >= 0, int4(1), int4(-1));
        let step_y = select(dir_y >= 0, int4(1), int4(-1));

        let dir_inv_x = rcp(select(abs(dir_x) < M_EPSILON, float4(M_EPSILON) * float4(step_x), dir_x));
        let dir_inv_y = rcp(select(abs(dir_y) < M_EPSILON, float4(M_EPSILON) * float4(step_y), dir_y));

        var t = float4(0.0);
        var pos_x = int4(floor(map_starts[0]));
        var pos_y = int4(floor(map_starts[1]));
        var axis = int4(-1);

        var hit_mask = bool4(false);
        var res = HitResult4();
        res.id = int4(-1);

        [loop]
        for (uint steps = 0; steps < 128; steps++)
        {
            // 1. Efficient Termination Check
            let oob = Map.outsideBounds4(pos_x, pos_y);
            let active = !(hit_mask | (t >= 1.0) | oob);
            if (!any(active)) break;

            // 2. Memory Access Optimization
            // If rays are coherent (common), this load is prioritized by the L1 cache
            let skip_dist = uint4(
                active[0] ? rw_map_skip_dist.Load(int2(pos_x[0], pos_y[0])) : 1u,
                active[1] ? rw_map_skip_dist.Load(int2(pos_x[1], pos_y[1])) : 1u,
                active[2] ? rw_map_skip_dist.Load(int2(pos_x[2], pos_y[2])) : 1u,
                active[3] ? rw_map_skip_dist.Load(int2(pos_x[3], pos_y[3])) : 1u
            );

            // 3. Branchless Hit Recording
            let found_hit = active && (skip_dist == 0u);

            // Update results only for first-time hits using bitmasking
            let first_hit = found_hit && !hit_mask;
            if (any(first_hit)) {
                res.t = select(first_hit, t, res.t);
                res.id = select(first_hit, int4(Map::linearIndex4(pos_x, pos_y)), res.id);
                // We store axis now, reconstruct normals at the very end (once) to save registers
                axis = select(first_hit, axis, axis);
                hit_mask = hit_mask | first_hit;
            }

            // 4. Optimized DDA Step
            let mask = skip_dist - 1u;
            let next_bound_x = select(step_x > 0, (uint4(pos_x) | mask) + 1u, (uint4(pos_x) & ~mask));
            let next_bound_y = select(step_y > 0, (uint4(pos_y) | mask) + 1u, (uint4(pos_y) & ~mask));

            let t_bound_x = (float4(next_bound_x) - map_starts[0]) * dir_inv_x;
            let t_bound_y = (float4(next_bound_y) - map_starts[1]) * dir_inv_y;

            let is_skip_x = t_bound_x < t_bound_y;

            // Update state branchlessly
            let next_t = min(t_bound_x, t_bound_y);
            t = select(active, next_t, t);

            // Instead of floor(map_start + t * dir), use the exact boundary logic
            // This is significantly faster on GPUs as it avoids FMA + Floor pipelines
            let side_step_x = int4(next_bound_x) + select(step_x > 0, int4(0), int4(-1));
            let side_step_y = int4(next_bound_y) + select(step_y > 0, int4(0), int4(-1));

            // Predict next integer positions
            let floor_x = int4(floor(map_starts[0] + next_t * dir_x));
            let floor_y = int4(floor(map_starts[1] + next_t * dir_y));

            pos_x = select(active, select(is_skip_x, side_step_x, floor_x), pos_x);
            pos_y = select(active, select(is_skip_x, floor_y, side_step_y), pos_y);
            axis = select(active, select(is_skip_x, int4(0), int4(1)), axis);
        }

        // Final Normal Reconstruction (Move outside the loop to save 12 float registers inside the loop)
        [unroll]
        for(uint i=0; i<4; i++) {
            res.normal[i] = float3(
                select(axis[i] == 0, -float(step_x[i]), 0.0),
                select(axis[i] == 1, -float(step_y[i]), 0.0),
                select(axis[i] == -1, 1.0, 0.0)
            );
        }

        return res;
    }

    [ForceInline]
    bool occlusion(float3 pos)
    {
        let id = uint2(floor(pos.xy));
        return (pos.z > Map.volume.extent.z * 2.0 ||
                Map::Visibility.contains(id, 0) == Map::Visibility::Hit );
    }

    [ForceInline]
    float4 occlusion4(float3 p0, float3 p1, float3 p2, float3 p3)
    {
        let collision = bool4(
            Map::Visibility.contains(uint2(floor(p0.xy)), 0) == Map::Visibility::Hit,
            Map::Visibility.contains(uint2(floor(p1.xy)), 0) == Map::Visibility::Hit,
            Map::Visibility.contains(uint2(floor(p2.xy)), 0) == Map::Visibility::Hit,
            Map::Visibility.contains(uint2(floor(p3.xy)), 0) == Map::Visibility::Hit );
        return select(collision | (float4(p0.z, p1.z, p2.z, p3.z) > Map.volume.extent.z * 2.0), float4(1), float4(0));
    }

    [ForceInline]
    float vertexAo(float2 side, float corner)
    {
        return (side.x + side.y + max(corner, side.x * side.y)) / 3.0;
    }

    [ForceInline]
    float4 voxelAo(float3 pos, float3 d1, float3 d2)
    {
        let side   = occlusion4(pos + d1, pos + d2, pos - d1, pos - d2);
        let corner = occlusion4(pos + d1 + d2, pos - d1 + d2, pos - d1 - d2, pos + d1 - d2);

        var ao: float4;
        ao.x = vertexAo(side.xy, corner.x);
        ao.y = vertexAo(side.yz, corner.y);
        ao.z = vertexAo(side.zw, corner.z);
        ao.w = vertexAo(side.wx, corner.w);
        return saturate(1.0 - ao);
    }

    float ambientOcclusion(float3 voxel_pos, float3 hit_pos, float3 mask)
    {
        let uv = fract(float2(
            dot(mask * hit_pos.yzx, float3(1)),
            dot(mask * hit_pos.zxy, float3(1)) ) );

        let ambient = voxelAo(voxel_pos, mask.zxy, mask.yzx);

        float interpAo = lerp(
            lerp(ambient.z, ambient.w, uv.x),
            lerp(ambient.y, ambient.x, uv.x),
            uv.y );

        interpAo = pow(interpAo, 1.0 / 3.0);
        //interpAo = smoothHermite(interpAo);
        return max(interpAo, 0.5);
    }

    Collision::HitResult intersect(Collision::Ray ray, out float out_ao)
    {
        // 1. Coarse Intersection (Bounding Box)
        let coarse_hit = Map.volume.intersect(ray);
        if (!coarse_hit.valid)
        {
            out_ao = 1.0;
            return coarse_hit;
        }

        // 2. Precision Nudge
        // Move the entry point slightly *inside* the volume along the ray.
        // This prevents "acne" where floor() rounds to the wrong voxel at integer boundaries.
        let ray_start_world = Map.volume.closestPoint(coarse_hit.entry(ray));

        // 3. Space Conversion
        // Convert position to Map Space (Voxels).
        let p0 = Map.worldToMap(ray_start_world);
        if (occlusion(p0))
        {
            var narrow_hit = coarse_hit;
            narrow_hit.id = Map.linearIndex(int2(floor(p0.xy)));
            out_ao = 1.0;
            return narrow_hit;
        }

        // 4. DDA Setup
        let ray_step = sign(ray.dir);
        // delta_dist is the amount of 't' (ray parameter) required to cross 1 map unit on that axis.
        // Use a large but safe maximum value (prevents overflow while keeping DDA logic intact)
        const float MAX_DELTA_DIST = MAP_SIZE*2; // Adjust based on your world scale
        let delta_dist = select(
            abs(ray.dir) > M_EPSILON,
            min(abs(rcp(ray.dir)), float3(MAX_DELTA_DIST)),  // Clamp reciprocal
            float3(MAX_DELTA_DIST)                            // Use max for near-zero components
        );

        var pos = floor(p0);

        // Calculate distance to the *next* boundary.
        var side_dist = (ray_step * (pos - p0) + (ray_step * 0.5) + 0.5) * delta_dist;

        var mask = float3(0);

        // 5. Traversal Loop
        // Safety: Limit steps to prevent infinite loops if ray is parallel to axis
        for (int i = 0; i < 32; i++) {
            // Select the smallest distance axis
            mask = step(side_dist.xyz, side_dist.yzx) * step(side_dist.xyz, side_dist.zxy);
            side_dist += mask * delta_dist;
            pos += mask * ray_step;
            if (occlusion(pos)) break;
        }

        let id = int2(floor(pos.xy));
        if (Map.outsideBounds(id))
        {
            out_ao = 1.0;
            return Collision::HitResult();
        }

        var narrow_hit: Collision::HitResult;
        narrow_hit.id = Map.linearIndex(id);

        // 6. T Calculation (The Fix)
        // 'side_dist' now holds the t-value to EXIT the voxel we just hit.
        // We want the t-value to ENTER the voxel.
        // We simply subtract the delta of the last step taken.
        // Note: If we hit on the very first check (i==0), mask is 0, t_local is 0. Correct.
        let t_local_entry = dot(side_dist - delta_dist, mask);

        narrow_hit.t.x = coarse_hit.t.x + t_local_entry;
        narrow_hit.normal = -ray_step * mask;

        // 7. Exit Calculation
        // We are currently inside a solid voxel. We want to find where we exit it.
        // We just need to know which boundary is next.
        let exit_mask = step(side_dist.xyz, side_dist.yzx) * step(side_dist.xyz, side_dist.zxy);
        let t_local_exit = dot(side_dist, exit_mask);

        narrow_hit.t.y = coarse_hit.t.x + t_local_exit;

        out_ao = ambientOcclusion(pos - ray_step * mask, p0 + ray.dir * t_local_entry, mask);
        return narrow_hit;
    }

} //!namespace Voxel

//------------------------------------------------------------------------------------------
// Map volume 3D for rendering
//------------------------------------------------------------------------------------------

namespace Map
{

static const Collision::Aabb volume = Collision::Aabb(float3(0, 0, -3.0), float3(DIM_2D * 0.5, 2.0));

static const Collision::Plane foreground = Collision::Plane(volume.center, float3(0, 0, 1));
static const Collision::Plane background = Collision::Plane(volume.center + float3(0,0,150), float3(0, 0, 1));

[ForceInline]
float2 closestPoint(float2 map_pos)
{
    return clamp(map_pos, float2(0), float2(DIM_2D - 1 - M_EPSILON));
}
[ForceInline]
float3 worldToMap(float3 world_pos)
{
    return world_pos - volume.min();
}
[ForceInline]
float3 mapToWorld(float3 map_pos)
{
    return map_pos + volume.min();
}
[ForceInline]
float3 mapToWorld(float2 map_pos)
{
    return mapToWorld(float3(map_pos, volume.extent.z));
}
[ForceInline]
float3 mapToWorld(float2 map_pos, float override_world_z)
{
    var world_pos = mapToWorld(map_pos);
    world_pos.z = override_world_z;
    return world_pos;
}

int2 project(float3 world_pos)
{
    return int2(floor(volume.worldToObject(world_pos).xy * DIM_2D));
}

float3 unproject(int2 map_pos)
{
    return volume.min() + float3(map_pos, 0) + 0.5;
}

struct Scene : Collision::IVolume, Collision::IRayTracer
{
    float euclideanDistance(float3 pos)
    {
        return Map.volume.euclideanDistance(pos);
    }

    bool contains(float3 pos)
    {
        return Map.volume.contains(pos);
    }

    float3 closestPoint(float3 pos)
    {
        return Map.volume.closestPoint(pos);
    }

    Collision::HitResult intersect(Collision::Ray ray)
    {
        float ignored_ao;
        return Voxel.intersect(ray, ignored_ao);
    }

    [ForceInline]
    Collision::HitResult trace(float3 start, float3 end)
    {
        return Voxel.trace(worldToMap(start).xy, worldToMap(end).xy);
    }
};

} //! namespace Map

//==========================================================================================
// Game state
//__________________________________________________________________________________________
namespace Game
{
//==========================================================================================

[playground::SLIDER(1.0, 0.0, 5.0)]
uniform float un_game_time_speed;

struct State
{
    Camera::Pan camera;

    float3 mouse_world;
    float3 mouse_screen;
    float3 click_pos_world;

    float2 taa_offset = float2(0);
    uint4 frame_seed = uint4(0);
    float triangular_rnd = 0.5;

    float frame_time_delta = 0;
    float frame_time_last_tick = 0;

    float game_time_elaspsed = 0;
    float game_time_delta = 0;
    float game_time_last_tick = 0;

    int frontal_light = -1;
    int is_clicked = 0;

    __init(Camera::Pan camera)
    {
        this.frame_time_last_tick = Time.elapsed;
        this.camera = camera;
    }

    [mutating]
    void update(float2 viewport)
    {
        //this.taa_offset = Camera::TAA_OFFSETS[Time.frame % Camera::TAA_OFFSETS.getCount()];
        this.taa_offset = hammersley(Time.frame % 16, 16) - 0.5;

        this.frame_time_delta = Time.elapsed - this.frame_time_last_tick;
        this.frame_time_last_tick = Time.elapsed;

        this.game_time_delta = this.frame_time_delta * un_game_time_speed;
        this.game_time_last_tick = this.game_time_elaspsed;
        this.game_time_elaspsed += this.game_time_delta;

        var rng = Rand.create(uint4(1, 3, 7, 11) + Time.frame);
        this.frame_seed = rng.state;
        this.triangular_rnd = Noise.interleavedGradient(int2(0), Time.frame);

        Game.last_camera = Game.camera;
        let cam = this.camera.update(viewport, this.frame_time_delta, key_alt == 1);
        Game.camera = cam;

        if (this.frontal_light >= 0)
        {
            let eye = this.camera.persp.eye;
            let light_focus = Map.volume.closestPoint(eye);
            let dlight = light_focus - eye;

            let frontal_cone = this.camera.persp.fov;
            var frontal = Shading::LightCulling.light(this.frontal_light);
            frontal.position = eye;
            frontal.range = length(dlight) + Map.volume.extent.z * 2;
            frontal.direction = dlight / frontal.range;
            // frontal.inner_cone = frontal_cone*frontal_cone;
            // frontal.outer_cone = frontal_cone;
            // frontal.outer_cone = frontal_cone*M_PI;
            // frontal.outer_cone = M_PI;
            frontal.range *= M_PHI;
            frontal.luminance = un_frontal_light_luminance;
            // frontal.color = Color::Pastel.rgb(cos(Time.elapsed)*0.5+0.5);

            //Shading::rw_light_allocation.get().store(this.frontal_light, frontal.pack().packed);
        }

        if (Mouse.is_down)
        {
            if (this.is_clicked == 0)
            {
                this.is_clicked = 1;
                this.click_pos_world = mousePick(cam, Mouse.click_pos);
            }
        }
        else
        {
            this.is_clicked = 0;
        }

        this.mouse_world = mousePick(cam, Mouse.pos);
        this.mouse_screen = cam.worldToScreen(this.mouse_world);
    }

    static float3 mousePick(Camera::Model cam, float2 pixel)
    {
        let ray = cam.worldRay(pixel);
        var hit = Map::Scene().intersect(ray);
        if (hit.valid)
        {
            return hit.entry(ray);
        }
        hit = Map.foreground.intersect(ray);
        if (hit.valid)
        {
            let world_pos = hit.entry(ray);
            return Map.volume.closestPoint(world_pos);
        }
        return Map.volume.closestPoint(ray.org);
    }
};

struct GlobalState
{
    State state;
    Camera::Model cam;
    Camera::Model last_cam;
};
property Game::State state
{
    get {
        return rw_global_state[0].state;
    }
    set {
        rw_global_state[0].state = newValue;
    }
}
property Camera::Model camera
{
    get {
        return rw_global_state[0].cam;
    }
    set {
        rw_global_state[0].cam = newValue;
    }
}
property Camera::Model last_camera
{
    get {
        return rw_global_state[0].last_cam;
    }
    set {
        rw_global_state[0].last_cam = newValue;
    }
}

[playground::SLIDER(0.3, 0.0, 5.0)]
uniform float un_frontal_light_luminance;

//------------------------------------------------------------------------------------------
} //! namespace Game

[playground::ZEROS(1)]
RWStructuredBuffer<Game::GlobalState> rw_global_state;

[shader("compute")]
[numthreads(1, 1, 1)]
[playground::CALL(1, 1, 1)]
void passGameUpdate(uint2 dispatchThreadID: SV_DispatchThreadID)
{
    var viewport : float2;
    outputTexture.GetDimensions(viewport.x, viewport.y);

    var state = Game.state;
    state.update(viewport);
    Game.state = state;
}

//==========================================================================================
//------------------------------------------------------------------------------------------
//==========================================================================================

// Shading pipeline

//------------------------------------------------------------------------------------------
// RWFat buffer
//------------------------------------------------------------------------------------------

namespace Fat
{
struct Scene : Fat::IBuffer<uint32_t>
{
    [ForceInline]
    static uint32_t load(uint idx)
    {
        return rw_fat_scene[idx];
    }

    [ForceInline]
    static void store(uint idx, uint32_t value)
    {
        rw_fat_scene[idx] = value;
    }

    static const uint CAPACITY = Map::Visibility::rw_map_visiblity::TAIL;
};
}

namespace BxDF::IntegrationMapBRDF
{
typealias rw_brdf_integration = Fat::Scene::Section<Fat::Scene::Cube<NDOTV_DIM, ROUGHNESS_DIM, ANISOTROPY_DIM>, ANISOTROPY_DIM, NDOTV_DIM*ROUGHNESS_DIM, 0>;
}
namespace Shading
{
typealias rw_light_culling = Fat::Scene::Section<Fat::Scene::Vec4, LightCulling::GRID_DIM * LightCulling::GRID_DIM, 4, BxDF::IntegrationMapBRDF::rw_brdf_integration::TAIL>;
typealias rw_light_allocation = Fat::Scene::Section<Fat::Scene::Vec4, MAX_LIGHTS, 4, rw_light_culling::TAIL>;
}
namespace Map
{
extension Biome
{
    typealias rw_inverse_cdf = Fat::Scene::Section<Fat::Scene::Grid<NUM_BIOMES + 1, NUM_CDF>, NUM_CDF, (NUM_BIOMES + 1), Shading::rw_light_allocation::TAIL>;
};
extension Visibility
{
    typealias rw_map_visiblity = Fat::Scene::Section<Fat::Scene::MipMapVector<LVL0_ROW_PITCH, LVL0_COL_PITCH, NUM_LEVELS, 2>, 1u, Fat::Scene::MipMapVector<LVL0_ROW_PITCH, LVL0_COL_PITCH, NUM_LEVELS, 2>.STRIDE, Biome::rw_inverse_cdf::TAIL>;
};
}

[playground::ZEROS(Fat::Scene::CAPACITY)]
RWStructuredBuffer<uint32_t> rw_fat_scene;

//------------------------------------------------------------------------------------------
// Direct lighting
//------------------------------------------------------------------------------------------

[shader("compute")]
[numthreads(16, 16, 1)]
[playground::CALL::ONCE]
[playground::CALL(BxDF::IntegrationMapBRDF::NDOTV_DIM, BxDF::IntegrationMapBRDF::ROUGHNESS_DIM, BxDF::IntegrationMapBRDF::ANISOTROPY_DIM)]
void passPrecomputeIntegrationMapBRDF(uint3 dispatchThreadID: SV_DispatchThreadID)
{
    BxDF::IntegrationMapBRDF::precompute(dispatchThreadID);
}

[shader("compute")]
[numthreads(16, 16, 1)]
[playground::CALL(Shading::LightCulling::GRID_DIM, Shading::LightCulling::GRID_DIM, 1)]
void passLightsCulling(uint2 dispatchThreadID: SV_DispatchThreadID, uint group_idx: SV_GroupIndex)
{
    static groupshared Shading::LightCulling s_light_allocs;

    if (group_idx == 0)
    {
        s_light_allocs = Shading::LightManager.culling();
    }
    GroupMemoryBarrierWithGroupSync();

    let bounds = Map::Cell.boundingBox(
        (dispatchThreadID.xy + 0) * Shading::LightCulling::GRID_TILE,
        (dispatchThreadID.xy + 1) * Shading::LightCulling::GRID_TILE - 1);
    let culling = s_light_allocs.cull(bounds);

    Shading::LightCulling::store(dispatchThreadID.xy, culling);
}

//------------------------------------------------------------------------------------------
// Global illumination
//------------------------------------------------------------------------------------------

[playground::SLIDER(1.0, 0.0, 2.0)]
uniform float un_radiance_multibounce;

[playground::SLIDER(1.0, 0.0, 2.0)]
uniform float un_radiance_emissive;

[playground::SLIDER(1.0, 0.0, 2.0)]
uniform float un_radiance_directDiffuse;

[playground::SLIDER(1.0, 0.0, 2.0)]
uniform float un_radiance_directSpecular;

[playground::SLIDER(1.0, 0.0, 2.0)]
uniform float un_radiance_diffuseIrradiance;

[playground::SLIDER(1.0, 0.0, 2.0)]
uniform float un_radiance_specularRadiance;

[ForceInline]
float3 shade<
    TIndirect : Shading::IIndirect,
    TRayTracer : Collision::IRayTracer,
    >(
    Shading::Geometry geo,
    Shading::Material mat,
    float ambient_occlusion,
    TRayTracer shadows)
{
    var radiance = mat.emissive * un_radiance_emissive;

    let F0 = mat.F0;
    let env_brdf = BxDF::IntegrationMapBRDF.sample(geo.normal, geo.surface_to_eye, mat.roughness, mat.anisotropy);

    // https://google.github.io/filament/Filament.html#materialsystem/improvingthebrdfs/energylossinspecularreflectance
    let energy_compensation = max(1.0 + F0 * (1.0 / max(env_brdf.y, M_EPSILON) - 1.0), float3(0));
    radiance += Shading::directLighting(geo, mat, F0, energy_compensation, shadows);

    var indirect = float3(0.0);
    let map_pos = Map.worldToMap(geo.position);

    // Find ambient diffuse IBL component
    let NdotV = BxDF::dot_c(geo.normal, geo.surface_to_eye);
    let F = BxDF::fresnelSchlickRoughness(NdotV, mat.F0, mat.roughness);
    let kD = max((1.0 - F) * (1.0 - mat.metallic) * mat.albedo / M_PI, float3(0));

    if (Color::luminance(kD) > M_EPSILON)
        indirect += kD * TIndirect.diffuseIrradiance(map_pos, geo.normal) * un_radiance_diffuseIrradiance;

    // Find ambient specular IBL component
    let R = geo.reflect(-geo.surface_to_eye, mat.roughness, mat.anisotropy);
    let kS = max(lerp(env_brdf.x, env_brdf.y, F0) * energy_compensation, float3(0));

    if (Color::luminance(kS) > M_EPSILON)
    {
        let max_specular_luma = Color.luminance(mat.albedo * (1-mat.metallic) + F0);
        var specular = kS * TIndirect.specularRadiance(map_pos, R, mat.roughness);
        let specular_luma = Color.luminance(specular);
        specular *= min(1.0, max_specular_luma / specular_luma);
        indirect += specular * un_radiance_specularRadiance;
    }

    radiance += indirect * ambient_occlusion;

    /*if (any(isnan(radiance)))
    {
        radiance = M_TAU * Color::Error.blink();
    }*/

    return max(radiance, float3(0));
}

struct CascadeRadiance<TIndirect: Shading::IIndirect> : Radiance::ICascadeTracer
{
    [ForceInline]
    float4 shade(float2 start, float3 eye, Collision::HitResult hit)
    {
        if (!hit.valid) return float4(0,0,0,1);

        hit.t *= eye.z;

        let map_id = Map.mapIndex(hit.id);
        let cell = Map::Cell.load(map_id);
        let mat = cell.shading();
        var geo = hit.geometry(Collision::Ray(float3(start, 0), float3(eye.xy, 0)));

        geo.position = Map.mapToWorld(geo.position.xy);
        geo.normal = normalizeSafe(geo.normal, float3(0,0,1));

        var radiance = ::shade<TIndirect>(geo, mat, un_radiance_multibounce, Map::Scene());

        return float4(radiance, select(hit.valid, 0.0, saturate(1-Color::luminance(radiance))));
    }

    [ForceInline]
    Collision::HitResult intersect(float2 start, float2 end, out float3 out_eye)
    {
        let map_id = int2(floor(start));
        if (Map.outsideBounds(map_id)) return Collision::HitResult();
        //if (Map::Visibility.contains(map_id, 0) == Map::Visibility::Hit) return float4(0,0,0,0);

        var eye = (end - start);
        var t_end = length(eye);
        eye /= max(t_end, M_EPSILON);
        t_end = min(t_end, Collision.intersectRayAABBInside(start, eye, float4(0,0,Map::DIM_2D-1)));
        end = start + eye * t_end;

        out_eye = float3(eye, t_end);

        return Voxel.trace(start, end);
    }

    float4 trace(float2 start, float2 end)
    {
        float3 eye;
        let hit = this.intersect(start, end, eye);
        return shade(start, eye, hit);
    }

    [ForceInline]
    float4 shade2(float2 start, float2 end, Collision::HitResult hit)
    {
        if (!hit.valid) return float4(0,0,0,1);

        var geo : Shading::Geometry;
        geo.surface_to_eye = float3(normalizeSafe(start - end, float2(0,1)), 0);
        geo.position = float3(lerp(start, end, hit.t.x), 0.0);
        geo.normal = hit.normal;

        geo.position = Map.mapToWorld(geo.position.xy);
        orthonormalBasis(geo.normal, geo.tangent, geo.binormal);
        geo.position_ddx = geo.tangent * 0.5;
        geo.position_ddy = geo.binormal * 0.5;

        let map_id = Map.mapIndex(hit.id);
        let cell = Map::Cell.load(map_id);
        let mat = cell.shading();
        var radiance = ::shade<TIndirect>(geo, mat, un_radiance_multibounce, Map::Scene());

        return float4(radiance, select(hit.valid, 0.0, saturate(1-Color::luminance(radiance))));
    }

    float4 trace2(float2 start, float2 end)
    {
        if (Map.outsideBounds(int2(floor(start)))) return float4(0,0,0,1);

        var eye = (end - start);
        var t_end = length(eye);
        eye /= max(t_end, M_EPSILON);
        t_end = min(t_end, Collision.intersectRayAABBInside(start, eye, float4(0,0,Map::DIM_2D-1)));
        end = start + eye * t_end;

        let hit = Voxel.trace(start, end);
        return shade2(start, end, hit);
    }
};

typealias RadianceSingleBounce = CascadeRadiance<Shading::NullIndirect>;
typealias RadianceMultiBounce = CascadeRadiance<Radiance::SuperSampling>;

[ForceInline]
void passRadianceCascadeImpl<let Level: uint>(uint2 dispatchThreadID)
{
    let tracer = RadianceMultiBounce();
    Radiance::Cascade::computeLevelAndStore<Level>(dispatchThreadID.xy, tracer);
}

#define PASS_RADIANCE_CASCADE(_LEVEL) \
    [shader("compute")] \
    [numthreads(16, 16, 1)] \
    [playground::CALL(Radiance::Cascade::DISPATCH, Radiance::Cascade::DISPATCH, 1)] \
    void passRadianceCascade ## _LEVEL(uint2 dispatchThreadID: SV_DispatchThreadID) \
    { \
        passRadianceCascadeImpl<_LEVEL>(dispatchThreadID); \
    }

PASS_RADIANCE_CASCADE(5)
PASS_RADIANCE_CASCADE(4)
PASS_RADIANCE_CASCADE(3)
PASS_RADIANCE_CASCADE(2)
PASS_RADIANCE_CASCADE(1)
#if !use_radiance_supersampling
PASS_RADIANCE_CASCADE(0)
#endif
#undef PASS_RADIANCE_CASCADE

#if use_radiance_supersampling
[shader("compute")]
[numthreads(16, 16, 1)]
[playground::CALL(Radiance::SuperSampling::DISPATCH, Radiance::SuperSampling::DISPATCH, 1)]
void passRadianceCascadeTemporalUpdate(uint2 dispatchThreadID: SV_DispatchThreadID)
{
    Radiance::SuperSampling.update(dispatchThreadID.xy, CascadeRadiance<Radiance::Cascade>());
}
#endif

#define use_radiance_debugging 0

[playground::URL("https://dl.polyhaven.org/file/ph-assets/Textures/png/4k/marble_mosaic_tiles/marble_mosaic_tiles_nor_dx_4k.png")]
[format("rgba8")]
Texture2D<float4> tex_normal_map;

[playground::SAMPLER]
SamplerState linear_sampler;

Shading::Material floorMaterial(inout Shading::Geometry geo)
{
    var mat = Map::Cell::VACUUM.shading();

    let uv = Map.volume.worldToObject(geo.position).xy;
    let uv_ddx = Map.volume.worldToObject(geo.position + geo.position_ddx).xy - uv;
    let uv_ddy = Map.volume.worldToObject(geo.position + geo.position_ddy).xy - uv;

    let pattern = Color.filteredXorGrad(
        uv * 4 * MAP_SIZE,
        uv_ddx * 4 * MAP_SIZE,
        uv_ddy * 4 * MAP_SIZE );

    mat.roughness = pattern.x;
    mat.albedo = float3(lerp(0.8, 1.0, pattern.x));

    let border_tile = 4 * 4;
    let border_pos = fract(geo.position.xy / border_tile) - 0.5;
    var border = maxNorm(border_pos);
    let dborder = ddxyLen(geo.position_ddx.xy / border_tile, geo.position_ddy.xy / border_tile);
    border = smoothstep(2.5, 0.0, abs(border - 0.5) / dborder);
    mat.albedo += border * 0.5;

    mat.roughness = min(pattern.x, 1 - border);
    mat.anisotropy = cos(Time.elapsed);
    //mat.metallic = border;

    let nrm = normalize(float3(pattern.yz*0.5, 1));

    geo.normal = normalize(float3(
        dot(nrm, geo.tangent),
        dot(nrm, geo.binormal),
        dot(nrm, geo.normal) ));


#if 1 // looks terrible without mip-maps
    const float tiling = 8.0;
    var nrm_map = tex_normal_map.SampleGrad(linear_sampler, uv*tiling, uv_ddx*tiling, uv_ddy*tiling).xyz ;

    nrm_map = nrm_map * 2 - 1;
    //nrm.y = -nrm.y; // for OpenGL textures

    geo.normal = normalize(float3(
        dot(nrm_map, geo.tangent),
        dot(nrm_map, geo.binormal),
        dot(nrm_map, geo.normal) ));
#endif
    return mat;
}

#if use_visibilitycache_debug
[playground::SLIDER(0.0, 0.0, 8.0)]
uniform float un_radiance_cascade;
#endif

void debugIndirectCascade(float2 map_pos, float ddxy_len, uint level, inout Canvas.Context canvas)
{
    using namespace Radiance;
    let cascade = Cascade(level);
    let probe_id = cascade.probeSample(map_pos).nearest;
    let probe_frame = Cascade.probeFrame(probe_id, Time.frame);
    let probe_center = (probe_id + 0.5) * cascade.probe_size;  //cascade.probeCenter(probe_id, probe_frame);
    let probe_radius = 0.5 * cascade.probe_size - ddxy_len * 5.5;

    let sample_dir = normalizeSafe(float3(map_pos - probe_center, 0), float3(0,0,1));
    let radiance = cascade.eval(probe_id, sample_dir);

    let tonemapped = saturate(Color::AGX.Default.from(radiance));

    canvas.style.fill = float4(0);

    for (uint ch = 0u; ch < 3u; ch++)
    {
        let intensity = tonemapped[ch];
        canvas.style.stroke = float4(0,0,0,intensity);
        canvas.style.stroke[ch] = intensity;
        canvas.style.stroke_width = 5*intensity;
        canvas.style.glow = float4(canvas.style.stroke.rgb, 0.1);
        canvas.drawCircle(probe_center, probe_radius * intensity, ddxy_len);
    }

    let intensity = max(tonemapped.r, max(tonemapped.g, tonemapped.b));
    canvas.style.stroke_width = 0;
    canvas.style.glow = float4(tonemapped, 0.4);
    canvas.style.glow_width = 10;
    canvas.drawCircle(probe_center, probe_radius * intensity, ddxy_len);
}

#if use_radiance_supersampling
void debugIndirectSuperSampling(float2 map_pos, float ddxy_len, inout Canvas.Context canvas)
{
    using namespace Radiance;
    let probe_id = SuperSampling.probeSample(map_pos).nearest;
    let probe_center = SuperSampling.probeCenter(probe_id);
    let probe_radius = 0.5 * SuperSampling.probe_size - ddxy_len * 5.5;

    let probe = SuperSampling.load(probe_id);

    let sample_dir = normalizeSafe(float3(map_pos - probe_center, 0), float3(0,0,1));
    let radiance = SuperSampling.eval(probe, sample_dir);

    let tonemapped = saturate(Color::AGX.Default.from(radiance));

    canvas.style.fill = float4(0);

    for (uint ch = 0u; ch < 3u; ch++)
    {
        let intensity = tonemapped[ch];
        canvas.style.stroke = float4(0,0,0,intensity);
        canvas.style.stroke[ch] = intensity;
        canvas.style.stroke_width = 5*intensity;
        canvas.style.glow = float4(canvas.style.stroke.rgb, 0.1);
        canvas.drawCircle(probe_center, probe_radius * intensity, ddxy_len);
    }


    let intensity = max(tonemapped.r, max(tonemapped.g, tonemapped.b));
    canvas.style.stroke_width = 0;
    canvas.style.glow = float4(tonemapped, 0.4);
    canvas.style.glow_width = 10;
    canvas.drawCircle(probe_center, probe_radius * intensity, ddxy_len);

#if 0
    let select_id = SuperSampling.probeSample(Map.worldToMap(Game.state.mouse_world).xy).nearest;
    //if (all(select_id == probe_id))
    {
        static const Cascade cascade = Cascade(1u);
        let probe_center = SuperSampling.probeCenter(select_id);// + Game.state.taa_offset - 0.5;
        let probe_box = probe_center.xyxy + (float2(-1, 1) * cascade.probe_size).xxyy;
        let sampler = cascade.probeSample(probe_center);

        canvas.style.stroke_width = 1;
        canvas.style.stroke = float4(0,0,1, 0.8);
        canvas.style.glow = float4(canvas.style.stroke.rgb, 0.1);
        canvas.style.fill = float4(0);
        canvas.drawBox(probe_center, cascade.probe_size, ddxy_len);

        for (var it = sampler.begin(); it != sampler.end(); it++)
        {
            let smp = sampler.at(it);
            let parent_probe_frame = Cascade.probeFrame(smp.index, Time.frame);
            let parent_probe_center = cascade.probeCenter(smp.index, parent_probe_frame);

            canvas.style.stroke_width = 1;
            canvas.style.stroke = float4(1,0,0, 0.4);
            canvas.style.glow = float4(canvas.style.stroke.rgb, 0.1);
            canvas.style.fill = float4(0);
            canvas.drawBox(parent_probe_center, cascade.probe_size*0.5, ddxy_len);

            let parent_probe_box = parent_probe_center.xyxy + (float2(-0.5, 0.5) * cascade.probe_size).xxyy;

            for (var phi = 0u; phi < cascade.num_dirs; phi++)
            {
                let location = Cascade::ProbeLocation(smp.index, phi, cascade);
                let pixel_index = Cascade.probeLocationToPixelIndex(location);

                var radiance_far = rw_radiance_cascades[pixel_index].unpack();

                let parent_probe_dir = cascade.probeDir(smp.index, parent_probe_frame, phi);
                let probe_hit = Collision.intersectRayAABBInside(parent_probe_center, parent_probe_dir, probe_box);
                //let probe_hit = Collision.intersectRayAABBInside(parent_probe_center, parent_probe_dir, parent_probe_box);
                let probe_dir = normalizeSafe(parent_probe_center + parent_probe_dir * probe_hit - probe_center, parent_probe_dir);

                let map = RadianceSingleBounce();
                var radiance_near = map.trace(probe_center, parent_probe_center + parent_probe_dir * probe_hit);
                let radiance_merged = Cascade.merge(radiance_near, radiance_far);

                canvas.style.stroke_width = 0;
                canvas.style.glow_width = 0;
                canvas.style.fill = float4(radiance_far.rgb, 0.4);
                canvas.drawArrow(parent_probe_center, parent_probe_center + parent_probe_dir * probe_hit, (2 - radiance_far.a) * ddxy_len, ddxy_len);

                canvas.style.stroke_width = 1;
                canvas.style.glow_width = 6;
                canvas.style.fill = float4(radiance_merged.rgb, 0.4);
                canvas.style.stroke = float4(radiance_merged.rgb, 0.8);
                canvas.style.glow = float4(canvas.style.fill.rgb, 0.1);
                canvas.drawArrow(probe_center, parent_probe_center + parent_probe_dir * probe_hit, (2 - radiance_merged.a) * ddxy_len, ddxy_len);
            }

        }

    }
#endif
}
#endif

void debugIndirect(float2 map_pos, float ddxy_len, out float4 outp)
{
    var canvas = Canvas::Context();
    canvas.moveTo(map_pos);

    debugIndirectCascade(map_pos, ddxy_len, 1u, canvas);
    debugIndirectSuperSampling(map_pos, ddxy_len, canvas);

    using namespace Radiance;
    let probe_id = SuperSampling.probeSample(map_pos).nearest;
    let probe_center = SuperSampling.probeCenter(probe_id);
    let probe_radius = 0.5 * SuperSampling.probe_size - ddxy_len * 5.5;

    let map = RadianceSingleBounce();
    let mouse_map = Map.worldToMap(Game.state.mouse_world).xy;
    let ray_dir = normalize(mouse_map - probe_center);
    let ray_start = probe_center + ray_dir * Cascade(0).depth_interval.x;
    let ray_end = probe_center + ray_dir * Cascade(0).depth_interval.y;

    float3 ray_eye;
    var hit = map.intersect(ray_start, ray_end, ray_eye);

    if (hit.valid)
    {
        let radiance = map.shade(ray_start, ray_eye, hit);

        let hit_pos = select(hit.valid, ray_start + ray_eye.xy * ray_eye.z * hit.t.x, ray_end);

        canvas.style.stroke_width = select(hit.valid, 1, 0);
        canvas.style.glow_width = select(hit.valid, 6, 0);
        canvas.style.fill = float4(radiance.rgb, 0.4);
        canvas.style.stroke = float4(radiance.rgb, 0.8);
        canvas.style.glow = float4(radiance.rgb, 0.1);

        canvas.drawArrow(ray_start, hit_pos, (2 - radiance.a) * ddxy_len, ddxy_len);
        if (hit.valid)
            canvas.drawArrow(hit_pos, hit_pos + hit.normal.xy, ddxy_len, ddxy_len);
    }

    outp.rgb = canvas.output(outp.rgb, 1.0).rgb;
}

float4 shadeWorld(int2 pixel_id, float2 screen_size)
{
    let frag = Shading::Fragment(pixel_id, uint2(screen_size));

    let cam = Game.camera;
    Collision::Ray eye_ddx, eye_ddy;
    var eye = cam.worldRayDdxDdy(frag.coord, eye_ddx, eye_ddy);

    var outp = float4(0, 0, 0, 1);

    var ao = 1.0;
    var hit = Voxel.intersect(eye, ao);
    if (hit.valid)
    {
        let map_id = Map.mapIndex(hit.id);
        let cell = Map::Cell.load(map_id);
        var geo = hit.geometry(eye, eye_ddx, eye_ddy);

        var mat = cell.shading();
        if (cell.is_interior)
        {
#if 0
            let nrm_dx = geo.tangent;
            let nrm_dy = geo.binormal;

            // Transform view direction to tangent space
            let view_tangent = float3(
                dot(geo.surface_to_eye, nrm_dx),
                dot(geo.surface_to_eye, nrm_dy),
                dot(geo.surface_to_eye, geo.normal)
            );

            // First pass: compute approximate height for parallax
            let pos_frac_init = frac(geo.position + geo.normal * M_EPSILON);
            let pos_centered_init = pos_frac_init - 0.5;
            let dist_from_edge_init = 0.5 - abs(pos_centered_init);
            let bevel_radius = 0.3 * mat.roughness;

            let chamfer_blend_init = smootherStep(0.0, bevel_radius, bevel_radius - dist_from_edge_init);
            let height = 1.0 - max(max(chamfer_blend_init.x, chamfer_blend_init.y), chamfer_blend_init.z);

            // Fade out parallax near face edges to avoid seams
            let edge_fade = min(min(dist_from_edge_init.x, dist_from_edge_init.y), dist_from_edge_init.z);
            let parallax_mask = smootherStep(0.0, ddxyLen(geo.position_ddx, geo.position_ddy)*1.5, edge_fade);

            // Parallax offset with edge masking
            let parallax_scale = 0.1*un_parallax_offset;
            let parallax_offset = (height - 0.5) * parallax_scale * parallax_mask / max(abs(view_tangent.z), 0.1);
            let pos_parallax = geo.position + parallax_offset * view_tangent;

            // Second pass: compute chamfer with parallax-corrected position
            let pos_frac = frac(pos_parallax + geo.normal * M_EPSILON);
            let pos_centered = pos_frac - 0.5;
            let dist_from_edge = 0.5 - abs(pos_centered);

            // Compute derivatives of distance field
            let dist_ddx = -sign(pos_centered) * geo.position_ddx;
            let dist_ddy = -sign(pos_centered) * geo.position_ddy;

            let pixel_footprint = ddxyLen(dist_ddx, dist_ddy);
            let filter_strength = smootherStep(bevel_radius * 1.0, bevel_radius * 0.5, pixel_footprint);

            let chamfer_blend = smootherStep(0.0, bevel_radius, bevel_radius - dist_from_edge) * filter_strength;

            // UVs and texture
            let uv = float2(dot(geo.position, nrm_dx), dot(geo.position, nrm_dy));
            let uv_ddx = float2(dot(geo.position_ddx, nrm_dx), dot(geo.position_ddx, nrm_dy));
            let uv_ddy = float2(dot(geo.position_ddy, nrm_dx), dot(geo.position_ddy, nrm_dy));

            const float tile = 8.0;
            let fxor = Color.filteredXorGrad(uv*tile, uv_ddx*tile, uv_ddy*tile);
            let xor_normal = normalize(float3(-fxor.yz, -0.5));
            mat.roughness = saturate(mat.roughness + (2*fxor.x-1) * 0.1);

            // Direction from cube center to current position
            let to_surface = pos_frac - 0.5;
            let chamfer_direction = sign(to_surface) * chamfer_blend;

            let bevel_amount = max(max(chamfer_blend.x, chamfer_blend.y), chamfer_blend.z);

            //if (frag.debug_flip)
            geo.normal = normalize(geo.normal + chamfer_direction * bevel_amount * 0.5);
#else
            let nrm_dx = geo.tangent;
            let nrm_dy = geo.binormal;

            // === 45-Degree Chamfer with Filtering ===
            let pos_frac = frac(geo.position + geo.normal * M_EPSILON);

            // Project to tangent space (only use in-plane coordinates)
            let pos_2d = float2(dot(pos_frac, nrm_dx), dot(pos_frac, nrm_dy));
            let pos_centered_2d = frac(pos_2d) - 0.5;
            let dist_from_edge_2d = 0.5 - abs(pos_centered_2d);

            let bevel_radius = 0.25 * mat.roughness;

            // Compute derivatives of distance field (2D only)
            let dist_ddx_2d = -sign(pos_centered_2d) * float2(dot(geo.position_ddx, nrm_dx), dot(geo.position_ddx, nrm_dy));
            let dist_ddy_2d = -sign(pos_centered_2d) * float2(dot(geo.position_ddy, nrm_dx), dot(geo.position_ddy, nrm_dy));

            let pixel_footprint_2d = ddxyLen(dist_ddx_2d, dist_ddy_2d);
            let filter_strength_2d = smootherStep(bevel_radius * 2.0, 0.0, pixel_footprint_2d);

            // Chamfer blend (2D only - x and y in tangent space)
            let chamfer_blend_2d = smootherStep(0, bevel_radius, bevel_radius - dist_from_edge_2d) * filter_strength_2d;

            // Bevel amount: now only from in-plane edges
            let bevel_amount = max(chamfer_blend_2d.x, chamfer_blend_2d.y);

            // Height: 1.0 at center, 0.0 in crevices
            let height = 1.0 - bevel_amount;

            // AO in crevices
            const float ao_strength = 0.5;
            let cavity_ao = smoothHermite(height*height);
            ao = ao * pow(lerp(1.0 - ao_strength, 1.0, cavity_ao), 1.0/3);

            // Direction from cube center (3D for normal calculation)
            let to_surface = pos_frac - 0.5;
            let dist_from_edge_3d = 0.5 - abs(to_surface);
            let chamfer_blend_3d = smootherStep(0, bevel_radius, bevel_radius - dist_from_edge_3d);
            let chamfer_direction = sign(to_surface) * chamfer_blend_3d;

            // detail normal
            let uv = float2(dot(geo.position, nrm_dx), dot(geo.position, nrm_dy));
            let uv_ddx = float2(dot(geo.position_ddx, nrm_dx), dot(geo.position_ddx, nrm_dy));
            let uv_ddy = float2(dot(geo.position_ddy, nrm_dx), dot(geo.position_ddy, nrm_dy));

            // xor texture
            const float tile = 8.0;
            let fxor = Color.filteredXorGrad(uv*tile, uv_ddx*tile, uv_ddy*tile);
            var xor_normal = float3(-fxor.yz, -0.5);
            xor_normal = float3(
                dot(xor_normal, geo.tangent),
                dot(xor_normal, geo.binormal),
                -dot(xor_normal, geo.normal),
            );

            geo.normal = normalize(geo.normal +
                chamfer_direction * bevel_amount * 0.5 +
                xor_normal * height );
            //mat.albedo = Color::OKLab.mix(mat.albedo, float3(1), smoothstep(0.0, 0.2, bevel_amount));
#endif
        }
        else
        {
            mat = floorMaterial(geo);
        }

        outp.rgb = shade<Radiance::GI>(geo, mat, ao, Map::Scene());
        //outp.rgb = (geo.normal*0.5+0.5)*ao;
        //outp.rgb = Color::OKLab.mix(mat.albedo, geo.normal*0.5+0.5, 0.5);

        if (cell.elem.phase == Map::Phase::Translucent)
        {

        }

#if 1
        //outp.rgb = select(Map::Visibility.contains(map_id, 0) == Map::Visibility::Hit, Color::Error.anim(), float3(0,0,1));
        debugIndirect(  Map.worldToMap(geo.position).xy,
                        ddxyLen(geo.position_ddx.xy, geo.position_ddy.xy),
                        outp);
#endif
#if 0
        var canvas = Canvas.Context();
        canvas.moveTo(Map.worldToMap(geo.position).xy);

        let ddxy_len = ddxyLen(geo.position_ddx.xy, geo.position_ddy.xy);

        let mouse_pos = Map.worldToMap(Game.state.mouse_world).xy;
        let mouse_id = int2(floor(mouse_pos));

        canvas.style.fill = float4(Color::Error.anim(), 0.5);
        canvas.drawBox(Radiance::SuperSampling.probeCenter(mouse_id), float2(0.5), ddxy_len);

        let cascade = Radiance::Cascade(0);
        let sampling = cascade.probeSample(mouse_pos).bicubic;
        for (var it = sampling.begin(); it != sampling.end(); it++)
        {
            let smp = sampling.at(it);
            canvas.style.fill = float4(Color::Temperature.rgb(smp.weight), 0.5);
            let probe_center = (smp.index + 0.5) * cascade.probe_size; // cascade.probeCenter(smp.index);
            canvas.drawBox(probe_center, float2(0.5 * cascade.probe_size), ddxy_len);

            //let parent_probe_center = cascade.probeCenter(smp.index);
            //let radiance_near = map.trace(probe_center, parent_probe_center);
            canvas.drawArrow(mouse_id + 0.5, probe_center, ddxy_len, ddxy_len);
        }

        outp.rgb = canvas.output(outp.rgb, 1.0).rgb;

#endif

    }
    else
    {
        hit = Map.background.intersect(eye);

        var geo = hit.geometry(eye, eye_ddx, eye_ddy);
        let mat = floorMaterial(geo);
        ao = 0.02;
/*
        var dist_to_eye: float;
        let light_pos = eye.org + float3(0,0,80);
        hit = Map::volume.intersect(Collision::Ray.fromSegment(geo.position, light_pos, dist_to_eye));


        if (hit.valid)
        {
            ao *= 0.85;
        }
*/
        outp.rgb += mat.albedo * ao;
    }

    if (any(isnan(outp)))
    {
        outp = float4(Color::Magma.blink(), 0);
    }

    outp.rgb = Color::AGX::Punchy.from(outp.rgb);
    return outp;
}

[shader("compute")]
[numthreads(16, 16, 1)]
[playground::CALL::SIZE_OF("outputTexture")]
void imageMain(uint2 dispatchThreadID: SV_DispatchThreadID)
{
#if 0
    drawPixel(dispatchThreadID, (int2 screen_size) => {
        let id = int2(dispatchThreadID.xy);
        return shadeWorld(id, screen_size);
	});
#else
    uint outputWidth, outputHeight;
    outputTexture.GetDimensions(outputWidth, outputHeight);

    let id = int2(dispatchThreadID.xy);
    let screen_size = float2(outputWidth, outputHeight);
    let col = shadeWorld(id, screen_size);
    outputTexture.Store(id, col);
#endif
}
