class FexTestRunGateImpl final : public FexTestRunGate {
  public:
    bool WaitAtEntry(std::uint64_t context_id, std::uint64_t thread_id, std::uint64_t invocation,
                     std::uint64_t timeout_ms) override {
        std::unique_lock lock{mutex_};
        if (!armed_ || arm_thread_ != thread_id) {
            // Not the held owner (or nothing armed): pass straight through, do not touch gate state.
            return true;
        }
        present_context_ = context_id;
        present_thread_ = thread_id;
        present_invocation_ = invocation;
        arrived_ = true;
        cv_.notify_all();
        const bool released = cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms ? timeout_ms : 1000),
                                          [&] { return !armed_; });
        arrived_ = false;
        ++exited_generation_;
        cv_.notify_all();
        return released;
    }

    bool Arm(std::uint64_t thread_id) override {
        std::unique_lock lock{mutex_};
        // Refuse to arm while the previous generation has not fully passed through/left the gate.
        if (armed_)
            return false;
        armed_ = true;
        arrived_ = false;
        arm_thread_ = thread_id;
        cv_.notify_all();
        return true;
    }

    bool Arrived() override {
        std::unique_lock lock{mutex_};
        return armed_ && arrived_ && arm_thread_ == present_thread_;
    }

    bool Release(std::uint64_t timeout_ms) override {
        std::unique_lock lock{mutex_};
        const std::uint64_t before = exited_generation_;
        armed_ = false;
        cv_.notify_all();
        return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms ? timeout_ms : 1000),
                            [&] { return exited_generation_ > before; });
    }

  private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool armed_{false};
    bool arrived_{false};
    std::uint64_t arm_thread_{0};
    std::uint64_t present_context_{0};
    std::uint64_t present_thread_{0};
    std::uint64_t present_invocation_{0};
    std::uint64_t exited_generation_{0};
};
