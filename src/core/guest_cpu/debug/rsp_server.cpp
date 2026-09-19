// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include "core/guest_cpu/debug/rsp_server.h"

namespace Core::GuestCpu::Debug {
namespace {
constexpr char digits[] = "0123456789abcdef";
int Digit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
std::optional<std::uint64_t> Number(std::string_view s) {
    std::uint64_t n{};
    auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), n, 16);
    if (s.empty() || ec != std::errc{} || end != s.data() + s.size())
        return {};
    return n;
}
std::string Hex(std::uint64_t n) {
    char buffer[17];
    auto [end, ec] = std::to_chars(buffer, buffer + 17, n, 16);
    return {buffer, end};
}
std::string Encode(std::span<const std::byte> bytes) {
    std::string r;
    for (auto b : bytes) {
        auto v = std::to_integer<unsigned>(b);
        r += digits[v >> 4];
        r += digits[v & 15];
    }
    return r;
}
std::string TextHex(std::string_view s) {
    return Encode(std::as_bytes(std::span{s.data(), s.size()}));
}
std::optional<std::vector<std::byte>> Decode(std::string_view s) {
    if (s.size() % 2 || s.size() > 8192)
        return {};
    std::vector<std::byte> r;
    for (size_t i = 0; i < s.size(); i += 2) {
        int a = Digit(s[i]), b = Digit(s[i + 1]);
        if (a < 0 || b < 0)
            return {};
        r.push_back(std::byte((a << 4) | b));
    }
    return r;
}
struct Register {
    std::string name;
    unsigned size;
    int gpr{-1};
    unsigned dwarf{};
};
const std::vector<Register>& Registers() {
    static const std::vector<Register> regs = [] {
        std::vector<Register> r;
        constexpr Gpr order[]{Gpr::Rax, Gpr::Rbx, Gpr::Rcx, Gpr::Rdx, Gpr::Rsi, Gpr::Rdi,
                              Gpr::Rbp, Gpr::Rsp, Gpr::R8,  Gpr::R9,  Gpr::R10, Gpr::R11,
                              Gpr::R12, Gpr::R13, Gpr::R14, Gpr::R15};
        constexpr unsigned dwarf[]{0, 3, 2, 1, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
        for (unsigned i = 0; i < 16; ++i)
            r.push_back({std::string(ToString(order[i])), 8, int(Index(order[i])), dwarf[i]});
        r.push_back({"rip", 8, -1, 16});
        r.push_back({"eflags", 4, -1, 49});
        for (auto s : {"cs", "ss", "ds", "es", "fs", "gs"})
            r.push_back({s, 4});
        for (unsigned i = 0; i < 8; ++i)
            r.push_back({"st" + std::to_string(i), 10, -1, 33 + i});
        for (auto s : {"fctrl", "fstat", "ftag", "fiseg", "fioff", "foseg", "fooff", "fop"})
            r.push_back({s, 4});
        for (unsigned i = 0; i < 16; ++i)
            r.push_back({"xmm" + std::to_string(i), 16, -1, 17 + i});
        r.push_back({"mxcsr", 4, -1, 64});
        r.push_back({"fs_base", 8, -1, 58});
        r.push_back({"gs_base", 8, -1, 59});
        return r;
    }();
    return regs;
}
std::string RegisterValue(unsigned n, const CpuSnapshot& s) {
    const auto& all = Registers();
    if (n >= all.size())
        return "E22";
    auto& r = all[n];
    auto& v = s.registers;
    const auto required = n < 16              ? RegisterValidity::Gpr
                          : n == 16           ? RegisterValidity::Rip
                          : n == 17           ? RegisterValidity::Rflags
                          : n >= 40 && n < 56 ? RegisterValidity::Xmm
                          : n == 56           ? RegisterValidity::Mxcsr
                          : n >= 57           ? RegisterValidity::SegmentBases
                                              : RegisterValidity::None;
    if (required == RegisterValidity::None || !HasAll(v.validity, required))
        return std::string(r.size * 2, 'x');
    std::uint64_t value{};
    if (n < 16)
        value = v.gpr[r.gpr];
    else if (n == 16)
        value = v.rip;
    else if (n == 17)
        value = v.rflags;
    else if (n >= 40 && n < 56)
        return Encode(std::as_bytes(std::span{&v.xmm[n - 40], 1}));
    else if (n == 56)
        value = v.mxcsr;
    else if (n == 57)
        value = v.fs_base;
    else if (n == 58)
        value = v.gs_base;
    else
        return std::string(r.size * 2, 'x'); // never fabricate absent x87/segment state
    return Encode(std::as_bytes(std::span{&value, 1}).first(r.size));
}
bool SetRegister(RegisterPatch& patch, unsigned n, std::span<const std::byte> bytes) {
    const auto& r = Registers();
    if (n >= r.size() || bytes.size() != r[n].size)
        return false;
    std::uint64_t value{};
    std::memcpy(&value, bytes.data(), std::min(bytes.size(), sizeof(value)));
    if (n < 16) {
        patch.fields = patch.fields | RegisterValidity::Gpr;
        patch.gpr_mask |= 1u << r[n].gpr;
        patch.values.gpr[r[n].gpr] = value;
    } else if (n == 16) {
        patch.fields = patch.fields | RegisterValidity::Rip;
        patch.values.rip = value;
    } else if (n == 17) {
        patch.fields = patch.fields | RegisterValidity::Rflags;
        patch.values.rflags = value;
    } else if (n >= 40 && n < 56) {
        patch.fields = patch.fields | RegisterValidity::Xmm;
        patch.xmm_mask |= 1u << (n - 40);
        std::memcpy(&patch.values.xmm[n - 40], bytes.data(), 16);
    } else if (n == 56) {
        patch.fields = patch.fields | RegisterValidity::Mxcsr;
        patch.values.mxcsr = value;
    } else if (n == 57 || n == 58) {
        patch.fields = patch.fields | RegisterValidity::SegmentBases;
        (n == 57 ? patch.values.fs_base : patch.values.gs_base) = value;
    } else
        return false;
    return true;
}
std::string Xml() {
    std::string s =
        "<?xml version=\"1.0\"?><target><architecture>i386:x86-64</architecture><feature "
        "name=\"org.gnu.gdb.i386.core\">";
    for (unsigned i = 0; i < Registers().size(); ++i) {
        if (i == 40)
            s += "</feature><feature name=\"org.gnu.gdb.i386.sse\">";
        if (i == 57)
            s += "</feature><feature name=\"org.gnu.gdb.i386.segments\">";
        const auto& r = Registers()[i];
        s += "<reg name=\"" + r.name + "\" bitsize=\"" + std::to_string(r.size * 8) +
             "\" regnum=\"" + std::to_string(i) + "\"";
        if (i == 16)
            s += " type=\"code_ptr\" generic=\"pc\"";
        if (i == 7)
            s += " type=\"data_ptr\" generic=\"sp\"";
        if (i == 6)
            s += " type=\"data_ptr\" generic=\"fp\"";
        s += " group=\"" + std::string(i < 24 ? "general" : "float") + "\"/>";
    }
    return s + "</feature></target>";
}
const char* PhaseName(Phase p) {
    switch (p) {
    case Phase::Ready:
        return "ready";
    case Phase::Guest:
        return "guest-running";
    case Phase::Hle:
        return "hle-boundary-read-only";
    case Phase::Parked:
        return "guest-stopped";
    }
    return "unknown";
}
std::string ThreadDescription(const Thread& t) {
    auto description = std::string(PhaseName(t.phase)) + " guest=" + std::to_string(t.guest_tid) +
                       " host=" + std::to_string(t.host_tid) +
                       " gen=" + std::to_string(t.handle.generation);
    if (t.snapshot) {
        description += " snapshot=" + std::string(ToString(t.snapshot->kind)) +
                       " epoch=" + std::to_string(t.snapshot->stop_epoch) +
                       " invocation=" + std::to_string(t.snapshot->invocation_id);
        if (t.phase == Phase::Hle && t.snapshot->kind == SnapshotKind::HleBoundary)
            description += " hle-op=" + std::to_string(t.snapshot->registers.Get(Gpr::Rax));
    }
    return description;
}
std::string JsonString(std::string_view input) {
    std::string out = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : input) {
        if (c == '\\' || c == '"') {
            out += '\\';
            out += c;
        } else if (c < 32) {
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 15];
        } else
            out += c;
    }
    return out + "\"";
}
std::string StackJson(const MixedStack& stack) {
    std::string out = "{\"schemaVersion\":1,\"contextId\":" + std::to_string(stack.context_id) +
                      ",\"threadId\":" + std::to_string(stack.thread_id) +
                      ",\"generation\":" + std::to_string(stack.generation) +
                      ",\"stopEpoch\":" + std::to_string(stack.stop_epoch) +
                      ",\"phase\":" + JsonString(stack.phase) +
                      ",\"limitation\":" + JsonString(stack.limitation) + ",\"frames\":[";
    bool first = true;
    for (auto& f : stack.frames) {
        if (!first)
            out += ',';
        first = false;
        out += "{\"kind\":" + JsonString(f.kind) + ",\"provenance\":" + JsonString(f.provenance) +
               ",\"module\":" + JsonString(f.module) + ",\"symbol\":" + JsonString(f.symbol) +
               ",\"pc\":\"0x" + Hex(f.pc) + "\",\"sp\":\"0x" + Hex(f.sp) + "\",\"fp\":\"0x" +
               Hex(f.fp) + "\",\"invocation\":" + std::to_string(f.invocation) +
               ",\"operation\":" + std::to_string(f.operation) + "}";
    }
    return out + "]}";
}
} // namespace

struct RspServer::Impl {
    Target& target;
    int listener{-1};
    std::atomic<int> client{-1};
    std::uint16_t port{};
    std::jthread worker;
    std::uint64_t selected{}, continuing{};
    bool no_ack{}, detach{}, awaiting{};
    bool await_new_stop{};
    std::map<std::uint64_t, std::uint64_t> previous_stops;
    std::string last, thread_xml, library_xml, mixed_json;
    std::uint64_t mixed_thread{};
    std::chrono::steady_clock::time_point interrupt_deadline{};
    explicit Impl(Target& t) : target(t) {}
    ~Impl() {
        worker.request_stop();
        if (auto fd = client.load(); fd >= 0)
            ::shutdown(fd, SHUT_RDWR);
        if (worker.joinable())
            worker.join();
        if (listener >= 0)
            ::close(listener);
    }
    bool SendRaw(std::string_view s) {
        while (!s.empty()) {
            const auto n = ::send(client.load(), s.data(), s.size(), MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                return false;
            s.remove_prefix(n);
        }
        return true;
    }
    bool Send(std::string_view s) {
        std::string encoded;
        unsigned sum{};
        for (unsigned char c : s) {
            if (c == '$' || c == '#' || c == '}' || c == '*') {
                encoded += '}';
                sum += '}';
                c ^= 0x20;
            }
            encoded += char(c);
            sum += c;
        }
        last = "$" + encoded + "#" + digits[(sum >> 4) & 15] + digits[sum & 15];
        return SendRaw(last);
    }
    std::optional<Thread> Selected() {
        auto threads = target.Threads();
        for (auto& t : threads)
            if (t.handle.id == selected)
                return t;
        if (!selected && !threads.empty()) {
            selected = threads.front().handle.id;
            return threads.front();
        }
        return {};
    }
    std::optional<std::string> StopReply() {
        auto threads = target.Threads();
        if (threads.empty())
            return {};
        auto fresh = [&](const auto& t) {
            return t.phase == Phase::Parked && t.snapshot &&
                   t.snapshot->stop_epoch > previous_stops[t.handle.id];
        };
        if (await_new_stop && std::none_of(threads.begin(), threads.end(), fresh))
            return {};
        // Native HLE may still be waiting/working. Its guest continuation is
        // held at the next debug gate; only its immutable HLE snapshot is read.
        if (std::any_of(threads.begin(), threads.end(),
                        [](auto& t) { return t.phase == Phase::Guest; }))
            return {};
        auto eligible = [&](const auto& t) {
            return t.phase == Phase::Parked && (!await_new_stop || fresh(t));
        };
        auto it = std::find_if(threads.begin(), threads.end(), [&](auto& t) {
            return eligible(t) &&
                   (t.reason == StopReason::Watchpoint || t.reason == StopReason::Breakpoint ||
                    t.reason == StopReason::StepComplete);
        });
        if (it == threads.end())
            it = std::find_if(threads.begin(), threads.end(), eligible);
        if (it == threads.end())
            it = threads.begin();
        if (!it->snapshot)
            return {};
        selected = it->handle.id;
        std::string r = "T05thread:" + Hex(selected) + ";10:" + RegisterValue(16, *it->snapshot) +
                        ";07:" + RegisterValue(7, *it->snapshot) + ";";
        // Refusal is still an actual new owner stop. An E packet here would
        // leave clients believing Continue was still running and hide recovery.
        if (it->reason == StopReason::Unsupported)
            r += "shadps4-stop:unsupported;";
        if (it->reason == StopReason::Breakpoint)
            r += "swbreak:;";
        if (it->reason == StopReason::Watchpoint && it->watch_hit) {
            const auto& hit = *it->watch_hit;
            r += std::string(hit.access == WatchAccess::Read    ? "rwatch:"
                             : hit.access == WatchAccess::Write ? "watch:"
                                                                : "awatch:") +
                 Hex(hit.watched_address) + ";watch-rip:" + Hex(hit.instruction_rip) +
                 ";watch-address:" + Hex(hit.address) + ";watch-size:" + Hex(hit.bytes) + ";";
        }
        return r;
    }
    bool Resume(std::uint64_t id, bool step) {
        previous_stops.clear();
        for (auto& t : target.Threads())
            if (t.snapshot)
                previous_stops[t.handle.id] = t.snapshot->stop_epoch;
        auto status = target.Continue(id, step);
        if (!status)
            return Send("E16");
        awaiting = true;
        await_new_stop = true;
        interrupt_deadline = {};
        return true;
    }
    bool Handle(std::string_view p) {
        if (p.starts_with("qSupported"))
            return Send("PacketSize=2000;qXfer:features:read+;qXfer:threads:read+;qXfer:libraries:"
                        "read+;QStartNoAckMode+;swbreak+;qXfer:shadps4-mixed-stack:read+");
        if (p == "QStartNoAckMode") {
            bool ok = Send("OK");
            no_ack = true;
            return ok;
        }
        if (p == "qAttached")
            return Send("1");
        if (p == "qC") {
            auto t = Selected();
            return Send(t ? "QC" + Hex(t->handle.id) : "E16");
        }
        if (p == "qHostInfo")
            return Send("triple:" + TextHex("x86_64-unknown-linux-gnu") +
                        ";endian:little;ptrsize:8;");
        if (p == "qProcessInfo")
            return Send("pid:" + Hex(::getpid()) + ";triple:" +
                        TextHex("x86_64-unknown-linux-gnu") + ";endian:little;ptrsize:8;");
        if (p == "qfThreadInfo") {
            std::string r = "m";
            for (auto& t : target.Threads()) {
                if (r.size() > 1)
                    r += ',';
                r += Hex(t.handle.id);
            }
            return Send(r == "m" ? "l" : r);
        }
        if (p == "qsThreadInfo")
            return Send("l");
        if (p.starts_with("qThreadExtraInfo,")) {
            auto id = Number(p.substr(17));
            if (!id)
                return Send("E22");
            for (auto& t : target.Threads())
                if (t.handle.id == *id)
                    return Send(TextHex(ThreadDescription(t)));
            return Send("E22");
        }
        constexpr std::string_view mixed_prefix = "qXfer:shadps4-mixed-stack:read:";
        if (p.starts_with(mixed_prefix)) {
            auto args = p.substr(mixed_prefix.size());
            auto colon = args.find(':');
            auto comma = args.find(',', colon == args.npos ? 0 : colon);
            if (colon == args.npos || comma == args.npos)
                return Send("E22");
            auto id = Number(args.substr(0, colon)),
                 off = Number(args.substr(colon + 1, comma - colon - 1)),
                 len = Number(args.substr(comma + 1));
            if (!id || !*id || !off || !len || !*len)
                return Send("E22");
            if (!*off) {
                auto stack = target.ReadMixedStack(*id);
                if (!stack)
                    return Send("E16");
                mixed_json = StackJson(stack.Value());
                mixed_thread = *id;
            }
            if (mixed_thread != *id || *off > mixed_json.size())
                return Send("E22");
            const auto n = std::min<std::uint64_t>({*len, 4096, mixed_json.size() - *off});
            return Send(std::string(*off + n == mixed_json.size() ? "l" : "m") +
                        mixed_json.substr(*off, n));
        }
        if (p.starts_with("qXfer:threads:read::") || p.starts_with("qXfer:libraries:read::")) {
            const bool threads = p.starts_with("qXfer:threads:");
            auto arg = p.substr(threads ? std::string_view("qXfer:threads:read::").size()
                                        : std::string_view("qXfer:libraries:read::").size());
            auto comma = arg.find(',');
            auto off = Number(arg.substr(0, comma)),
                 len = comma == arg.npos ? std::nullopt : Number(arg.substr(comma + 1));
            if (!off || !len || !*len)
                return Send("E22");
            auto& xml = threads ? thread_xml : library_xml;
            if (!*off) {
                if (threads) {
                    xml = "<threads>";
                    for (auto& t : target.Threads()) {
                        xml += "<thread id=\"" + Hex(t.handle.id) + "\" name=\"" +
                               ThreadDescription(t) + "\">" +
                               (t.phase == Phase::Parked ? "Runnable" : "Waiting") + "</thread>";
                    }
                    xml += "</threads>";
                } else {
                    xml = "<library-list>";
                    for (auto& m : target.Modules()) {
                        std::string name;
                        for (char c : m.name) {
                            if (c == '&')
                                name += "&amp;";
                            else if (c == '<')
                                name += "&lt;";
                            else if (c == '\"')
                                name += "&quot;";
                            else
                                name += c;
                        }
                        xml += "<library name=\"" + name + "\" load-base=\"0x" + Hex(m.load_base) +
                               "\">";
                        for (auto& segment : m.segments)
                            xml += "<segment address=\"0x" + Hex(segment.base.value) + "\"/>";
                        xml += "</library>";
                    }
                    xml += "</library-list>";
                }
            }
            if (*off > xml.size())
                return Send("E22");
            auto n = std::min<std::uint64_t>({*len, xml.size() - *off, 4096});
            return Send(std::string(*off + n == xml.size() ? "l" : "m") + xml.substr(*off, n));
        }
        if (p.starts_with("qXfer:features:read:target.xml:")) {
            auto arg = p.substr(31);
            auto comma = arg.find(',');
            auto off = Number(arg.substr(0, comma)),
                 len = comma == arg.npos ? std::nullopt : Number(arg.substr(comma + 1));
            auto xml = Xml();
            if (!off || !len || *len == 0 || *off > xml.size())
                return Send("E22");
            auto n = std::min<std::uint64_t>({*len, xml.size() - *off, 4096});
            return Send(std::string(*off + n == xml.size() ? "l" : "m") + xml.substr(*off, n));
        }
        if (p.starts_with("qRegisterInfo")) {
            auto n = Number(p.substr(13));
            if (!n || *n >= Registers().size())
                return Send("E45");
            auto& r = Registers()[*n];
            unsigned offset = 0;
            for (unsigned i = 0; i < *n; ++i)
                offset += Registers()[i].size;
            std::string reply = "name:" + r.name + ";bitsize:" + std::to_string(r.size * 8) +
                                ";offset:" + std::to_string(offset) +
                                ";encoding:uint;format:hex;set:Guest x86-64;";
            if (r.dwarf || *n == 0)
                reply += "dwarf:" + std::to_string(r.dwarf) +
                         ";ehframe:" + std::to_string(r.dwarf) + ";";
            if (*n == 16)
                reply += "generic:pc;";
            if (*n == 7)
                reply += "generic:sp;";
            if (*n == 6)
                reply += "generic:fp;";
            if (*n == 17)
                reply += "generic:flags;";
            return Send(reply);
        }
        if (p.size() >= 2 && p[0] == 'H' && (p[1] == 'g' || p[1] == 'c')) {
            auto v = p.substr(2);
            auto id = (v == "-1" || v == "0") ? std::optional<std::uint64_t>(0) : Number(v);
            if (!id)
                return Send("E22");
            auto ts = target.Threads();
            if (*id &&
                std::none_of(ts.begin(), ts.end(), [&](auto& t) { return t.handle.id == *id; }))
                return Send("E22");
            (p[1] == 'g' ? selected : continuing) = *id;
            return Send("OK");
        }
        if (p.starts_with("T")) {
            auto id = Number(p.substr(1));
            for (auto& t : target.Threads())
                if (id && t.handle.id == *id)
                    return Send("OK");
            return Send("E22");
        }
        if (p == "?") {
            if (!target.Pause())
                return Send("E16");
            awaiting = true;
            await_new_stop = false;
            interrupt_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            return true;
        }
        if (p == "D") {
            auto r = target.Detach();
            if (!r)
                return Send("E16");
            detach = true;
            return Send("OK");
        }
        if (p == "vCont?")
            return Send("vCont;c;s");
        if (p.starts_with("vCont;")) {
            auto a = p.substr(6);
            // Multi-action requests are refused atomically, never partially run.
            if (a.empty() || a.find(';') != a.npos || (a[0] != 'c' && a[0] != 's'))
                return Send("E22");
            std::uint64_t id = 0;
            if (a.size() > 1) {
                if (a[1] != ':')
                    return Send("E22");
                auto n = Number(a.substr(2));
                if (!n)
                    return Send("E22");
                id = *n;
            }
            if (a[0] == 's' && !id) {
                auto t = Selected();
                if (!t)
                    return Send("E16");
                id = t->handle.id;
            }
            return Resume(id, a[0] == 's');
        }
        if (p == "c" || p == "s") {
            auto t = Selected();
            return Resume(p == "s" ? (continuing ? continuing : (t ? t->handle.id : 0))
                                   : continuing,
                          p == "s");
        }
        if (p == "g" || p.starts_with("p")) {
            auto t = Selected();
            if (!t || !t->snapshot || t->phase == Phase::Guest)
                return Send("E16");
            if (p == "g") {
                std::string r;
                for (unsigned i = 0; i < Registers().size(); ++i)
                    r += RegisterValue(i, *t->snapshot);
                return Send(r);
            }
            auto n = Number(p.substr(1));
            return Send(n ? RegisterValue(*n, *t->snapshot) : "E22");
        }
        if (p.starts_with("P") || p.starts_with("G")) {
            auto t = Selected();
            if (!t || !t->snapshot || t->phase != Phase::Parked)
                return Send("E16");
            RegisterPatch patch{};
            patch.values = t->snapshot->registers;
            if (p[0] == 'P') {
                auto equal = p.find('=');
                auto n = Number(p.substr(1, equal - 1));
                auto bytes = equal == p.npos ? std::nullopt : Decode(p.substr(equal + 1));
                if (!n || !bytes || !SetRegister(patch, *n, *bytes))
                    return Send("E22");
            } else {
                size_t offset = 1;
                for (unsigned i = 0; i < Registers().size(); ++i) {
                    auto len = Registers()[i].size * 2;
                    if (offset + len > p.size())
                        return Send("E22");
                    auto field = p.substr(offset, len);
                    offset += len;
                    if (field == std::string(len, 'x'))
                        continue;
                    auto b = Decode(field);
                    if (!b || !SetRegister(patch, i, *b))
                        return Send("E22");
                }
                if (offset != p.size())
                    return Send("E22");
            }
            return Send(target.WriteStoppedRegisters(t->handle, patch, t->snapshot->stop_epoch)
                            ? "OK"
                            : "E16");
        }
        if (p.starts_with("m") || p.starts_with("M")) {
            auto colon = p.find(':');
            auto arg = p.substr(1, colon == p.npos ? p.size() - 1 : colon - 1);
            auto comma = arg.find(',');
            auto addr = Number(arg.substr(0, comma)),
                 len = comma == arg.npos ? std::nullopt : Number(arg.substr(comma + 1));
            if (!addr || !len || !*len || *len > 4096)
                return Send("E22");
            if (p[0] == 'm') {
                if (colon != p.npos)
                    return Send("E22");
                std::vector<std::byte> b(*len);
                return Send(target.ReadMemory(GuestAddress{*addr}, b) ? Encode(b) : "E14");
            }
            auto b = colon == p.npos ? std::nullopt : Decode(p.substr(colon + 1));
            if (!b || b->size() != *len)
                return Send("E22");
            return Send(target.WriteMemory(GuestAddress{*addr}, *b) ? "OK" : "E16");
        }
        if (p.starts_with("Z") || p.starts_with("z")) {
            if (p.size() < 3 || (p[1] != '0' && (p[1] < '2' || p[1] > '4')) || p[2] != ',')
                return Send("");
            auto comma = p.find(',', 3);
            auto addr = Number(p.substr(3, comma - 3)),
                 kind = comma == p.npos ? std::nullopt : Number(p.substr(comma + 1));
            if (!addr || !kind || !*kind || (p[1] == '0' ? *kind != 1 : *kind > 8))
                return Send("E22");
            if (p[1] != '0') {
                const auto access = p[1] == '2'   ? WatchAccess::Write
                                    : p[1] == '3' ? WatchAccess::Read
                                                  : WatchAccess::Access;
                return Send(target.SetWatchpoint({GuestAddress{*addr}, *kind}, access, p[0] == 'Z')
                                ? "OK"
                                : "E16");
            }
            return Send(target.Breakpoint(GuestAddress{*addr}, p[0] == 'Z') ? "OK" : "E16");
        }
        // Unknown packets are unsupported, never an invented OK.
        return Send("");
    }
    void Serve(std::stop_token stop) {
        selected = continuing = mixed_thread = 0;
        mixed_json.clear();
        no_ack = detach = awaiting = await_new_stop = false;
        last.clear();
        std::string wire;
        unsigned checksum{};
        int state = 0, first{};
        while (!stop.stop_requested() && !detach) {
            if (awaiting) {
                if (auto reply = StopReply()) {
                    awaiting = false;
                    if (!Send(*reply))
                        break;
                } else if (interrupt_deadline.time_since_epoch().count() &&
                           std::chrono::steady_clock::now() > interrupt_deadline) {
                    awaiting = false;
                    if (!Send("E16"))
                        break;
                }
            }
            pollfd fd{client.load(), POLLIN, 0};
            int ready = ::poll(&fd, 1, 20);
            if (ready < 0 && errno == EINTR)
                continue;
            if (ready < 0 || fd.revents & (POLLERR | POLLHUP | POLLNVAL))
                break;
            if (!ready)
                continue;
            char c{};
            if (::recv(fd.fd, &c, 1, 0) != 1)
                break;
            if (state == 0) {
                if (c == '$') {
                    wire.clear();
                    checksum = 0;
                    state = 1;
                } else if (c == '-' && !no_ack) {
                    if (!SendRaw(last))
                        break;
                } else if (c == 3) {
                    (void)target.Pause();
                    awaiting = true;
                    await_new_stop = false;
                    interrupt_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                }
                continue;
            }
            if (state == 1) {
                if (c == '#') {
                    state = 2;
                    continue;
                }
                if (wire.size() >= 8192)
                    break;
                wire += c;
                checksum += static_cast<unsigned char>(c);
                continue;
            }
            if (state == 2) {
                first = Digit(c);
                state = 3;
                continue;
            }
            state = 0;
            int second = Digit(c);
            if (first < 0 || second < 0 || ((first << 4) | second) != (checksum & 255)) {
                if (!no_ack && !SendRaw("-"))
                    break;
                continue;
            }
            if (!no_ack && !SendRaw("+"))
                break;
            std::string decoded;
            bool escaped = false;
            for (char b : wire) {
                if (escaped) {
                    decoded += char(b ^ 0x20);
                    escaped = false;
                } else if (b == '}')
                    escaped = true;
                else
                    decoded += b;
            }
            if (escaped) {
                if (!Send("E22"))
                    break;
                continue;
            }
            if (!Handle(decoded))
                break;
        }
        if (!detach) {
            (void)target.Pause();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!target.Detach() && std::chrono::steady_clock::now() < deadline &&
                   !stop.stop_requested())
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
};
RspServer::RspServer(std::unique_ptr<Impl> value) : impl(std::move(value)) {}
RspServer::~RspServer() = default;
std::uint16_t RspServer::Port() const {
    return impl->port;
}
Result<std::unique_ptr<RspServer>> RspServer::Listen(Target& target, std::uint16_t port) {
    auto impl = std::make_unique<Impl>(target);
    impl->listener = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (impl->listener < 0)
        return MakeError(ErrorCategory::BackendFailure, "guest debugger", "socket failed");
    int reuse = 1;
    ::setsockopt(impl->listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(impl->listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) ||
        ::listen(impl->listener, 1))
        return MakeError(ErrorCategory::BackendFailure, "guest debugger",
                         "loopback bind/listen failed");
    socklen_t length = sizeof(addr);
    ::getsockname(impl->listener, reinterpret_cast<sockaddr*>(&addr), &length);
    impl->port = ntohs(addr.sin_port);
    auto* ptr = impl.get();
    impl->worker = std::jthread([ptr](std::stop_token stop) {
        while (!stop.stop_requested()) {
            pollfd fd{ptr->listener, POLLIN, 0};
            if (::poll(&fd, 1, 50) <= 0)
                continue;
            int client = ::accept4(ptr->listener, nullptr, nullptr, SOCK_CLOEXEC);
            if (client < 0)
                continue;
            timeval timeout{1, 0};
            ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
            ptr->client.store(client);
            ptr->Serve(stop);
            ptr->client.store(-1);
            ::close(client);
        }
    });
    return std::unique_ptr<RspServer>(new RspServer(std::move(impl)));
}
} // namespace Core::GuestCpu::Debug
