// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstring>
#if defined(__ANDROID__)
#include <android/log.h>
#include <sys/system_properties.h>
#endif
#include "common/logging/log.h"
#include "common/profiler.h"
#include "core/libraries/kernel/kernel.h"
#include "guest_storage_hle.h"
namespace Core::HostRuntime {
using namespace GuestCpu;
namespace {
struct Mount2 {
    s32 user;
    u32 pad;
    u64 directory, blocks;
    u32 mode;
    std::array<u8, 36> reserved;
};
struct Mount1 {
    s32 user;
    u32 pad;
    u64 title, directory, fingerprint, blocks;
    u32 mode;
    std::array<u8, 36> reserved;
};
static_assert(sizeof(Mount2) == 64 && sizeof(Mount1) == 80);
template <class T>
bool Copy(GuestAddressSpace& space, u64 address, T& value) {
    return bool(
        space.ReadData(GuestAddress{address}, std::as_writable_bytes(std::span{&value, 1})));
}
std::optional<std::string> String(GuestAddressSpace& space, u64 address, size_t max) {
    std::string out;
    if (!address || address > UINT64_MAX - max)
        return {};
    for (size_t i = 0; i < max; ++i) {
        char c{};
        if (!Copy(space, address + i, c))
            return {};
        if (!c)
            return out;
        out += c;
    }
    return {};
}
} // namespace
static u64 DispatchStoragePinned(GuestStorage& storage, GuestAddressSpace& space,
                                 const StorageEntry& e, const std::array<u64, 6>& a,
                                 const std::function<u64(int)>& posix_failure) {
    // Data leases preserve only the buffers of this request. No execution gate
    // spans storage calls, cursor waits or guest errno publication.
    std::optional<Common::Profiler::Scope> phase;
    if (storage.TraceIo())
        phase.emplace("Storage.DataAdmission");
    phase.reset();
    if (storage.TraceIo()) phase.emplace("Storage.Marshal");
    auto slow = [&](auto&& work) {
        phase.reset();
        std::optional<Common::Profiler::Scope> operation;
        if (storage.TraceIo()) operation.emplace("Storage.NativeOperation");
        return work();
    };
    using SE = GuestStorage::Error;
    auto error = [&](int native) -> u64 {
        if (e.save)
            return static_cast<u32>(SE::PARAMETER);
        int posix = Libraries::Kernel::NativeToPosixErrno(native);
        return e.posix ? posix_failure(posix)
                       : static_cast<u64>(s64(Libraries::Kernel::ErrnoToSceKernelError(posix)));
    };
    auto io = [&](GuestStorage::IoResult r) -> u64 {
        if (r.error)
            LOG_WARNING(Lib_SaveData, "Session file nid={} error={} result={}", e.nid, r.error,
                        r.value);
        return r.error ? error(r.error) : u64(r.value);
    };
    // No guest pin crosses filesystem work. Re-admit every output with all
    // mapping identities, then publish the batch atomically with respect to VM retirement.
    auto publish = [&](std::vector<GuestAddressSpace::DataRequest> requests, auto&& operation) -> u64 {
        std::vector<std::vector<GuestAddressSpace::MappingIdentity>> identities(requests.size());
        {
            auto pins=space.AcquireDataBatch(requests);if(!pins)return error(EFAULT);
            for(size_t i=0;i<requests.size();++i)for(u64 at=requests[i].range.base.value;at<requests[i].range.End();) {
                auto m=space.Query(GuestAddress{at});if(!m)return error(EFAULT);
                const auto end=std::min(requests[i].range.End(),m.Value().range.End());
                identities[i].push_back({at,end,m.Value().mapping_generation});at=end;
            }
        }
        std::vector<std::vector<u8>> outputs(requests.size());
        const auto result=slow([&]{return operation(outputs);});
        if(result!=SE::OK)return u32(result);
        for(size_t i=0;i<requests.size();++i)requests[i].identities=identities[i];
        auto pins=space.AcquireDataBatch(requests);if(!pins)return error(EFAULT);
        for(size_t i=0;i<outputs.size();++i) {
            if(outputs[i].size()!=requests[i].range.size)return error(EINVAL);
        }
        for(size_t i=0;i<outputs.size();++i)std::memcpy(pins.Value()[i].WritableBytes().data(),outputs[i].data(),outputs[i].size());
        return 0;
    };
    switch (e.op) {
    case StorageOp::SetupMemory: {
        Libraries::SaveData::OrbisSaveDataParam param{};
        if (a[2] && !Copy(space,a[2],param)) return error(EFAULT);
        return u32(slow([&]{return storage.SetupMemory(s32(a[0]),a[1],a[2]?&param:nullptr);}));
    }
    case StorageOp::GetMemory:
    case StorageOp::SetMemory: {
        if (a[2]>64*1024*1024 || !a[1]) return error(EINVAL);
        std::vector<u8> bytes(a[2]);
        const bool write=e.op==StorageOp::SetMemory;
        if (write) {
            if (!space.ReadData(GuestAddress{a[1]},std::as_writable_bytes(std::span{bytes}))) return error(EFAULT);
            return u32(slow([&]{return storage.Memory(s32(a[0]),bytes,s64(a[3]),true);}));
        }
        if (!a[2]) return u32(storage.Memory(s32(a[0]),bytes,s64(a[3]),false));
        return publish({{{GuestAddress{a[1]},a[2]},GuestPermission::Write}},
            [&](auto& outputs){auto rc=storage.Memory(s32(a[0]),bytes,s64(a[3]),false);outputs[0]=std::move(bytes);return rc;});
    }
    case StorageOp::SaveIcon: {
        struct Icon {u64 buffer,size,data_size;std::array<u8,32> reserved;};
        Icon icon{};auto point=String(space,a[0],16);
        if (!point || !Copy(space,a[1],icon) || !icon.buffer || icon.size>4*1024*1024) return error(EFAULT);
        std::vector<u8> bytes(std::min(icon.size,icon.data_size));
        if (!bytes.empty() && !space.ReadData(GuestAddress{icon.buffer},std::as_writable_bytes(std::span{bytes}))) return error(EFAULT);
        return u32(slow([&]{return storage.SaveIcon(*point,bytes);}));
    }
    case StorageOp::Search: {
        using namespace Libraries::SaveData;
        OrbisSaveDataDirNameSearchCond cond{};OrbisSaveDataDirNameSearchResult result{};
        if (!Copy(space,a[0],cond)||!Copy(space,a[1],result)||result.dirNamesNum>4096) return error(EFAULT);
        const auto original=result;
        OrbisSaveDataTitleId title{};OrbisSaveDataDirName pattern{};
        if (cond.titleId) {auto text=String(space,reinterpret_cast<u64>(cond.titleId),10);if(!text)return error(EFAULT);title.data.FromString(*text);cond.titleId=&title;}
        if (cond.dirName) {auto text=String(space,reinterpret_cast<u64>(cond.dirName),32);if(!text)return error(EFAULT);pattern.data.FromString(*text);cond.dirName=&pattern;}
        std::vector<GuestAddressSpace::DataRequest> requests{{{GuestAddress{a[1]},sizeof(result)},GuestPermission::Write}};
        std::vector<OrbisSaveDataDirName> names(result.dirNamesNum);
        std::vector<OrbisSaveDataParam> params(result.params?result.dirNamesNum:0);
        std::vector<OrbisSaveDataSearchInfo> infos(result.infos?result.dirNamesNum:0);
        auto add=[&](auto* pointer,auto& data) {
            if(data.empty())return true;
            const u64 address=reinterpret_cast<u64>(pointer), size=data.size()*sizeof(data[0]);
            if(!space.ReadData(GuestAddress{address},std::as_writable_bytes(std::span{data})))return false;
            requests.push_back({{GuestAddress{address},size},GuestPermission::Write});return true;
        };
        if(!add(result.dirNames,names)||!add(result.params,params)||!add(result.infos,infos))return error(EFAULT);
        result.dirNames=names.data();result.params=params.empty()?nullptr:params.data();result.infos=infos.empty()?nullptr:infos.data();
        return publish(std::move(requests),[&](auto& outputs){
            const auto rc=storage.Search(cond,result);
            auto visible=original;visible.hitNum=result.hitNum;visible.setNum=result.setNum;
            auto bytes=[](const auto& value){const auto view=std::as_bytes(std::span{value});return std::vector<u8>(reinterpret_cast<const u8*>(view.data()),reinterpret_cast<const u8*>(view.data())+view.size());};
            outputs[0]=bytes(std::span{&visible,1});size_t index=1;
            if(!names.empty())outputs[index++]=bytes(std::span{names});
            if(!params.empty())outputs[index++]=bytes(std::span{params});
            if(!infos.empty())outputs[index++]=bytes(std::span{infos});
            return rc;
        });
    }
    case StorageOp::GetDents:
    case StorageOp::GetDirEntries: {
        if (a[2] < 512 || a[2] > u64(INT64_MAX)) return error(EINVAL);
        const auto size = std::min<u64>(a[2], 64 * 1024);
        std::vector<GuestAddressSpace::DataRequest> requests{
            {{GuestAddress{a[1]}, size}, GuestPermission::Write}};
        if (e.op == StorageOp::GetDirEntries && a[3])
            requests.push_back({{GuestAddress{a[3]}, sizeof(s64)}, GuestPermission::Write});
        auto pins = space.AcquireDataBatch(requests);
        if (!pins)
            return error(EFAULT);
        std::vector<u8> data(size);
        s64 base{};
        const auto result = slow([&] { return storage.GetDents(s32(a[0]), data, &base); });
        if (!result.error) {
            std::memcpy(pins.Value()[0].WritableBytes().data(), data.data(), result.value);
            if (pins.Value().size() == 2)
                std::memcpy(pins.Value()[1].WritableBytes().data(), &base, sizeof(base));
        }
        return io(result);
    }
    case StorageOp::Stat:
    case StorageOp::Fstat: {
        using Stat = Libraries::Kernel::OrbisKernelStat;
        auto pin = space.AcquireDataSpan({GuestAddress{a[1]}, sizeof(Stat)}, true);
        if (!pin)
            return error(EFAULT);
        Stat value{};
        GuestStorage::IoResult result{};
        if (e.op == StorageOp::Stat) {
            auto path = String(space, a[0], 1024);
            if (!path)
                return error(EFAULT);
            result = slow([&] { return storage.Stat(*path, value); });
        } else {
            result = slow([&] { return storage.Fstat(s32(a[0]), value); });
        }
        if (!result.error)
            std::memcpy(pin.Value().WritableBytes().data(), &value, sizeof(value));
        return io(result);
    }
    case StorageOp::Truncate:
        return io(slow([&] { return storage.Truncate(s32(a[0]), s64(a[1])); }));
    case StorageOp::Pread:
    case StorageOp::Pwrite:
    case StorageOp::Preadv:
    case StorageOp::Pwritev: {
        const bool vector = e.op == StorageOp::Preadv || e.op == StorageOp::Pwritev;
        const bool write = e.op == StorageOp::Pwrite || e.op == StorageOp::Pwritev;
        struct Iovec {
            u64 address, length;
        };
        std::vector<Iovec> input;
        if (s64(a[3]) < 0)
            return error(EINVAL);
        if (vector) {
            const s32 count = s32(a[2]);
            if (count < 0 || count > 1024)
                return error(EINVAL);
            input.resize(count);
            if (count &&
                !space.ReadData(GuestAddress{a[1]}, std::as_writable_bytes(std::span{input})))
                return error(EFAULT);
        } else {
            input.push_back({a[1], a[2]});
        }
        u64 total{};
        for (const auto& item : input) {
            if (item.length > u64(INT64_MAX) - total)
                return error(EINVAL);
            total += item.length;
        }
        if (total > u64(INT64_MAX) - a[3])
            return error(EOVERFLOW);
        // One bounded regular-file admission; partial I/O is legal. Validate all
        // buffers participating in it before disk I/O, including later vectors.
        u64 remaining = std::min<u64>(total, 16 * 1024 * 1024);
        std::vector<GuestAddressSpace::DataRequest> requests;
        for (const auto& item : input) {
            if (!remaining)
                break;
            const u64 length = std::min(remaining, item.length);
            if (!length)
                continue;
            requests.push_back({{GuestAddress{item.address}, length},
                                write ? GuestPermission::Read : GuestPermission::Write});
            remaining -= length;
        }
        auto pinned = space.AcquireDataBatch(requests);
        if (!pinned)
            return error(EFAULT);
        std::vector<GuestStorage::Buffer> buffers;
        for (auto& pin : pinned.Value()) {
            auto* data =
                write ? const_cast<std::byte*>(pin.Bytes().data()) : pin.WritableBytes().data();
            buffers.push_back({data, pin.Bytes().size()});
        }
        return io(slow([&] { return storage.Positioned(s32(a[0]), buffers, s64(a[3]), write); }));
    }
    case StorageOp::Event: {
        auto pin = space.AcquireDataSpan({GuestAddress{a[1]}, sizeof(GuestStorage::Event)}, true);
        if (!pin)
            return error(EFAULT);
        GuestStorage::Event value{};
        auto r = storage.GetEvent(value);
        if (r == SE::OK)
            std::memcpy(pin.Value().WritableBytes().data(), &value, sizeof(value));
        return u32(r);
    }
    case StorageOp::CheckBackup:
    case StorageOp::RestoreBackup:
    case StorageOp::Delete: {
        struct Input {
            s32 user;
            u32 pad;
            u64 title, directory, param, icon;
            std::array<u8, 32> reserved;
        };
        Input in{};
        // Delete/Restore are 72 bytes; CheckBackup is 72 bytes with nested outputs.
        if (!space.ReadData(GuestAddress{a[0]}, std::as_writable_bytes(std::span{&in, 1})
                                                    .first(e.op == StorageOp::Delete ? 64 : 72)))
            return error(EFAULT);
        auto dir = String(space, in.directory, 32);
        if (!dir)
            return error(EFAULT);
        std::string title;
        if (in.title) {
            auto t = String(space, in.title, 10);
            if (!t)
                return error(EFAULT);
            title = *t;
        }
        if (e.op == StorageOp::Delete)
            return u32(storage.Delete(in.user, title, *dir));
        if (e.op == StorageOp::RestoreBackup) {
            std::array<u8, 80> fingerprint{};
            if (in.param && !Copy(space, in.param, fingerprint))
                return error(EFAULT);
            return u32(storage.RestoreBackup(in.user, title, *dir));
        }
        struct Icon {
            u64 buffer, capacity, size;
            std::array<u8, 32> reserved;
        };
        Icon icon{};
        if (in.icon && (!Copy(space, in.icon, icon) || icon.capacity > 4 * 1024 * 1024))
            return error(EFAULT);
        std::vector<GuestAddressSpace::DataRequest> requests;
        if (in.param)
            requests.push_back(
                {{GuestAddress{in.param}, sizeof(Libraries::SaveData::OrbisSaveDataParam)},
                 GuestPermission::Write});
        if (in.icon) {
            requests.push_back({{GuestAddress{in.icon}, sizeof(icon)}, GuestPermission::Write});
            if (icon.capacity)
                requests.push_back(
                    {{GuestAddress{icon.buffer}, icon.capacity}, GuestPermission::Write});
        }
        auto pinned = space.AcquireDataBatch(requests);
        if (!pinned)
            return error(EFAULT);
        size_t index{};
        PinnedSpan* param = in.param ? &pinned.Value()[index++] : nullptr;
        PinnedSpan* icon_out = in.icon ? &pinned.Value()[index++] : nullptr;
        PinnedSpan* icon_data = in.icon && icon.capacity ? &pinned.Value()[index++] : nullptr;
        Libraries::SaveData::OrbisSaveDataParam metadata{};
        std::vector<u8> bytes;
        auto r = storage.CheckBackup(in.user, title, *dir, metadata, bytes);
        if (r == SE::OK) {
            if (param)
                std::memcpy(param->WritableBytes().data(), &metadata, sizeof(metadata));
            if (icon_out) {
                icon.size = bytes.size();
                std::memcpy(icon_out->WritableBytes().data(), &icon, sizeof(icon));
                if (icon_data)
                    std::memcpy(icon_data->WritableBytes().data(), bytes.data(),
                                std::min<u64>(bytes.size(), icon.capacity));
            }
        }
        return u32(r);
    }
    case StorageOp::Init:
        return static_cast<u32>(storage.Initialize());
    case StorageOp::Term:
        return static_cast<u32>(storage.Terminate());
    case StorageOp::Mount:
    case StorageOp::Mount2: {
        Mount2 m{};
        std::string title;
        if (e.op == StorageOp::Mount) {
            Mount1 legacy{};
            if (!Copy(space, a[0], legacy))
                return error(EFAULT);
            m.user = legacy.user;
            m.directory = legacy.directory;
            m.blocks = legacy.blocks;
            m.mode = legacy.mode;
            if (legacy.title) {
                auto t = String(space, legacy.title, 10);
                if (!t)
                    return error(EFAULT);
                title = *t;
            }
            // Fingerprint is optional opaque input; never dereference it natively.
            std::array<u8, 80> fingerprint{};
            if (legacy.fingerprint && !Copy(space, legacy.fingerprint, fingerprint))
                return error(EFAULT);
        } else if (!Copy(space, a[0], m))
            return error(EFAULT);
        auto dir = String(space, m.directory, 32);
        if (!dir)
            return error(EFAULT);
        auto out =
            space.AcquireDataSpan({GuestAddress{a[1]}, sizeof(GuestStorage::MountResult)}, true);
        if (!out)
            return error(EFAULT);
        GuestStorage::MountResult value{};
        auto result = storage.Mount(m.user, title, *dir, m.blocks, m.mode, value);
        if (result == SE::OK || result == SE::NO_SPACE_FS)
            std::memcpy(out.Value().WritableBytes().data(), &value, sizeof(value));
        return static_cast<u32>(result);
    }
    case StorageOp::UnmountBackup:
    case StorageOp::Unmount:
    case StorageOp::Info:
    case StorageOp::GetParam:
    case StorageOp::SetParam: {
        auto point = String(space, a[0], 16);
        if (!point)
            return error(EFAULT);
        if (e.op == StorageOp::UnmountBackup)
            return static_cast<u32>(storage.UnmountBackup(*point));
        if (e.op == StorageOp::Unmount)
            return static_cast<u32>(storage.Unmount(*point));
        if (e.op == StorageOp::Info) {
            auto pin =
                space.AcquireDataSpan({GuestAddress{a[1]}, sizeof(GuestStorage::MountInfo)}, true);
            if (!pin)
                return error(EFAULT);
            GuestStorage::MountInfo value{};
            auto result = storage.Info(*point, value);
            if (result == SE::OK)
                std::memcpy(pin.Value().WritableBytes().data(), &value, sizeof(value));
            return static_cast<u32>(result);
        }
        if (a[3] == 0 || a[3] > sizeof(Libraries::SaveData::OrbisSaveDataParam))
            return error(EINVAL);
        if (e.op == StorageOp::SetParam) {
            std::vector<u8> local(a[3]);
            if (!space.ReadData(GuestAddress{a[2]}, std::as_writable_bytes(std::span{local})))
                return error(EFAULT);
            return static_cast<u32>(storage.SetParam(*point, a[1], local));
        }
        std::vector<GuestAddressSpace::DataRequest> requests{
            {{GuestAddress{a[2]}, a[3]}, GuestPermission::Write}};
        if (a[4])
            requests.push_back({{GuestAddress{a[4]}, 8}, GuestPermission::Write});
        auto pinned = space.AcquireDataBatch(requests);
        if (!pinned)
            return error(EFAULT);
        std::vector<u8> local(a[3]);
        u64 size{};
        auto result = storage.GetParam(*point, a[1], local, size);
        if (result == SE::OK) {
            std::memcpy(pinned.Value()[0].WritableBytes().data(), local.data(), size);
            if (a[4])
                std::memcpy(pinned.Value()[1].WritableBytes().data(), &size, 8);
        }
        return static_cast<u32>(result);
    }
    case StorageOp::Open:
    case StorageOp::Mkdir:
    case StorageOp::Rmdir:
    case StorageOp::Unlink:
    case StorageOp::Rename: {
        auto path = String(space, a[0], 1024);
        if (!path)
            return error(EFAULT);
        if (e.op == StorageOp::Open) {
            const auto result = slow([&] { return storage.Open(*path, a[1], a[2]); });
            LOG_DEBUG(Lib_SaveData, "Session open path={} flags={:#x} fd={} error={}", *path,
                      a[1], result.value, result.error);
#if defined(__ANDROID__)
            // Opt-in immediate output survives a guest native crash before the
            // ordinary file logger flushes. It never changes open/errno results.
            char diagnostic[PROP_VALUE_MAX]{};
            if (__system_property_get("debug.shadps4.storage_log", diagnostic) > 0 &&
                diagnostic[0] == '1')
                __android_log_print(ANDROID_LOG_INFO, "GuestStorage", "open path=%s flags=%x fd=%lld error=%d",
                                    path->c_str(), u32(a[1]), (long long)result.value, result.error);
#endif
            return io(result);
        }
        if (e.op == StorageOp::Mkdir)
            return io(slow([&] { return storage.Mkdir(*path, a[1]); }));
        if (e.op == StorageOp::Rmdir)
            return io(slow([&] { return storage.Rmdir(*path); }));
        if (e.op == StorageOp::Unlink)
            return io(slow([&] { return storage.Unlink(*path); }));
        auto to = String(space, a[1], 1024);
        if (!to)
            return error(EFAULT);
        return io(slow([&] { return storage.Rename(*path, *to); }));
    }
    case StorageOp::Close:
        return io(slow([&] { return storage.Close(s32(a[0])); }));
    case StorageOp::Seek:
        return io(slow([&] { return storage.Seek(s32(a[0]), s64(a[1]), s32(a[2])); }));
    case StorageOp::Sync:
        return io(slow([&] { return storage.Sync(s32(a[0])); }));
    case StorageOp::Read:
    case StorageOp::Write: {
        // Regular files only; bound one I/O admission so Cancel has an HLE boundary.
        if (a[2] > u64(INT64_MAX))
            return error(EINVAL);
        const auto count = std::min<u64>(a[2], 16 * 1024 * 1024);
        if (!a[2])
            return e.op == StorageOp::Read ? io(slow([&] { return storage.Read(a[0], {}); }))
                                           : io(slow([&] { return storage.Write(a[0], {}); }));
        auto pin = space.AcquireDataSpan({GuestAddress{a[1]}, count}, e.op == StorageOp::Read);
        if (!pin)
            return error(EFAULT);
        if (e.op == StorageOp::Read) {
            auto bytes = pin.Value().WritableBytes();
            return io(slow([&] { return storage.Read(a[0], {reinterpret_cast<u8*>(bytes.data()), bytes.size()}); }));
        }
        auto bytes = pin.Value().Bytes();
        return io(slow([&] { return storage.Write(a[0], {reinterpret_cast<const u8*>(bytes.data()), bytes.size()}); }));
    }
    }
    return error(EINVAL);
}
u64 DispatchStorage(GuestStorage& storage, GuestAddressSpace& space, const StorageEntry& entry,
                    const std::array<u64, 6>& args, const std::function<u64(int)>& posix_failure) {
    // Retire the completed I/O's references before entering the errno callback.
    std::optional<int> deferred_errno;
    const auto result = DispatchStoragePinned(storage, space, entry, args, [&](int error) {
        deferred_errno = error;
        return UINT64_MAX;
    });
    return deferred_errno ? posix_failure(*deferred_errno) : result;
}
} // namespace Core::HostRuntime
