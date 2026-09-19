#include <oboe/Oboe.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <thread>
#include <pthread.h>
#include <unistd.h>
struct Callback final : oboe::AudioStreamDataCallback {
    std::atomic<int> calls{},frames{},policy{-1},priority{-1},min{1000000},max{};
    oboe::DataCallbackResult onAudioReady(oboe::AudioStream* s, void* out, int32_t n) override {
        if (calls.fetch_add(1)==0) {int p{};sched_param q{}; if(!pthread_getschedparam(pthread_self(),&p,&q)){policy=p;priority=q.sched_priority;}}
        std::memset(out,0,size_t(n)*s->getBytesPerFrame()); frames+=n;
        min=std::min(min.load(),n);max=std::max(max.load(),n);
        return oboe::DataCallbackResult::Continue;
    }
};
int main(){
    std::printf("{\"uid\":%d,\"pid\":%d,\"scope\":\"Oboe standalone; silence; no emulator SinkStream\"}\n",getuid(),getpid());
    for(int mode=0;mode<4;++mode){
        Callback cb;std::shared_ptr<oboe::AudioStream> s;oboe::AudioStreamBuilder b;
        b.setDirection(oboe::Direction::Output)->setUsage(oboe::Usage::Game)->setPerformanceMode(oboe::PerformanceMode::LowLatency)
        ->setAudioApi(mode==0?oboe::AudioApi::OpenSLES:oboe::AudioApi::AAudio)
        ->setSharingMode(mode==3?oboe::SharingMode::Exclusive:oboe::SharingMode::Shared)
        ->setChannelCount(2)->setChannelMask(oboe::ChannelMask::Stereo)
        ->setFormat(mode<2?oboe::AudioFormat::I16:oboe::AudioFormat::Float)
        ->setFormatConversionAllowed(true)->setChannelConversionAllowed(true)
        ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::High)->setDataCallback(&cb);
        if(mode<2)b.setSampleRate(48000)->setBufferCapacityInFrames(480);
        auto r=b.openStream(s);if(r!=oboe::Result::OK || !s){std::printf("{\"mode\":%d,\"open_error\":%d}\n",mode,int(r));continue;}
        auto tuned=s->setBufferSizeInFrames(mode<2?480:s->getFramesPerBurst()*2);
        auto started=s->start();std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        auto xrun=s->getXRunCount();int64_t pos{},ns{};auto ts=s->getTimestamp(CLOCK_MONOTONIC,&pos,&ns);
        const int api=int(s->getAudioApi()),perf=int(s->getPerformanceMode()),sharing=int(s->getSharingMode()),
            rate=s->getSampleRate(),channels=s->getChannelCount(),format=int(s->getFormat()),burst=s->getFramesPerBurst(),cap=s->getBufferCapacityInFrames(),size=s->getBufferSizeInFrames();
        auto stop=s->stop();auto close=s->close();
        std::printf("{\"mode\":%d,\"api\":%d,\"perf\":%d,\"sharing\":%d,\"rate\":%d,\"channels\":%d,\"format\":%d,\"burst\":%d,\"capacity\":%d,\"buffer_size\":%d,\"callbacks\":%d,\"frames\":%d,\"callback_min\":%d,\"callback_max\":%d,\"callback_policy\":%d,\"callback_priority\":%d,\"xrun_error\":%d,\"xrun\":%d,\"timestamp_result\":%d,\"timestamp_frame\":%lld,\"tune_error\":%d,\"start\":%d,\"stop\":%d,\"close\":%d}\n",mode,api,perf,sharing,rate,channels,format,burst,cap,size,cb.calls.load(),cb.frames.load(),cb.min.load(),cb.max.load(),cb.policy.load(),cb.priority.load(),int(xrun.error()),xrun?xrun.value():-1,int(ts),(long long)pos,int(tuned.error()),int(started),int(stop),int(close));std::fflush(stdout);
    }
}
