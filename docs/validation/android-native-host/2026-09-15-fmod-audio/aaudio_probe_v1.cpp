#include <aaudio/AAudio.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
#include <unistd.h>
#include <pthread.h>
struct CallbackState {
    int channels{};
    std::atomic<int> calls{}, frames{}, min_frames{1000000}, max_frames{}, policy{-1}, priority{-1};
};
aaudio_data_callback_result_t callback(AAudioStream*, void* user, void* data, int32_t n) {
    auto& s = *static_cast<CallbackState*>(user);
    if (s.calls.fetch_add(1) == 0) {
        int p{}; sched_param q{};
        if (!pthread_getschedparam(pthread_self(), &p, &q)) { s.policy = p; s.priority = q.sched_priority; }
    }
    std::memset(data, 0, size_t(n) * s.channels * sizeof(float));
    s.frames.fetch_add(n);
    s.min_frames = std::min(s.min_frames.load(), n);
    s.max_frames = std::max(s.max_frames.load(), n);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
int main() {
    std::printf("{\"uid\":%d,\"pid\":%d,\"scope\":\"standalone native probe; silence; no FEX\"}\n", getuid(), getpid());
    for (int mode=0; mode<5; ++mode) {
        const int channels = (mode==0 || mode==2) ? 8 : 2;
        const bool cb=mode>=2, tune=mode==4;
        CallbackState state; state.channels=channels;
        AAudioStreamBuilder* b{}; AAudioStream* stream{};
        auto result = AAudio_createStreamBuilder(&b);
        if (result) return 2;
        AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
        AAudioStreamBuilder_setSampleRate(b,48000);
        AAudioStreamBuilder_setChannelCount(b,channels);
        AAudioStreamBuilder_setUsage(b,AAUDIO_USAGE_GAME);
        AAudioStreamBuilder_setBufferCapacityInFrames(b,2048);
        if (cb) AAudioStreamBuilder_setDataCallback(b,callback,&state);
        result=AAudioStreamBuilder_openStream(b,&stream);
        AAudioStreamBuilder_delete(b);
        if (result || !stream) { std::printf("{\"mode\":%d,\"open_error\":%d}\n",mode,result); continue; }
        if (AAudioStream_getChannelCount(stream)!=channels || AAudioStream_getFormat(stream)!=AAUDIO_FORMAT_PCM_FLOAT) {
            AAudioStream_close(stream); std::printf("{\"mode\":%d,\"format_mismatch\":true}\n",mode); continue;
        }
        int burst=AAudioStream_getFramesPerBurst(stream), tuning=0;
        if (tune) tuning=AAudioStream_setBufferSizeInFrames(stream,burst*2);
        result=AAudioStream_requestStart(stream);
        int writes{}, accepted{}, error{};
        std::vector<float> silence(size_t(512)*channels);
        auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(1200);
        while (!result && std::chrono::steady_clock::now()<end) {
            if (cb) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            else {
                int n=AAudioStream_write(stream,silence.data(),512,10000000);
                if(n<0) {error=n;break;} ++writes; accepted+=n;
            }
        }
        int64_t pos{}, time{};
        int ts=AAudioStream_getTimestamp(stream,CLOCK_MONOTONIC,&pos,&time);
        const int perf=AAudioStream_getPerformanceMode(stream), rate=AAudioStream_getSampleRate(stream),
            cap=AAudioStream_getBufferCapacityInFrames(stream), size=AAudioStream_getBufferSizeInFrames(stream),
            xrun=AAudioStream_getXRunCount(stream);
        const auto wr=AAudioStream_getFramesWritten(stream),rd=AAudioStream_getFramesRead(stream);
        const int stop=AAudioStream_requestStop(stream),closed=AAudioStream_close(stream);
        std::printf("{\"mode\":%d,\"channels\":%d,\"callback\":%s,\"tuned\":%s,\"start\":%d,\"perf\":%d,\"rate\":%d,\"burst\":%d,\"capacity\":%d,\"buffer_size\":%d,\"tuning_result\":%d,\"xrun\":%d,\"callback_calls\":%d,\"callback_frames\":%d,\"callback_min\":%d,\"callback_max\":%d,\"callback_policy\":%d,\"callback_priority\":%d,\"write_calls\":%d,\"write_accepted\":%d,\"write_error\":%d,\"frames_written\":%lld,\"frames_read\":%lld,\"timestamp_result\":%d,\"timestamp_frame\":%lld,\"stop\":%d,\"close\":%d}\n",
          mode,channels,cb?"true":"false",tune?"true":"false",result,perf,rate,burst,cap,size,tuning,xrun,state.calls.load(),state.frames.load(),state.min_frames.load(),state.max_frames.load(),state.policy.load(),state.priority.load(),writes,accepted,error,(long long)wr,(long long)rd,ts,(long long)pos,stop,closed);
        std::fflush(stdout);
    }
}
