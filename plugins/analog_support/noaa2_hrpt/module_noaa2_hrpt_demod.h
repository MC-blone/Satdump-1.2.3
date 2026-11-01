#pragma once

#include "modules/demod/module_demod_base.h"
#include "common/dsp/demod/quadrature_demod.h"
#include "common/dsp/utils/real_to_complex.h"
#include "common/dsp/utils/freq_shift.h"
#include "common/dsp/filter/fir.h"
#include "common/dsp/resamp/rational_resampler.h"
#include "common/dsp/io/wav_writer.h"
#include "common/audio/audio_sink.h"

namespace noaa2_hrpt
{
    class NOAA2HRPTDemodModule : public demod::BaseDemodModule
    {
    protected:
        std::shared_ptr<dsp::FIRBlock<complex_t>> filt1;
        std::shared_ptr<dsp::QuadratureDemodBlock> fm1;
        std::shared_ptr<dsp::RealToComplexBlock> rtc;
        std::shared_ptr<dsp::FreqShiftBlock> fsh;
        std::shared_ptr<dsp::FIRBlock<complex_t>> filt_lp;
        std::shared_ptr<dsp::QuadratureDemodBlock> fm2;
        std::shared_ptr<dsp::RationalResamplerBlock<float>> rsp;

        std::ofstream audio_out;

        bool save_wav = false;
        bool save_audio = false;
        bool play_audio = false;
        float audio_volume = 1.0f;
        std::shared_ptr<audio::AudioSink> audio_sink;

        long audio_samplerate = 120000;

    public:
        NOAA2HRPTDemodModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters);
        ~NOAA2HRPTDemodModule();
        void init();
        void stop();
        void process();
        void drawUI(bool window);

        bool enable_audio = false;

    public:
        static std::string getID();
        virtual std::string getIDM() { return getID(); };
        static std::vector<std::string> getParameters();
        static std::shared_ptr<ProcessingModule> getInstance(std::string input_file, std::string output_file_hint, nlohmann::json parameters);
    };
}