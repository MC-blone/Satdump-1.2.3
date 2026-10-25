#pragma once

#include "core/module.h"
#include "common/dsp/demod/quadrature_demod.h"
#include "common/dsp/utils/real_to_complex.h"
#include "common/dsp/utils/freq_shift.h"
#include "common/dsp/filter/fir.h"
#include "common/dsp/resamp/rational_resampler.h"
#include "common/dsp/io/wav_writer.h"
#include "common/dsp/io/strict_file_source.h"
#include "common/audio/audio_sink.h"
#include "common/widgets/constellation.h"

namespace noaa2_hrpt
{
    class NOAA2HRPTDemodModule : public ProcessingModule
    {
    protected:
        std::shared_ptr<dsp::StrictFileSource> baseband_io;
        
        std::shared_ptr<dsp::FIRBlock<complex_t>> filt1;       
        std::shared_ptr<dsp::QuadratureDemodBlock> fm1;        
        std::shared_ptr<dsp::RealToComplexBlock> rtc;          
        std::shared_ptr<dsp::FreqShiftBlock> fsh;              
        std::shared_ptr<dsp::FIRBlock<complex_t>> filt_lp;     
        std::shared_ptr<dsp::QuadratureDemodBlock> fm2;        
        std::shared_ptr<dsp::RationalResamplerBlock<float>> rsp; 

        std::ofstream data_out;
        std::ofstream audio_out;
        
        std::atomic<uint64_t> filesize;
        std::atomic<uint64_t> progress;

        bool save_wav = false;
        bool save_audio = false;
        bool enable_audio = false;
        bool play_audio = false;
        float audio_volume = 1.0f;
        std::shared_ptr<audio::AudioSink> audio_sink;
        
        double audio_samplerate = 120000.0; 
        double d_samplerate = 2880000.0;
        std::string baseband_format = "cs16";

        // UI Stuff
        widgets::ConstellationViewer constellation;

    public:
        NOAA2HRPTDemodModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters);
        virtual ~NOAA2HRPTDemodModule();
        void process();
        void drawUI(bool window);
        std::vector<ModuleDataType> getInputTypes() { return {DATA_FILE, DATA_STREAM}; }
        std::vector<ModuleDataType> getOutputTypes() { return {DATA_FILE}; }

    public:
        static std::string getID();
        virtual std::string getIDM() { return getID(); };
        static std::vector<std::string> getParameters();
        static std::shared_ptr<ProcessingModule> getInstance(std::string input_file, std::string output_file_hint, nlohmann::json parameters);
    };
}