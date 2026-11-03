#pragma once
#include "polar_engine.hpp"
#include "packed_fft.hpp"
#include "fft0.hpp"
#include "fft1.hpp"
#include "original_math_tables.hpp"
#include <memory>
#include <string>

namespace polar_recovered {
class PolarProcessor {
    PackedRealFFT transform_;
    PolarEngine engine_;
    std::unique_ptr<PolarFFT0> spectral_;
    std::unique_ptr<OriginalAlg1Reference> periodic_;
    PolarEngine::PitchRequest source_{};
    std::uint64_t generation_=~std::uint64_t(0);
    int half_=0;
    void prepare() {
        auto f=[this](float*p,int n){transform_.forward(p,n);};
        auto i=[this](float*p,int n){transform_.inverse(p,n);};
        spectral_=std::make_unique<PolarFFT0>(f,i,[this](int start,int count,float*l,float*r){
            for(int j=0;j<count;j++){auto k=std::uint32_t(start+j)&source_.ring_mask;
                l[j]=source_.ring_left[k];if(r)r[j]=source_.ring_right[k];}
        });
        spectral_->prepare(engine_.sampleRate());
        periodic_=std::make_unique<OriginalAlg1Reference>(engine_.sampleRate(),f,i,
            [this](int index,int channel){return(channel?source_.ring_right:source_.ring_left)[std::uint32_t(index)&source_.ring_mask];});
        generation_=~std::uint64_t(0);half_=0;
    }
    void renderPitch(const PolarEngine::PitchRequest& r,PolarEngine::PitchOutput& out) {
        source_=r;
        if(r.rebase_frames){spectral_->rebaseSource(r.rebase_frames*2);periodic_->rebaseSource(r.rebase_frames*2);}
        if((engine_.blockCount()&127u)==0){spectral_->rebaseSynthesis(0x100000);periodic_->rebaseOutput(0x100000);}
        if(half_!=r.fft_half_window) {
            if(spectral_->setWindowHalf(r.fft_half_window))applyOriginalTables(*spectral_);
            if(!periodic_->configureMaxLag(r.fft_half_window))throw std::runtime_error("Unsupported FFT window");
            half_=r.fft_half_window;
        }
        if(generation_!=r.reset_generation){spectral_->resetAt(r.anchor);periodic_->reset(r.anchor);generation_=r.reset_generation;}
        if(r.algorithm==0)
            spectral_->process(r.anchor,r.source_step,r.ratio_1,r.ratio_2,64,r.stereo,r.dual_voice,
                out[0].data(),out[1].data(),out[2].data(),out[3].data());
        else {
            OriginalAlg1Reference::Outputs ptr{{{{out[0].data(),out[1].data()}},{{out[2].data(),out[3].data()}}}};
            periodic_->process(r.anchor,r.ratio_1,r.ratio_2,64,r.dual_voice?2:1,r.stereo,ptr);
        }
        if(!r.stereo){out[1]=out[0];out[3]=out[2];}
    }
public:
    using Parameters=PolarEngine::Parameters;
    explicit PolarProcessor(float sr=48000):engine_(sr){prepare();engine_.setPitchRenderer([this](auto&r,auto&o){renderPitch(r,o);});}
    PolarProcessor(const PolarProcessor&)=delete;
    PolarProcessor& operator=(const PolarProcessor&)=delete;
    Parameters& parameters(){return engine_.parameters();}
    const Parameters& parameters()const{return engine_.parameters();}
    void sampleRate(float sr){if(sr!=engine_.sampleRate()){engine_.setSampleRate(sr);prepare();}}
    void reset(){engine_.reset();generation_=~std::uint64_t(0);}
    float sampleRate()const{return engine_.sampleRate();}
    const PolarEngine& engine()const{return engine_;}
    void processBlock(const float*l,const float*r,float*outL,float*outR){engine_.processBlock(l,r,outL,outR);}
    // Names are the public message/API interface. All values must be finite.
    bool set(const std::string& name,float value){
        if(!std::isfinite(value))return false;
        auto&p=parameters();
#define POLAR_FLOAT(n) if(name==#n){p.n=value;return true;}
#define POLAR_INT(n) if(name==#n){if(value < -2147483648.0 || value > 2147483647.0)return false;p.n=int(value);return true;}
#define POLAR_BOOL(n) if(name==#n){p.n=value!=0;return true;}
        POLAR_INT(algorithm) POLAR_INT(speed) POLAR_BOOL(reverse) POLAR_BOOL(lock) POLAR_BOOL(stereo)
        POLAR_FLOAT(position) POLAR_FLOAT(loop) POLAR_FLOAT(position_lfo) POLAR_FLOAT(position_env)
        POLAR_FLOAT(pitch_1) POLAR_FLOAT(tune_1) POLAR_FLOAT(feedback_1) POLAR_FLOAT(level_1) POLAR_FLOAT(pan_1)
        POLAR_FLOAT(pitch_lfo_1) POLAR_FLOAT(pitch_env_1) POLAR_BOOL(enable_2)
        POLAR_FLOAT(pitch_2) POLAR_FLOAT(tune_2) POLAR_FLOAT(feedback_2) POLAR_FLOAT(level_2) POLAR_FLOAT(pan_2)
        POLAR_FLOAT(delay_2) POLAR_FLOAT(pitch_lfo_2) POLAR_FLOAT(pitch_env_2)
        POLAR_BOOL(filter_on) POLAR_INT(filter_mode) POLAR_FLOAT(cutoff) POLAR_FLOAT(resonance)
        POLAR_FLOAT(filter_lfo) POLAR_FLOAT(filter_env) POLAR_FLOAT(lfo_rate) POLAR_BOOL(lfo_sync)
        POLAR_FLOAT(tempo) POLAR_FLOAT(division) POLAR_INT(lfo_wave) POLAR_FLOAT(lfo_phase) POLAR_FLOAT(pan_lfo)
        POLAR_BOOL(gate) POLAR_FLOAT(attack) POLAR_FLOAT(decay) POLAR_FLOAT(sustain) POLAR_FLOAT(release)
        POLAR_FLOAT(env_to_amp) POLAR_FLOAT(dry) POLAR_FLOAT(dry_delay) POLAR_FLOAT(output_db) POLAR_BOOL(bypass)
        POLAR_BOOL(external_modulation) POLAR_FLOAT(external_lfo) POLAR_FLOAT(external_envelope)
#undef POLAR_FLOAT
#undef POLAR_INT
#undef POLAR_BOOL
        return false;
    }
};
}
