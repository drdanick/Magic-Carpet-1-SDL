#pragma once
// 32-bit "addresses" of the decompiled code on 64-bit builds.
//
// The decompiled game code keeps addresses in 32-bit ints and in 4-byte slots of packed DOS-layout
// structs (entity +156/+160, lists, tables). On 64-bit builds those ints are offsets into two windows:
//   0 .. 1 GB      from g_ptrBase (the executable): its statics and the game memory arena (Port_ArenaAlloc)
//   1 GB .. 2 GB   the game thread's stack (the code passes addresses of locals around as ints)
//   PTR(x)  int offset -> pointer (0 stays nullptr)      used where the code casts an int to a pointer
//   OFF(p)  pointer -> int offset (nullptr stays 0)       used where the code casts a pointer to an int
// OFF() aborts with a message for anything outside both windows. On 32-bit builds both are plain casts.
//
// TODO (before the code split/organisation phase): replace this with real typed pointers
// (remc2 style) - see docs/DEVELOPER_GUIDE.md section 7.
#include <cstddef>
#include <cstdint>

extern uint8_t* const g_ptrBase;
extern uint8_t* g_ptrBases[4];  // per offset >> 30: [0] g_ptrBase, [1] game stack - 1 GB, [2][3] g_ptrBase
int32_t Port_PtrOutOfRange(const void* p);  // the stack window (registered on first use), else abort

// Game memory (zeroed), at a fixed offset from g_ptrBase with a deterministic layout - see ptr32.cpp.
// Port_ArenaFree passes pointers from elsewhere on to free().
void* Port_ArenaAlloc(size_t size);
void Port_ArenaFree(void* p);

#if INTPTR_MAX == INT32_MAX
inline uint8_t* PTR(uint32_t off) { return (uint8_t*)(uintptr_t)off; }
template <class T> inline int32_t OFF(T* p) { return (int32_t)(uintptr_t)p; }
#else
inline uint8_t* PTR(uint32_t off) { return off ? g_ptrBases[off >> 30] + off : nullptr; }
template <class T> inline int32_t OFF(T* p)
{
	if (!p)
		return 0;
	intptr_t d = (intptr_t)reinterpret_cast<uintptr_t>(p) - (intptr_t)g_ptrBase;
	if (d > 0 && d < 0x40000000)
		return (int32_t)d;
	return Port_PtrOutOfRange(reinterpret_cast<const void*>(p));
}
#endif
inline int32_t OFF(std::nullptr_t) { return 0; }

// Pointer fields of packed DOS-layout structs: 4 bytes in every build.
//   ptr32<T>     a game pointer stored as an OFF() offset (converts to/from T*)
//   extptr32<T>  a pointer to memory outside the game window (FILE*), stored as a small handle
// On 32-bit builds both are plain T* (the harness reads the PDB types of 32-bit builds).
#if INTPTR_MAX == INT32_MAX
template <class T> using ptr32 = T*;
template <class T> using extptr32 = T*;
#else
template <class T> struct ptr32
{
	int32_t off;
	ptr32() = default;
	ptr32(T* p) : off(OFF(p)) {}
	ptr32(std::nullptr_t) : off(0) {}
	ptr32(int32_t o) : off(o) {}  // an int "address" of the decompiled code is an offset already (0 = null)
	ptr32& operator=(T* p) { off = OFF(p); return *this; }
	operator T*() const { return (T*)PTR(off); }
	template <class U> explicit operator U*() const { return (U*)PTR(off); }  // (uint8_t*)field etc.
	T* operator->() const { return (T*)PTR(off); }
	T& operator*() const { return *(T*)PTR(off); }
	ptr32& operator+=(intptr_t n) { off += (int32_t)(n * (intptr_t)sizeof(T)); return *this; }
	ptr32& operator-=(intptr_t n) { off -= (int32_t)(n * (intptr_t)sizeof(T)); return *this; }
	ptr32& operator++() { off += (int32_t)sizeof(T); return *this; }
	ptr32 operator++(int) { ptr32 r = *this; off += (int32_t)sizeof(T); return r; }
	ptr32& operator--() { off -= (int32_t)sizeof(T); return *this; }
};
template <class T> inline int32_t OFF(ptr32<T> p) { return p.off; }

uint32_t Port_ExtHandle(void* p);  // 0 for nullptr; the same pointer always gets the same handle
void* Port_ExtPointer(uint32_t h);
template <class T> struct extptr32
{
	uint32_t h;
	extptr32() = default;
	extptr32(T* p) : h(Port_ExtHandle(p)) {}
	extptr32& operator=(T* p) { h = Port_ExtHandle(p); return *this; }
	operator T*() const { return (T*)Port_ExtPointer(h); }
	T* operator->() const { return (T*)Port_ExtPointer(h); }
};
#endif
static_assert(sizeof(ptr32<int>) == 4 && sizeof(extptr32<int>) == 4, "pointer slots must stay 4 bytes");

// Function-pointer tables of the decompiled code cast functions that take or return int "addresses" to
// typed signatures, e.g. (void (*)(Type_AE400_29795*))sub_253E0 where sub_253E0(int a1). On 64-bit the
// call would pass a 64-bit pointer into a 32-bit int, so PORT_FN(F, Sig) puts a thunk in the table that
// converts every argument and the result (int <-> pointer through PTR/OFF). On 32-bit it is the plain
// cast (the harness maps table entries back to the functions themselves).
#include <type_traits>
template <class To, class From> inline To PortConv(From f)
{
	if constexpr (std::is_pointer_v<To> && std::is_integral_v<From>)
		return (To)PTR((uint32_t)f);
	else if constexpr (std::is_integral_v<To> && std::is_pointer_v<From>)
		return (To)OFF(f);
	else
		return (To)f;
}
template <class Sig, class FSig, auto F> struct PortThunkImpl;
template <class R, class... A, class FR, class... FA, auto F> struct PortThunkImpl<R(A...), FR(FA...), F>
{
	static R call(A... a)
	{
		if constexpr (std::is_void_v<R>)
			F(PortConv<FA>(a)...);
		else if constexpr (std::is_void_v<FR>)
		{
			F(PortConv<FA>(a)...);
			return R();
		}
		else
			return PortConv<R>(F(PortConv<FA>(a)...));
	}
};
// usage: PORT_FN(function, signature) - the signature last, it may contain commas
#if INTPTR_MAX == INT32_MAX
#define PORT_FN(F, ...) ((std::add_pointer_t<__VA_ARGS__>)(F))
#else
#define PORT_FN(F, ...) (&PortThunkImpl<__VA_ARGS__, std::remove_pointer_t<decltype(&F)>, &F>::call)
#endif
