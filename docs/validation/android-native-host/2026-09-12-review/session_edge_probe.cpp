// Review-only reproducer. Links the unchanged production SessionCore with a fake backend.
// Run each mode in a bounded child process; exit 1 means the named defect reproduced.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include "core/host_runtime/session_core.h"
using namespace Core::HostRuntime;
namespace GC = Core::GuestCpu;
struct Rt : SessionRuntime { std::atomic<int> controls{0}; };
struct Backend : ISessionBackend {
    std::string mode;
    std::mutex m;
    std::condition_variable cv;
    bool run_entered=false, finish_run=false, destroy_entered=false, allow_destroy=false;
    bool control_entered=false, allow_control=false;
    std::atomic<int> destroys{0};
    std::atomic<bool> overlap{false}, after_destroy{false};
    std::shared_ptr<Rt> rt;
    explicit Backend(std::string value) : mode(std::move(value)) {}
    void Await(bool& flag) {
        std::unique_lock l(m);
        if (!cv.wait_for(l,std::chrono::seconds(5),[&]{return flag;}))
            throw std::runtime_error("probe handshake timeout");
    }
    void Set(bool& flag) { std::lock_guard l(m);flag=true;cv.notify_all(); }
    GC::Result<std::shared_ptr<SessionRuntime>> Prepare(const SessionParams&) override {
        if(mode=="prepare_throw") throw std::runtime_error("injected Prepare exception");
        rt=std::make_shared<Rt>();
        return std::shared_ptr<SessionRuntime>(rt);
    }
    RunReport Run(SessionRuntime&) override {
        if(mode=="run_throw") throw std::runtime_error("injected Run exception");
        Set(run_entered);Await(finish_run);return {};
    }
    GC::Result<StopTicket> RequestCancel(SessionRuntime& value) override {
        if(mode=="stop_throw") throw std::runtime_error("injected RequestCancel exception");
        auto& r=static_cast<Rt&>(value);
        ++r.controls;Set(control_entered);Await(allow_control);
        if(!r.alive.load()) after_destroy=true;
        --r.controls;return StopTicket{1,true};
    }
    GC::Status WaitStopped(SessionRuntime&,const StopTicket&,std::uint64_t) override {
        return GC::Ok();
    }
    void Destroy(SessionRuntime& value) override {
        Set(destroy_entered);
        if(mode=="early_cancel") Await(allow_destroy);
        auto& r=static_cast<Rt&>(value);
        if(r.controls.load()!=0) overlap=true;
        r.alive=false;++destroys;
    }
};
int main(int argc,char** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    const std::string mode=argc>1?argv[1]:"early_cancel";
    Backend backend(mode);
    SessionCore core(backend);
    if(mode=="early_cancel") core.TestGate().Arm(Checkpoint::BeforePublish,1);
    const auto gen=core.Start({"review",1});
    if(mode=="prepare_throw" || mode=="run_throw") {
        Terminal terminal;
        bool done=core.WaitTerminal(gen,4'000'000'000,terminal);
        std::printf("exception safely reported=%d\n",done);
        return done?0:1;
    }
    if(mode=="early_cancel") {
        core.TestGate().WaitUntilArrived(Checkpoint::BeforePublish,gen);
        auto first=core.RequestStop(gen,1'000'000);
        core.TestGate().Release(Checkpoint::BeforePublish,gen);
        backend.Await(backend.destroy_entered);
        std::thread stopper([&]{(void)core.RequestStop(gen,1'000'000);});
        backend.Await(backend.control_entered);
        backend.Set(backend.allow_destroy);
        Terminal terminal;
        bool done=core.WaitTerminal(gen,1'000'000'000,terminal);
        backend.Set(backend.allow_control);stopper.join();
        std::printf("first=%s terminal=%d exited=%d destroy_with_active_control=%d control_after_destroy=%d\n",
                    ToString(first),done,terminal.exited,backend.overlap.load(),backend.after_destroy.load());
        return backend.overlap || backend.after_destroy ? 1:0;
    }
    backend.Await(backend.run_entered);
    std::thread stopper;
    bool escaped=false;
    if(mode=="stop_throw") {
        try {(void)core.RequestStop(gen,1'000'000);} catch(...) {escaped=true;}
    } else {
        stopper=std::thread([&]{(void)core.RequestStop(gen,1'000'000);});
        backend.Await(backend.control_entered);
    }
    backend.Set(backend.finish_run);
    Terminal terminal;
    bool done=core.WaitTerminal(gen,3'000'000'000,terminal);
    if(stopper.joinable()) {backend.Set(backend.allow_control);stopper.join();}
    Terminal again;
    bool second=core.WaitTerminal(gen,100'000'000,again);
    std::printf("escaped=%d terminal=%d exited=%d phase=%s after_control_return_terminal=%d exited=%d destroys=%d next_start=%llu\n",
                escaped,done,terminal.exited,ToString(core.QueryPhase(gen)),second,again.exited,
                backend.destroys.load(),static_cast<unsigned long long>(core.Start({"next",1})));
    return escaped || !terminal.exited || backend.destroys!=1 ? 1:0;
}
