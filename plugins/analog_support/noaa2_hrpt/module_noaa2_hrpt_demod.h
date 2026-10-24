#pragma once

#include "modules/demod/module_demod_base.h"
#include "common/dsp/demod/quadrature_demod.h"
#include "common/dsp/utils/real_to_complex.h"
#include "common/dsp/utils/freq_shift.h"
#include "common/dsp/resamp/rational_resampler.h"
#include "common/dsp/io/wav_writer.h"
#include "common/audio/audio_sink.h"
#include <fstream>

namespace noaa2_hrpt
{
    class NOAA2HRPTDemodModule : public demod::BaseDemodModule
    {
    protected:
        std::shared_ptr<dsp::RationalResamplerBlock<complex_t>> res1;
        std::shared_ptr<dsp::QuadratureDemodBlock> qua1;
        std::shared_ptr<dsp::RealToComplexBlock> rtc;
        std::shared_ptr<dsp::FreqShiftBlock> fshift;
        std::shared_ptr<dsp::RationalResamplerBlock<complex_t>> res2;
        std::shared_ptr<dsp::QuadratureDemodBlock> qua2;

        bool save_wav = false;
        bool save_audio = false;
        std::ofstream audio_out;
        
        bool enable_audio = false;
        bool play_audio = false;
        float audio_volume = 1.0f;
        std::shared_ptr<audio::AudioSink> audio_sink;
        
        double audio_samplerate = 60000.0;

    public:
        NOAA2HRPTDemodModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters);
        ~NOAA2HRPTDemodModule();
        void init();
        void stop();
        void process();
        void drawUI(bool window);

    public:
        static std::string getID();
        virtual std::string getIDM() { return getID(); };
        static std::vector<std::string> getParameters();
        static std::shared_ptr<ProcessingModule> getInstance(std::string input_file, std::string output_file_hint, nlohmann::json parameters);
    };
}