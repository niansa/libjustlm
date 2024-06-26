#include "justlm.hpp"

#include <cstring>
#include <ggml.h>
#include <llama.h>
#include <common/grammar-parser.h>


namespace LM {
class LLaMAInference final : public Inference {
    struct State {
        llama_context *ctx = nullptr;
        llama_model *model;
        llama_grammar *grammar = nullptr;
        bool grammar_override_temp;
        grammar_parser::parse_state parsed_grammar;
        std::string prompt; // Mostly here for easy "debugging"
        std::vector<int> tokens;
        unsigned n_ctx;
    };

    State*& get_state() {
        return *reinterpret_cast<State**>(&generic_state);
    }
    State* const& get_state() const {
        return *reinterpret_cast<State* const*>(&generic_state);
    }

    LM_ERRBOOL init(const std::string& weights_path) LM_NOEXCEPTDECL {
        auto& state = get_state();

        // Allocate state
        state = new State;

        // Get llama parameters
        auto lparams = llama_context_default_params();
        lparams.seed = params.seed;
        lparams.n_ctx = params.n_ctx = params.n_ctx>0?params.n_ctx:2024;
        lparams.n_threads = params.n_threads;
        //lparams.n_threads_batch = params.n_threads;  TODO: Is this sane?

        // Get model parameters
        auto mparams = llama_model_default_params();
        mparams.use_mlock = params.use_mlock;
        mparams.n_gpu_layers = params.n_gpu_layers;

        // Load model
        state->model = llama_load_model_from_file(weights_path.c_str(), mparams);
        if (!state->model) {
            LM_THROW("Failed to initialize llama model from file", LM_BOOL_ERROR);
        }

        // Create context
        state->ctx = llama_new_context_with_model(state->model, lparams);
        if (!state->ctx) {
            LM_THROW("Failed to initialize llama context from model", LM_BOOL_ERROR);
        }

        // Initialize some variables
        state->n_ctx = llama_n_ctx(state->ctx);

        return LM_BOOL_SUCCESS;
    }

    // This function reduces the size of our tokens vector according to some parameters
    // All tokens will be evaluated if scrolling was needed and true will be returned
    bool window_scroll() LM_NOEXCEPTDECL {
        auto &state = get_state();
        // Check that we actually need to scroll
        if (state->tokens.size() <= state->n_ctx) {
            // Nope
            return false;
        }
        // Start scrolling
        if (params.scroll_keep > 0.0f) {
            // "Scroll" down the context window...
            unsigned keep_count = float(state->tokens.size() - params.n_ctx_window_top_bar) * 0.4f; // We keep about 40%
            // Get vector of tokens to keep
            std::vector<int> tokens_in_view(state->tokens.end()-keep_count, state->tokens.end());
            // Cut down tokens vector size
            state->tokens.resize(params.n_ctx_window_top_bar+keep_count);
            // Overwrite tokens after top bar with tokens in view
            std::memcpy(state->tokens.data()+params.n_ctx_window_top_bar, tokens_in_view.data(), tokens_in_view.size()*sizeof(int));
        } else {
            // Cut down tokens vector size to top bar
            state->tokens.resize(params.n_ctx_window_top_bar);
        }
        // Evaluate tokens
        LM_ERROR_FORWARD(evaluate_tokens(0, on_scroll), LM_BOOL_ERROR);
        return true;
    }

    LM_ERRBOOL evaluate_tokens(size_t starting_offset, const AppendCallback &on_tick = nullptr) LM_NOEXCEPTDECL {
        auto& state = get_state();

        // Evaluate tokens in batches
        unsigned it;
        for (it = starting_offset; ; it += params.n_batch) {
            if (it + params.n_batch >= ssize_t(state->tokens.size())) break;

            // Evaluate
            const auto batch = llama_batch_get_one(state->tokens.data()+it, params.n_batch, it, 0);
            if (llama_decode(state->ctx, batch)) {
                LM_THROW("Failed to evaluate tokens in batches", LM_BOOL_ERROR);
            }

            // Tick
            if (on_tick) {
                // Calculate progress
                auto progress = float(it-starting_offset) / (state->tokens.size()-starting_offset) * 100.f;
                // Tick and yield
                if (!on_tick(progress)) return LM_BOOL_SUCCESS;
            }
        }

        // Evaluate remaining tokens
        if (it < state->tokens.size()) {
            for (; it != state->tokens.size(); it++) {
                const auto batch = llama_batch_get_one(state->tokens.data()+it, 1, it, 0);
                if (llama_decode(state->ctx, batch)) {
                    LM_THROW("Failed to evaluate individual tokens", LM_BOOL_ERROR);
                }
            }
        }

        // Notify about completion
        if (on_tick) on_tick(100.f);

        return LM_BOOL_SUCCESS;
    }

    int accept_token(int t) {
        auto& state = get_state();
        if (state->grammar)
            llama_grammar_accept_token(state->ctx, state->grammar, t);
        return t;
    }

    int llama_sample_top_p_top_k() {
        auto& state = get_state();
        auto logits = llama_get_logits(state->ctx);
        auto n_vocab = llama_n_vocab(state->model);
        // Populate initial list of all candidates
        std::vector<llama_token_data> candidates;
        candidates.reserve(n_vocab);
        for (int token_id = 0; token_id < n_vocab; token_id++) {
            candidates.emplace_back(llama_token_data{token_id, logits[token_id], 0.0f});
        }
        llama_token_data_array candidates_p = {candidates.data(), candidates.size(), false};
        // Sample repeat penalty
        auto n_repeat_last = std::min<size_t>(state->tokens.size(), params.n_repeat_last);
        llama_sample_repetition_penalties(state->ctx, &candidates_p, params.n_repeat_last?(state->tokens.data()+state->tokens.size()-n_repeat_last):nullptr, n_repeat_last, params.repeat_penalty, 1.0f, 1.0f); // Might be wrong
        // Grammar sampling
        if (state->grammar) {
            llama_sample_grammar(state->ctx, &candidates_p, state->grammar);
        }
        if (!(state->grammar && state->grammar_override_temp) && (params.temp > 0.01f || params.temp < -0.01f)) {
            // Temperature sampling
            switch (params.prefer_mirostat) {
            case 0: {
                llama_sample_top_k(state->ctx, &candidates_p, params.top_k, 1);
                llama_sample_tail_free(state->ctx, &candidates_p, 1.0f, 1);
                llama_sample_typical(state->ctx, &candidates_p, 1.0f, 1);
                llama_sample_top_p(state->ctx, &candidates_p, params.top_p, 1);
                llama_sample_temp(state->ctx, &candidates_p, params.temp);
                return accept_token(llama_sample_token(state->ctx, &candidates_p));
            }
            case 1: {
                float mirostat_mu = 2.0f * params.mirostat_target_entropy;
                const int mirostat_m = 100;
                llama_sample_temp(state->ctx, &candidates_p, params.temp);
                return accept_token(llama_sample_token_mirostat(state->ctx, &candidates_p, params.mirostat_target_entropy, params.mirostat_learning_rate, mirostat_m, &mirostat_mu));
            }
            case 2: {
                float mirostat_mu = 2.0f * params.mirostat_target_entropy;
                llama_sample_temp(state->ctx, &candidates_p, params.temp);
                return accept_token(llama_sample_token_mirostat_v2(state->ctx, &candidates_p, params.mirostat_target_entropy, params.mirostat_learning_rate, &mirostat_mu));
            }
            default: LM_THROW("Invalid mirostat version "+std::to_string(params.prefer_mirostat), LM_BOOL_ERROR);
            }
        } else {
            // Greedy sampling
            return accept_token(llama_sample_token(state->ctx, &candidates_p));
        }
    }

public:
    LLaMAInference(const std::string& weights_path, const Params& p) : Inference(p) {
        init(weights_path);
    }
    ~LLaMAInference() override {
        auto& state = get_state();

        if (state) {
            if (state->ctx) llama_free(state->ctx);
            delete state;
        }
    }

    LM_ERRBOOL append(const std::string& prompt, const AppendCallback &on_tick) LM_NOEXCEPTDECL override {
        auto& state = get_state();

        // Check if prompt was empty
        const bool was_empty = state->prompt.empty();

        // Append to current prompt
        state->prompt.append(prompt);

        // Resize buffer for tokens
        const auto old_token_count = state->tokens.size();
        state->tokens.resize(old_token_count+state->prompt.size());

        // Run tokenizer
        const auto token_count = llama_tokenize(state->model, prompt.c_str(), prompt.size(), state->tokens.data()+old_token_count, state->tokens.size()-old_token_count, was_empty, false);
        state->tokens.resize(old_token_count+token_count);

        // Make sure token limit isn't being hit
        if (window_scroll()) {
            // That function already has evaluated our tokens since scrolling was needed
            return LM_BOOL_SUCCESS;
        }

        // Evaluate new tokens
        return evaluate_tokens(old_token_count, on_tick);
    }

    std::string run(std::string_view end, const GenerateCallback &on_tick, const GenerateCallback& pre_tick) LM_NOEXCEPTDECL override {
        auto& state = get_state();
        std::string fres;

        // Loop until done
        bool abort = false;
        unsigned eos_count = 0;
        size_t last_size = 0;
        while (!abort && (end.empty() || fres.find(end) == fres.npos)) {
            last_size = fres.size();
            // Sample top p and top k
            int id;
            try {
                id = llama_sample_top_p_top_k();
            } catch (const std::exception& e) {
                LM_THROW(e.what(), "");
            }

            if (id == llama_token_eos(state->model)) {
                if (eos_count++ == params.n_eos_ignores) {
                    abort = true;
                    continue;
                }
                state->tokens.push_back(0);
                llama_tokenize(state->model, "\n", 1, &state->tokens.back(), 1, false, false);
                id = state->tokens.back();
            } else {
                // Add token
                state->tokens.push_back(id);
            }

            // Make sure token limit isn't hit
            window_scroll();

            // Get token as string
            std::string str(14, ' ');
            str.resize(llama_token_to_piece(state->model, id, str.data(), 14));

            // Append string to function result
            state->prompt.append(str);
            fres.append(str);

            // Tick
            if (pre_tick && !pre_tick(str.data())) abort = true;
            else {
                // Evaluate token
                //  TODO: Respect batch size
                const auto batch = llama_batch_get_one(state->tokens.data()+state->tokens.size()-1, 1, state->tokens.size()-1, 0);
                if (llama_decode(state->ctx, batch)) {
                    LM_THROW("Failed to evaluate new tokens", "");
                }
            }

            // Tick and yield
            if (on_tick && !on_tick(str.data())) abort = true;
        }

        // Create final string  TODO: Could be optimized
        if (!abort && fres.size() > end.size()) {
            fres = std::string(fres.data(), last_size);
        }

        // Return final string
        return fres;
    }

    unsigned get_context_size() const noexcept override {
        return get_state()->tokens.size();
    }

    LM_ERRBOOL create_savestate(Savestate &sv) const LM_NOEXCEPTDECL override {
        auto& state = get_state();
        sv.buf.resize(llama_get_state_size(state->ctx));
        llama_copy_state_data(state->ctx, sv.buf.data());
        sv.tokens = state->tokens;
        sv.prompt = state->prompt;
        sv.ctx = generic_state;
        return LM_BOOL_SUCCESS;
    }
    LM_ERRBOOL restore_savestate(const Savestate &sv) LM_NOEXCEPTDECL override {
        auto& state = get_state();
        if (sv.ctx != generic_state)
            LM_THROW("Savestate does not match context", LM_BOOL_ERROR);
        llama_set_state_data(state->ctx, const_cast<uint8_t*>(sv.buf.data()));
        state->tokens = sv.tokens;
        state->prompt = sv.prompt;
        return LM_BOOL_SUCCESS;
    }

    LM_ERRBOOL serialize(std::ostream &o) const LM_NOEXCEPTDECL override {
        auto& state = get_state();
        // Get state size
        auto state_size = llama_get_state_size(state->ctx);
        // Write sizes
        for (const uint32_t s : {static_cast<size_t>(state->n_ctx), state->tokens.size(), state->prompt.size(), state_size}) {
            if (!o.write(reinterpret_cast<const char*>(&s), sizeof(s))) {
                LM_THROW("Failed to serialize data sizes", LM_BOOL_ERROR);
            }
        }
        // Write tokens
        if (!o.write(reinterpret_cast<const char*>(state->tokens.data()), state->tokens.size()*sizeof(int))) {
            LM_THROW("Failed to serialize tokens", LM_BOOL_ERROR);
        }
        // Write prompt
        if (!o.write(state->prompt.data(), state->prompt.size())) {
            LM_THROW("Failed to serialize prompt", LM_BOOL_ERROR);
        }
        // Write state
        std::vector<uint8_t> state_buf(state_size);
        llama_copy_state_data(state->ctx, state_buf.data());
        if (!o.write(reinterpret_cast<const char*>(state_buf.data()), state_size)) {
            LM_THROW("Failed to serialize state", LM_BOOL_ERROR);
        }
        return LM_BOOL_SUCCESS;
    }
    LM_ERRBOOL deserialize(std::istream &i) LM_NOEXCEPTDECL override {
        auto& state = get_state();
        uint32_t n_ctx, embd_size, prompt_size, state_size;
        // Initialization to prevent compiler complaints
        n_ctx = embd_size = prompt_size = state_size = 0;
        // Read sizes
        for (uint32_t *s : {&n_ctx, &embd_size, &prompt_size, &state_size}) {
            if (!i.read(reinterpret_cast<char*>(s), sizeof(*s))) {
                LM_THROW("Failed to deserialize data sizes", LM_BOOL_ERROR);
            }
        }
        if (state->n_ctx != n_ctx) {
            LM_THROW("Context length differs (My "+std::to_string(state->n_ctx)+" vs. files "+std::to_string(n_ctx)+')', LM_BOOL_ERROR);
        }
        // Read tokens
        state->tokens.resize(embd_size);
        if (!i.read(reinterpret_cast<char*>(state->tokens.data()), state->tokens.size()*sizeof(int))) {
            LM_THROW("Failed to deserialize tokens", LM_BOOL_ERROR);
        }
        // Read prompt
        state->prompt.resize(prompt_size);
        if (!i.read(state->prompt.data(), state->prompt.size())) {
            LM_THROW("Failed to deserialize prompt", LM_BOOL_ERROR);
        }
        // Read state
        std::vector<uint8_t> state_buf(state_size);
        if (!i.read(reinterpret_cast<char*>(state_buf.data()), state_buf.size())) {
            LM_THROW("Failed to deserialize state", LM_BOOL_ERROR);
        }
        llama_set_state_data(state->ctx, state_buf.data());
        return LM_BOOL_SUCCESS;
    }

    LM_ERRBOOL load_grammar(const std::string& src, bool override_temperature) LM_NOEXCEPTDECL override {
        auto& state = get_state();

        state->parsed_grammar = grammar_parser::parse(src.c_str());
        if (state->parsed_grammar.rules.empty()) {
            LM_THROW("Failed to parse grammar (or no rules)", LM_BOOL_ERROR);
        }

        auto rules = state->parsed_grammar.c_rules();
        state->grammar = llama_grammar_init(rules.data(), rules.size(), state->parsed_grammar.symbol_ids.at("root"));
        if (!state->grammar) {
            LM_THROW("Failed to generate llama grammar", LM_BOOL_ERROR);
        }

        state->grammar_override_temp = override_temperature;

        return LM_BOOL_SUCCESS;
    }
    LM_ERRBOOL unload_grammar() LM_NOEXCEPTDECL override {
        get_state()->grammar = nullptr;

        return LM_BOOL_SUCCESS;
    }

    const std::string &get_prompt() const LM_NOEXCEPTDECL override {
        return get_state()->prompt;
    }

    bool is_mirostat_available() const noexcept override {
        return true;
    }

    bool is_grammar_available() const noexcept override {
        return true;
    }
};
}
