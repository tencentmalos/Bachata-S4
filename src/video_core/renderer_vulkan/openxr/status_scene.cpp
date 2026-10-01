// SPDX-License-Identifier: GPL-2.0-or-later
#include "status_scene.h"
#include "psv_indicator.h"
#include "status_projection.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <android/asset_manager.h>
#include <fmt/format.h>
#include "spatial/engine/Engine.h"
#include "spatial/engine/ViewRenderer.h"
#include "spatial/engine/VoxelGi.h"
#include "spatial/gltf/GltfModel.h"
#include "spatial/platform/android/JniHelper.h"
#include "spatial/scene/BasicScene.h"
#include "spatial/scene/LightContent.h"
#include "spatial/scene/MeshContent.h"
#include "spatial/scene/SceneNode.h"
#include "spatial/xr/XrMath.h"
#include "spatial/xr/XrSceneVulkanLayer.h"
// CPU text rasterization is independent of both ImGui contexts. Fonts are baked
// once; only changed status pixels are staged, at most four times a second.
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"

namespace Vulkan::OpenXr {
namespace {
using Clock = std::chrono::steady_clock;
namespace ov = spatial::imgui::overlay;
namespace sc = spatial::scene;
namespace math = spatial::math;
struct Options { bool visible{true}, gi{true}, cropped{true}, psvr_title{}; int theme{}, indicator{-1}, layout{}; uint64_t recenter{}; };
std::mutex control_mutex;
Options options;
std::string diagnostics = "PSV status scene not created";
constexpr std::array<const char*, 3> themes{"graphite", "pearl", "blue"};
struct Palette {
    std::array<float,4> shell, rear, buttons;
    std::array<uint8_t,3> accent;
};
constexpr Palette palettes[]{
    {{.013f,.019f,.028f,1},{.022f,.027f,.035f,1},{.021f,.028f,.040f,1},{107,200,255}},
    {{.72f,.75f,.79f,1},{.40f,.43f,.49f,1},{.30f,.34f,.41f,1},{166,193,255}},
    {{.020f,.075f,.25f,1},{.012f,.025f,.07f,1},{.018f,.028f,.050f,1},{97,228,219}},
};
std::vector<uint8_t> ReadAsset(const char* path) {
    auto* manager = spatial::platform::JniHelper::getAssetManager();
    std::unique_ptr<AAsset, decltype(&AAsset_close)> asset(
        manager ? AAssetManager_open(manager, path, AASSET_MODE_BUFFER) : nullptr, AAsset_close);
    if (!asset) throw std::runtime_error(std::string("PSV asset missing: ") + path);
    auto size = AAsset_getLength64(asset.get());
    if (size <= 0 || size > 8 * 1024 * 1024) throw std::runtime_error("Invalid PSV asset size");
    std::vector<uint8_t> bytes(size);
    if (AAsset_read(asset.get(), bytes.data(), bytes.size()) != size)
        throw std::runtime_error("Incomplete PSV asset read");
    return bytes;
}
// UTF-8 labels from PerfHud include degree signs and em dashes.
uint32_t Codepoint(std::string_view text, size_t& i) {
    uint32_t c = uint8_t(text[i++]);
    if (c < 128) return c;
    const unsigned count = c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
    c &= (1u << (6-count))-1;
    for (unsigned j=0;j<count && i<text.size();++j) c = (c<<6) | (uint8_t(text[i++])&63);
    return c;
}
struct Canvas {
    static constexpr unsigned W=960, H=544;
    struct Glyph { int w{}, h{}, x{}, y{}; float advance{}; std::vector<uint8_t> pixels; };
    std::array<Glyph,256> glyphs;
    std::vector<uint8_t> rgba = std::vector<uint8_t>(W*H*4);
    Canvas() {
        const auto bytes=ReadAsset("xr/NotoSans-Regular.ttf");
        stbtt_fontinfo font{};
        if (!stbtt_InitFont(&font,bytes.data(),stbtt_GetFontOffsetForIndex(bytes.data(),0)))
            throw std::runtime_error("PSV font is invalid");
        float scale=stbtt_ScaleForPixelHeight(&font,64);
        for (unsigned c=32;c<glyphs.size();++c) {
            auto& g=glyphs[c]; int adv{};
            stbtt_GetCodepointHMetrics(&font,c,&adv,nullptr); g.advance=adv*scale;
            auto* bitmap=stbtt_GetCodepointBitmap(&font,0,scale,c,&g.w,&g.h,&g.x,&g.y);
            if (bitmap) { g.pixels.assign(bitmap,bitmap+g.w*g.h); stbtt_FreeBitmap(bitmap,nullptr); }
        }
    }
    void Rect(int x,int y,int w,int h,std::array<uint8_t,3> c) {
        for(int j=std::max(y,0);j<std::min(y+h,int(H));++j)
            for(int i=std::max(x,0);i<std::min(x+w,int(W));++i) {
                auto* p=&rgba[(j*W+i)*4]; std::copy(c.begin(),c.end(),p); p[3]=255;
            }
    }
    void Text(int x,int baseline,std::string_view text,float size,std::array<uint8_t,3> c,int max_width=880) {
        float scale=size/64, pen=float(x);
        for(size_t i=0;i<text.size();) {
            uint32_t cp=Codepoint(text,i);
            if(cp==0x2014 || cp==0x2013)cp='-';
            const auto& g=glyphs[cp>=32 && cp<glyphs.size()?cp:'?'];
            if(pen+g.advance*scale>x+max_width)break;
            for(int j=0;j<int(std::ceil(g.h*scale));++j) for(int k=0;k<int(std::ceil(g.w*scale));++k) {
                int dx=int(pen+g.x*scale)+k,dy=baseline+int(g.y*scale)+j;
                if(dx<0 || dy<0 || dx>=int(W) || dy>=int(H))continue;
                const int sx=std::clamp(int(k/scale),0,g.w-1),sy=std::clamp(int(j/scale),0,g.h-1);
                const unsigned a=g.pixels[sy*g.w+sx]; auto* p=&rgba[(dy*W+dx)*4];
                for(unsigned n=0;n<3;++n)p[n]=(p[n]*(255-a)+c[n]*a+127)/255;
            }
            pen+=g.advance*scale;
        }
    }
    void Draw(const ov::StatusSnapshot& status, bool psvr, uint32_t w,uint32_t h,int theme,bool gi) {
        const auto accent=palettes[theme].accent;
        constexpr std::array<uint8_t,3> white{235,242,250},dim{149,168,190};
        Rect(0,0,W,H,{10,17,28}); Rect(28,62,904,2,accent);
        Text(30,44,"shadPS4",34,white); Text(650,43,"LIVE STATUS",25,accent);
        const bool stale=status.sampled_at==Clock::time_point{} || Clock::now()-status.sampled_at>std::chrono::seconds(2);
        const auto fps=stale?std::optional<double>(0):status.presentation_fps;
        Text(30,97,"GAME FPS",22,dim);
        Text(26,177,fps?fmt::format("{:.1f}",*fps):"--",92,accent,310);
        Text(30,213,fps && *fps>0 ? fmt::format("{:.1f} ms / frame",1000 / *fps):"Waiting for game frames",25,dim,460);
        Text(500,113,psvr?"PSVR / STEREO":"XR / CINEMA",32,white,430);
        Text(500,158,fmt::format("{} x {} / eye",w,h),28,dim,430);
        Text(500,204,gi?"White area light  /  GI ON":"White area light  /  GI OFF",22,accent,430);
        const std::array<std::string_view,6> keys{"perf.cpu","perf.gpu","perf.ram","perf.battery","perf.power","perf.battery_temp"};
        const std::array<std::string_view,6> labels{"CPU","GPU","MEMORY","BATTERY","POWER","BATTERY TEMP"};
        for(unsigned n=0;n<keys.size();++n) {
            const int x=28+(n%2)*466,y=237+(n/2)*88;
            Rect(x,y,438,78,{19,29,43});
            Text(x+16,y+25,labels[n],21,dim,402);
            auto it=std::find_if(status.summary_items.begin(),status.summary_items.end(),[&](const auto& s){return s.id.value()==keys[n];});
            Text(x+16,y+62,it!=status.summary_items.end() && !stale?it->value:"--",34,white,402);
        }
        Text(30,532,stale?"Waiting for status samples":"New game frames  /  Host device metrics",21,dim);
    }
};
} // namespace

void ConfigureStatus(int layout, bool psvr_title) {
    std::scoped_lock lock(control_mutex);
    options.visible = layout != 2;
    options.layout = layout == 1 ? 1 : 0;
    options.psvr_title = psvr_title;
    diagnostics = psvr_title ? "PSVR ImGui status pending; model disabled" : "Cinema PSV status pending";
}
StatusSettings ReadStatusSettings() {
    std::scoped_lock lock(control_mutex);
    return {options.visible, options.psvr_title, options.theme, options.layout};
}
void ReportStatusLayer(std::string text) {
    std::scoped_lock lock(control_mutex);
    diagnostics = std::move(text);
}

std::string StatusSceneCommand(const std::vector<std::string>& args) {
    std::scoped_lock lock(control_mutex);
    if(args.empty() || (args.size()==1 && args[0]=="status")) {
        return fmt::format("visible={} theme={} gi={} indicator={}\n{}",options.visible,themes[options.theme],options.gi,
            options.indicator<0?"auto":PsvIndicatorNames[options.indicator],diagnostics);
    }
    if(args.size()==1 && args[0]=="recenter") { ++options.recenter; return "PSV recenter queued"; }
    if(args.size()==2) {
        if(args[0]=="projection" && (args[1]=="full" || args[1]=="cropped")) {
            options.cropped=args[1]=="cropped"; return "PSV projection queued";
        }
        if(args[0]=="indicator") {
            if(args[1]=="auto") {options.indicator=-1;return "PSV indicator follows session/battery";}
            for(int i=0;i<int(PsvIndicatorNames.size());++i)
                if(args[1]==PsvIndicatorNames[i]) {options.indicator=i;return "PSV indicator preview queued";}
        }
        if(args[0]=="theme") {
            for(int i=0;i<3;++i) if(args[1]==themes[i]) {options.theme=i;return "PSV theme queued";}
        } else if((args[0]=="visible" || args[0]=="gi") && (args[1]=="on" || args[1]=="off")) {
            (args[0]=="gi"?options.gi:options.visible)=args[1]=="on"; return "XR status setting queued";
        }
    }
    return "Usage: xr_status status | visible on/off | gi on/off | theme graphite/pearl/blue | recenter | projection full/cropped | indicator auto/off/running/standby/charging/charge_low/notification";
}

struct StatusScene::Impl {
    spatial::xr::XrSceneVulkanLayer layer;
    std::unique_ptr<sc::BasicScene> scene;
    std::unique_ptr<sc::MeshContent> body;
    std::unique_ptr<sc::LightContent> light;
    std::unique_ptr<sc::LightContent> indicator_light;
    std::unique_ptr<spatial::engine::VoxelGi> gi;
    std::unique_ptr<Canvas> canvas;
    sc::SceneNode* root{};
    sc::SceneNode* lamp{};
    sc::ModelHandle model{};
    spatial::render::TextureId screen{};
    std::vector<XrCompositionLayerProjectionView> projection_views;
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    Clock::time_point last_update{},last_diagnostic{};
    Clock::time_point indicator_epoch{Clock::now()};
    std::optional<Clock::time_point> standby_since;
    PsvIndicator indicator_state{PsvIndicator::Running};
    std::array<float,4> indicator_emission{-1,-1,-1,-1};
    int theme{-1};
    bool last_gi{true}, rendered_cropped{true}, anchored{}, in_world{};
    std::optional<XrPosef> world_origin;
    float heading{};
    XrPosef anchor{spatial::xr::math::IdentityPose()};
    XrPosef rendered_anchor{spatial::xr::math::IdentityPose()};
    uint64_t frames{}, recenters{};
    std::string error;
    ~Impl() {
        if(layer.View())layer.View()->SetGi(nullptr);
        gi.reset();
        body.reset(); light.reset(); indicator_light.reset(); scene.reset();
        if(screen && layer.IsCreated())layer.Engine().Device().ReleaseTexture(screen);
        layer.Destroy();
        std::scoped_lock lock(control_mutex);diagnostics="PSV scene stopped";
    }
};
StatusScene::StatusScene():impl(std::make_unique<Impl>()) {}
StatusScene::~StatusScene()=default;
bool StatusScene::Create(XrSession session,const spatial::xr::SwapchainFunctions& functions,
                         const spatial::xr::XrImguiVulkanBinding& binding,uint32_t w,uint32_t h,std::string cache) {
    auto& p=*impl;
    spatial::xr::XrSceneDeviceFeatures features;
    features.keep_model_geometry=true; // actual CPU geometry, required by voxel GI
    if(!p.layer.Create(session,functions,binding,w,h,2,128,cache,{},features))
        throw std::runtime_error("PSV Lite Engine: "+p.layer.RendererError());
    auto bytes=ReadAsset("xr/psv_status.glb");
    auto gltf=std::make_shared<spatial::gltf::Model>();
    if(!spatial::gltf::ParseGlb(bytes.data(),bytes.size(),*gltf,p.error))throw std::runtime_error(p.error);
    if(gltf->FindNode("status_display")<0)throw std::runtime_error("PSV model has no status_display");
    p.model=p.layer.UploadModel(gltf,false,spatial::xr::XrModelLook::Pbr);
    if(p.model==sc::InvalidModel)throw std::runtime_error("PSV upload: "+p.layer.RendererError());
    p.scene=std::make_unique<sc::BasicScene>("psv_status");
    p.root=p.scene->GetRootSceneNode()->CreateChildSceneNode("psv_root");
    p.body=std::make_unique<sc::MeshContent>("psv",gltf);p.body->SetRenderHandle(p.model);
    p.root->AttachComponent(p.body.get());p.root->SetScale(3.8f,3.8f,3.8f);
    p.canvas=std::make_unique<Canvas>();p.canvas->Draw({},true,0,0,0,true);
    p.screen=p.layer.Engine().Device().UploadTextureRgba8(p.canvas->rgba,
        {.width=Canvas::W,.height=Canvas::H},p.error);
    if(!p.screen)throw std::runtime_error("PSV screen: "+p.error);
    auto* resource=p.layer.Engine().FindModel(p.model);bool bound=false;
    for(size_t i=0;i<gltf->materials.size();++i)if(gltf->materials[i].name=="status_screen") {
        auto& material=resource->Instances().at(i);
        bound=material.SetTexture("base_color_texture",p.screen);
        material.SetCustomBits(material.CustomBits() | 1u); // unlit HAS_BASE_COLOR_MAP
    }
    if(!bound)throw std::runtime_error("PSV screen material binding failed");
    p.light=std::make_unique<sc::LightContent>("psv_white_softbox");
    p.light->SetType(sc::LightType::Rect);p.light->SetSize(.65f,.45f);
    p.light->SetColour({.95f,.975f,1.f});p.light->SetIntensity(8.f);p.light->SetRange(3.f);
    p.lamp=p.scene->GetRootSceneNode()->CreateChildSceneNode("psv_softbox");p.lamp->AttachComponent(p.light.get());
    // Keep geometry/lights fixed in panel space. Recenter moves its LOCAL-space
    // anchor without rebuilding voxel GI. Head motion only changes the cameras.
    p.root->SetPosition({0,-.42f,-1.15f});
    p.root->SetOrientation(math::quatf(std::cos(-.16f),std::sin(-.16f),0,0));
    p.lamp->SetPosition({-.25f,.03f,-.60f});
    p.lamp->SetOrientation(math::quatf(std::cos(-.28f),std::sin(-.28f),0,0));
    p.indicator_light=std::make_unique<sc::LightContent>("psv_ps_key_light");
    p.indicator_light->SetType(sc::LightType::Point);
    p.indicator_light->SetRange(.15f);p.indicator_light->SetIntensity(.002f);
    auto* led=p.root->CreateChildSceneNode("psv_ps_key_light");
    led->SetPosition({-.071f,-.026f,.014f});led->AttachComponent(p.indicator_light.get());
    p.layer.SetEnvironment({.light_intensity=0.f});
    p.gi=std::make_unique<spatial::engine::VoxelGi>(p.layer.Engine());
    p.gi->SetSettings({.enabled=true,.voxel_size=.012f,.probes_per_frame=128,.hysteresis=.7f});
    p.layer.View()->SetGi(p.gi.get());
    {std::scoped_lock lock(control_mutex);diagnostics=fmt::format("created triangles={} screen={}x{} eye={}x{}",gltf->TriangleCount(),Canvas::W,Canvas::H,w,h);}
    return true;
}
void StatusScene::Recenter(){impl->anchored=false;}
void StatusScene::Place(const std::optional<XrPosef>& origin) {
    auto& p=*impl;
    p.world_origin=origin;
    const bool in_world=origin.has_value();
    if(in_world==p.in_world)return;
    p.in_world=in_world;
    // Authored cinema coordinates (assets/xr/cinema): the PSV stands on its
    // dock at 0.66 m, 1.05 m in front of the seated eye, scale 2.5.
    if(in_world) {
        p.root->SetPosition({0,.66f,-1.05f});p.root->SetScale(2.5f,2.5f,2.5f);
        p.lamp->SetPosition({-.25f,1.48f,-.60f});
    } else {
        p.root->SetPosition({0,-.42f,-1.15f});p.root->SetScale(3.8f,3.8f,3.8f);
        p.lamp->SetPosition({-.25f,.03f,-.60f});
    }
    p.anchored=false;
}
bool StatusScene::Render(std::span<const XrView> eyes,XrSpace space,const ov::StatusSnapshot& status,
                         const std::optional<spatial::perf::DeviceMetrics>& device,bool active,
                         bool psvr,uint32_t output_width,uint32_t output_height) {
    auto& p=*impl; Options config;
    {std::scoped_lock lock(control_mutex);config=options;}
    if(!config.visible || eyes.size()!=2)return false;
    if(p.recenters!=config.recenter){p.recenters=config.recenter;p.anchored=false;}
    const std::array<XrView,2> located{eyes[0],eyes[1]};
    if(p.world_origin) {
        p.anchor=*p.world_origin; // follows the cinema anchor and its recenter
        p.anchored=true;
    } else if(!p.anchored) {
        p.anchor=StatusProjection::Anchor(located,p.heading);
        p.anchored=true;
    }
    auto scene_views=StatusProjection::LocalViews(located,p.anchor);
    if(config.cropped) {
        const auto& bounds=p.body->GetWorldBoundingBox();
        for(auto& view:scene_views)
            // When bounds cross the eye's near plane, keep the full FOV and
            // let normal clipping handle them instead of making the panel pop.
            StatusProjection::Crop(spatial::xr::math::FromFoundation(bounds.min),
                spatial::xr::math::FromFoundation(bounds.max),view);
    }

    if(p.theme!=config.theme) {
        const auto& palette=palettes[config.theme];
        p.layer.SetModelParam(p.model,"base_color_factor",palette.shell,"shell");
        p.layer.SetModelParam(p.model,"base_color_factor",palette.rear,"rear_shell");
        p.layer.SetModelParam(p.model,"base_color_factor",palette.buttons,"buttons");
        p.theme=config.theme;p.last_update={};
    }
    if(p.last_gi!=config.gi) {auto settings=p.gi->Settings();settings.enabled=config.gi;p.gi->SetSettings(settings);p.last_gi=config.gi;p.last_update={};}
    const auto now=Clock::now();
    if(active)p.standby_since.reset();
    else if(!p.standby_since)p.standby_since=now;
    const bool charging=device && now-device->sampled_at<std::chrono::seconds(3) && device->battery.charging;
    auto indicator_state=active ? PsvIndicator::Running :
        now-*p.standby_since<std::chrono::milliseconds(1200) ? PsvIndicator::Standby :
        charging ? PsvIndicator::Charging : PsvIndicator::Off;
    // No invented low-battery threshold or fake notifications. These two
    // documented Vita states can be inspected explicitly through DebugBus.
    if(config.indicator>=0)indicator_state=static_cast<PsvIndicator>(config.indicator);
    if(indicator_state!=p.indicator_state) {p.indicator_state=indicator_state;p.indicator_epoch=now;}
    const auto led=IndicatorLight(indicator_state,std::chrono::duration<double>(now-p.indicator_epoch).count());
    const std::array<float,4> emission{led.colour[0]*2*led.brightness,led.colour[1]*2*led.brightness,led.colour[2]*2*led.brightness,0};
    if(emission!=p.indicator_emission) {
        p.layer.SetModelParam(p.model,"emissive_factor",emission,"ps_indicator");p.indicator_emission=emission;
    }
    p.indicator_light->SetColour({led.colour[0],led.colour[1],led.colour[2]});
    p.indicator_light->SetIntensity(.002f*led.brightness);
    if(now-p.last_update>=std::chrono::milliseconds(250)) {
        p.canvas->Draw(status,psvr,output_width,output_height,p.theme,config.gi);
        if(!p.layer.Engine().Device().UpdateTexture(p.screen,{0,0,0,Canvas::W,Canvas::H,1},std::as_bytes(std::span(p.canvas->rgba)),p.error))
            throw std::runtime_error("PSV screen update: "+p.error);
        p.last_update=now;
    }
    p.scene->TickFrame();
    if(!p.layer.Render(scene_views,*p.scene,{.ambient=.035f}))return false;
    if(!p.layer.LastRenderReused()) {
        p.rendered_anchor=p.anchor;
        p.rendered_cropped=config.cropped;
    }
    if(!p.layer.FillProjectionLayer(space,p.projection_views,p.projection))return false;
    // Restore the camera's actual LOCAL-space pose for compositor reprojection.
    // Reused pixels retain the anchor from the render that produced them.
    for(auto& view:p.projection_views)
        view.pose=StatusProjection::ToWorld(p.rendered_anchor,view.pose);
    // Lite Engine emits premultiplied colour, including the transparent caps.
    p.projection.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    ++p.frames;
    if(now-p.last_diagnostic>=std::chrono::seconds(1)) {
        const auto& g=p.gi->Stats();const auto e=p.layer.Engine().Stats();
        std::scoped_lock lock(control_mutex);
        diagnostics=fmt::format("frames={} reused={} mode={} eye={}x{} triangles={} voxels={} probes={} invalid={} skipped={} sweeps={} rebuilds={} rect_transfers={} gi_ms={:.3f}/{:.3f}/{:.3f} variants={}/{} failed={} indicator={} brightness={:.2f}\n{}",
            p.frames,p.layer.LastRenderReused(),psvr?"psvr":"cinema",p.layer.EyeWidth(),p.layer.EyeHeight(),g.triangles,g.voxels,g.probes,g.probes_invalid,g.models_skipped,g.sweeps,g.rebuilds,g.rect_transfers,g.build_ms,g.inject_ms,g.trace_ms,e.variants_compiled,e.variants_requested,e.variants_failed,PsvIndicatorNames[static_cast<int>(indicator_state)],led.brightness,p.layer.RendererError());
        diagnostics+=fmt::format("\nprojection={} samples=1 screen={}x{} anchor=local-fixed offset=0,-0.42,-1.15",
            p.rendered_cropped?"cropped-symmetric":"full",Canvas::W,Canvas::H);
        const auto at=spatial::xr::math::Transform(p.rendered_anchor,{0,-.42f,-1.15f});
        diagnostics+=fmt::format(" world_position={:.3f},{:.3f},{:.3f}",at.x,at.y,at.z);
        for(unsigned i=0;i<p.projection_views.size();++i) {
            const auto& f=p.projection_views[i].fov;
            constexpr float deg=180.f/spatial::xr::math::Pi;
            diagnostics+=fmt::format("\neye{} fov_deg=L{:.2f},R{:.2f},U{:.2f},D{:.2f}",i,
                f.angleLeft*deg,f.angleRight*deg,f.angleUp*deg,f.angleDown*deg);
        }
        p.last_diagnostic=now;
    }
    return true;
}
const XrCompositionLayerProjection* StatusScene::Layer()const{return &impl->projection;}
} // namespace Vulkan::OpenXr
