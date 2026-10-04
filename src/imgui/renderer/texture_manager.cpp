// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <utility>

#include <imgui.h>
#include "common/assert.h"
#include "common/io_file.h"
#include "common/polyfill_thread.h"
#include "common/stb.h"
#include "common/thread.h"
#include "imgui_core.h"
#include "spatial/imgui/VulkanRenderer.hpp"
#include "texture_manager.h"

namespace ImGui {

namespace Core::TextureManager {
struct Inner {
    std::atomic_int count = 0;
    // Published by the worker once the renderer owns the decoded pixels; width and height are set
    // before it.
    std::atomic<ImTextureID> texture_id = nullptr;
    u32 width = 0;
    u32 height = 0;

    ~Inner();
};
} // namespace Core::TextureManager

using namespace Core::TextureManager;

RefCountedTexture::RefCountedTexture(Inner* inner) : inner(inner) {
    ++inner->count;
}

RefCountedTexture RefCountedTexture::DecodePngTexture(std::vector<u8> data) {
    const auto core = new Inner;
    Core::TextureManager::DecodePngTexture(std::move(data), core);
    return RefCountedTexture(core);
}

RefCountedTexture RefCountedTexture::DecodePngFile(std::filesystem::path path) {
    const auto core = new Inner;
    Core::TextureManager::DecodePngFile(std::move(path), core);
    return RefCountedTexture(core);
}

RefCountedTexture::RefCountedTexture() : inner(nullptr) {}

RefCountedTexture::RefCountedTexture(const RefCountedTexture& other) : inner(other.inner) {
    if (inner != nullptr) {
        ++inner->count;
    }
}

RefCountedTexture::RefCountedTexture(RefCountedTexture&& other) noexcept : inner(other.inner) {
    other.inner = nullptr;
}

RefCountedTexture& RefCountedTexture::operator=(const RefCountedTexture& other) {
    if (this == &other)
        return *this;
    inner = other.inner;
    if (inner != nullptr) {
        ++inner->count;
    }
    return *this;
}

RefCountedTexture& RefCountedTexture::operator=(RefCountedTexture&& other) noexcept {
    if (this == &other)
        return *this;
    std::swap(inner, other.inner);
    return *this;
}

RefCountedTexture::~RefCountedTexture() {
    if (inner != nullptr) {
        if (inner->count.fetch_sub(1) == 1) {
            delete inner;
        }
    }
}

RefCountedTexture::Image RefCountedTexture::GetTexture() const {
    if (inner == nullptr) {
        return {};
    }
    const ImTextureID id = inner->texture_id.load(std::memory_order_acquire);
    if (id == nullptr) {
        return {};
    }
    return Image{
        .im_id = id,
        .width = inner->width,
        .height = inner->height,
    };
}

RefCountedTexture::operator bool() const {
    return inner != nullptr && inner->texture_id.load(std::memory_order_acquire) != nullptr;
}

struct Job {
    Inner* core;
    std::vector<u8> data;
    std::filesystem::path path;
};

static std::atomic_bool g_is_worker_running = false;
static std::jthread g_worker_thread;
static std::condition_variable g_worker_cv;

static std::mutex g_job_list_mtx;
static std::deque<Job> g_job_list;

namespace Core::TextureManager {

Inner::~Inner() {
    // The renderer destroys the texture once the frames that may draw it have completed.
    if (const ImTextureID id = texture_id.load(std::memory_order_acquire)) {
        Core::ReleaseTexture(id);
    }
}

static void Release(Inner* core) {
    if (core->count.fetch_sub(1) == 1) {
        delete core;
    }
}

void WorkerLoop() {
    Common::SetCurrentThreadName("shadPS4:ImGuiTextureManager");
    while (g_is_worker_running) {
        std::unique_lock lk{g_job_list_mtx};
        g_worker_cv.wait(lk, [] { return !g_is_worker_running || !g_job_list.empty(); });
        if (!g_is_worker_running) {
            break;
        }
        lk.unlock();
        while (true) {
            g_job_list_mtx.lock();
            if (g_job_list.empty()) {
                g_job_list_mtx.unlock();
                break;
            }
            auto [core, png_raw, path] = std::move(g_job_list.front());
            g_job_list.pop_front();
            g_job_list_mtx.unlock();

            if (!path.empty()) { // Decode PNG from file
                Common::FS::IOFile file(path, Common::FS::FileAccessMode::Read);
                if (!file.IsOpen()) {
                    LOG_ERROR(ImGui, "Failed to open PNG file: {}", path.string());
                    Release(core);
                    continue;
                }
                png_raw.resize(file.GetSize());
                file.Seek(0);
                file.ReadRaw<u8>(png_raw.data(), png_raw.size());
                file.Close();
            }

            int width, height;
            stbi_uc* pixels =
                stbi_load_from_memory(png_raw.data(), png_raw.size(), &width, &height, nullptr, 4);
            if (pixels == nullptr) {
                LOG_ERROR(ImGui, "Failed to decode PNG: {}", stbi_failure_reason());
                Release(core);
                continue;
            }
            // The renderer copies the pixels and records their upload into the next frame: no
            // submission or GPU wait happens here.
            auto* renderer = Core::Renderer();
            spatial::imgui::VulkanTexture* texture =
                renderer ? renderer->CreateTexture(pixels, width, height) : nullptr;
            stbi_image_free(pixels);
            if (texture != nullptr) {
                core->width = width;
                core->height = height;
                core->texture_id.store(texture, std::memory_order_release);
            }
            Release(core);
        }
    }
}

void StartWorker() {
    ASSERT(!g_is_worker_running);
    g_is_worker_running = true;
    g_worker_thread = std::jthread(WorkerLoop);
}

void StopWorker() {
    ASSERT(g_is_worker_running);
    { std::scoped_lock lock(g_job_list_mtx); g_is_worker_running = false; }
    g_worker_cv.notify_one();
    if (g_worker_thread.joinable()) g_worker_thread.join();
    // Jobs never decoded still hold a reference to their texture.
    std::deque<Job> abandoned;
    {
        std::scoped_lock lock(g_job_list_mtx);
        abandoned.swap(g_job_list);
    }
    for (Job& job : abandoned) {
        Release(job.core);
    }
}

void DecodePngTexture(std::vector<u8> data, Inner* core) {
    ++core->count;
    Job job{
        .core = core,
        .data = std::move(data),
    };
    std::unique_lock lk{g_job_list_mtx};
    g_job_list.push_back(std::move(job));
    g_worker_cv.notify_one();
}

void DecodePngFile(std::filesystem::path path, Inner* core) {
    ++core->count;
    Job job{
        .core = core,
        .path = std::move(path),
    };
    std::unique_lock lk{g_job_list_mtx};
    g_job_list.push_back(std::move(job));
    g_worker_cv.notify_one();
}

} // namespace Core::TextureManager

} // namespace ImGui
