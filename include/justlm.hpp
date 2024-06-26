#ifndef JUSTLM_HPP
#define JUSTLM_HPP
#include <iostream>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <thread>

#ifdef LM_NOEXCEPT
#   define LM_NOEXCEPTDECL noexcept
#   define LM_THROW(t, r) do {this->last_error = (t); return r;} while (0)
#   define LM_LAST_ERROR_STORAGE mutable std::string last_error;
#   define LM_LAST_ERROR_GETTER const std::string& get_last_error() const {return last_error;}
#   define LM_ERRBOOL bool
#   define LM_BOOL_ERROR false
#   define LM_BOOL_SUCCESS true
#   define LM_RETHROW(x) return x
#   define LM_ERROR_CATCH(x, errval, ...) {auto v = x; if (v == (errval)) __VA_ARGS__}
#   define LM_ERROR_FORWARD(x, errval) do {auto v = x; if (v == (errval)) return x;} while (0)
#else
#   define LM_NOEXCEPTDECL
#   define LM_THROW(t, r) throw Exception(t)
#   define LM_LAST_ERROR_STORAGE
#   define LM_LAST_ERROR_GETTER
#   define LM_ERRBOOL void
#   define LM_BOOL_ERROR
#   define LM_BOOL_SUCCESS
#   define LM_RETHROW(x) std::rethrow_exception(std::current_exception())
#   define LM_ERROR_CATCH(x, errval, ...) try {x;} catch (...) __VA_ARGS__
#   define LM_ERROR_FORWARD(x, errval) {x;}
#endif

#if _MSC_VER
#include <BaseTsd.h>
#endif


namespace LM {
#if _MSC_VER
using ssize_t = SSIZE_T;
#endif

using GenerateCallback = std::function<bool (const char *generated)>;
using AppendCallback = std::function<bool (float progress)>;

class Inference {
protected:
    AppendCallback on_scroll = nullptr;

    void *generic_state = nullptr;

    LM_LAST_ERROR_STORAGE

public:
    struct Exception : public std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    struct Params {
        int seed = 0; // RNG seed
        unsigned n_threads = 0; // Amount of threads to use, immutable after Inference was constructed
        unsigned n_ctx = 2024; // Context size
        unsigned n_ctx_window_top_bar = 0; // Top bar of context window. Must be smaller than context size
        unsigned n_batch = 8; // Batch size
        unsigned n_repeat_last = 0;
        unsigned n_eos_ignores = 0;

        float scroll_keep = 0.0f; // 0.4f to keep 40% of context below top bar when scrolling; 0.0f to remove everything after top bar

        unsigned top_k = 40;
        float top_p = 0.9f;
        float temp = 0.72f;
        float mirostat_learning_rate = 0.1f; // mirostat specific
        float mirostat_target_entropy = 5.0f; // mirostat specific
        float repeat_penalty = 1.0f;

        unsigned n_gpu_layers = 38;
        bool use_mlock = true; // llama specific
        int prefer_mirostat = 0; // Use given mirostat version if available (see is_mirostat_available()); llama specific
    } params;

    struct Savestate {
        std::vector<uint8_t> buf;
        std::vector<int> tokens;
        std::string prompt;
        void *ctx = nullptr;

        bool is_valid() const {
            return ctx != nullptr;
        }
    };

    Inference(const Params& p) : params(p) {
        // Set random seed
        params.seed = params.seed?params.seed:time(NULL);
        params.n_threads = params.n_threads?params.n_threads:(static_cast<unsigned>(std::thread::hardware_concurrency()) / 2);
    }
    virtual ~Inference() {}
    Inference(const Inference&) = delete;
    Inference(Inference&) = delete;
    Inference(Inference&& o)
            : generic_state(o.generic_state)
            , params(o.params) {
        o.generic_state = nullptr;
    }

    static
    Inference *construct(const std::string& weights_path, const Params& p);

    void set_scroll_callback(const AppendCallback& scroll_cb) noexcept {
        on_scroll = scroll_cb;
    }

    // This must be called with a non-empty prompt!
    virtual LM_ERRBOOL append(const std::string& prompt, const AppendCallback& on_tick = nullptr) LM_NOEXCEPTDECL = 0;

    // append() must have been called at least once before calling this!
    virtual std::string run(std::string_view end = "", const GenerateCallback& on_tick = nullptr, const GenerateCallback& pre_tick = nullptr) LM_NOEXCEPTDECL = 0;

    virtual unsigned get_context_size() const noexcept = 0;

    virtual LM_ERRBOOL create_savestate(Savestate&) const LM_NOEXCEPTDECL = 0;
    virtual LM_ERRBOOL restore_savestate(const Savestate&) LM_NOEXCEPTDECL = 0;

    virtual LM_ERRBOOL serialize(std::ostream&) const LM_NOEXCEPTDECL = 0;
    virtual LM_ERRBOOL deserialize(std::istream&) LM_NOEXCEPTDECL = 0;

    virtual LM_ERRBOOL load_grammar(const std::string&, bool override_temperature [[maybe_unused]] = false) LM_NOEXCEPTDECL {
        LM_THROW("Grammar is not available for this models backend", LM_BOOL_ERROR);
    }
    virtual LM_ERRBOOL unload_grammar() LM_NOEXCEPTDECL {
        LM_THROW("Grammar is not available for this models backend", LM_BOOL_ERROR);
    }

    virtual const std::string& get_prompt() const LM_NOEXCEPTDECL = 0;

    virtual bool is_mirostat_available() const noexcept {return false;}
    virtual bool is_grammar_available() const noexcept {return false;}

    LM_LAST_ERROR_GETTER
};


struct Implementation {
    bool is_fallback = false;
};
}
#endif // JUSTLM_HPP
