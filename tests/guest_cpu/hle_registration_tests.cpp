// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include "core/guest_cpu/hle/call_adapter.h"
using namespace Core::GuestCpu;
using namespace Core::GuestCpu::Hle;
static int checks{}, failures{};
static void Check(const char* name, bool good) {
    ++checks;
    failures += !good;
    std::printf("%s %s\n", good ? "PASS" : "FAIL", name);
}
static int Add(int a, int b) {
    return a + b;
}
static int Callback(int (*)(int)) {
    return 1;
}
struct Opaque;
static int OpaqueHandle(Opaque*) {
    return 1;
}
static int RawBuffer(int*, unsigned) {
    return 1;
}
static int Varargs(int, ...) {
    return 1;
}
static int Noexcept(int) noexcept {
    return 1;
}
using CallbackType = int (*)(int);
static CallbackType ReturnCallback() {
    return nullptr;
}
struct Aggregate {
    int x;
    int y;
};
static Aggregate Unsupported(Aggregate a) {
    return a;
}
int main() {
    auto shared = MakeHleAdapter(&Add, "add");
    HleCallRegistry a, b;
    auto one = a.Adopt(shared, "a");
    (void)b.Register(&Add, "padding");
    auto two = b.Adopt(shared, "b");
    Check("independent operations", one && two && one.Value() == 1 && two.Value() == 2);
    Check("first registry identity immutable",
          a.Find(1)->Operation() == 1 && a.Find(1)->Name() == "a");
    Check("factory descriptor remains unassigned", shared->Operation() == 0);
    auto unsupported = MakeHleAdapter(&Unsupported, "aggregate");
    const auto size = a.Size();
    Check("unsupported adoption refused", !a.Adopt(unsupported, "aggregate") && a.Size() == size);
    Check("raw callback signature refused", !a.Register(&Callback, "callback"));
    auto pointer = static_cast<int (*)(int, int)>(nullptr);
    Check("null function refused", !a.Register(pointer, "null"));
    Check("opaque pointer descriptor compiles and refuses",
          !MakeHleAdapter(&OpaqueHandle, "opaque")->SignatureSupported());
    Check("raw data buffer needs explicit policy",
          !MakeHleAdapter(&RawBuffer, "raw")->SignatureSupported());
    Check("varargs descriptor compiles and refuses",
          !MakeHleAdapter(&Varargs, "variadic")->SignatureSupported());
    Check("noexcept descriptor is explicitly unsupported",
          !MakeHleAdapter(&Noexcept, "noexcept")->SignatureSupported());
    Check("callback return refused", !a.Register(&ReturnCallback, "return_callback"));
    auto next = a.Register(&Add, "next");
    Check("refusals do not consume operations", next && next.Value() == size + 1);
    HleCallFrame f{};
    f.operation = 1;
    f.registers.Set(Gpr::Rdi, 4);
    f.registers.Set(Gpr::Rsi, 5);
    Check("original dispatch intact", a.Dispatch(f).HasValue() && f.registers.Get(Gpr::Rax) == 9);
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
