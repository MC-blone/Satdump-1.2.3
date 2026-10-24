#include "module_noaa2_hrpt_demod.h"
#include "common/dsp/filter/firdes.h"
#include "core/config.h"
#include "logger.h"
#include "imgui/imgui.h"
#include <volk/volk.h>
#include <vector>
#include <numeric>
#include <cmath>
#include <algorithm>

namespace noaa2_hrpt
{
    NOAA2HRPTDemodModule::NOAA2HRPTDemodModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters) 
        : BaseDemodModule(input_file, output_file_hint, parameters)
    {
        if (parameters.count("save_wav") > 0) save_wav = parameters["save_wav"].get<bool>();
        if (parameters.count("save audio") > 0) save_audio = parameters["save audio"].get<bool>();

        name = "NOAA-2 HRPT Demodulator (FM)";
        show_freq = false;
        play_audio = satdump::config::main_cfg["user_interface"]["play_audio"]["value"].get<bool>();
        
        constellation.d_hscale = 1.0;
        constellation.d_vscale = 0.5;
    }

    NOAA2HRPTDemodModule::~NOAA2HRPTDemodModule() {}

    void NOAA2HRPTDemodModule::init()
    {
        BaseDemodModule::initb();

        double fs = d_samplerate;
        if (fs <= 0) fs = 2880000;
        
        double subcarrier = 99000.0;
        double bw = 200000.0;

        // 1. 主带通滤波 (99kHz)
        filt1 = std::make_shared<dsp::FIRBlock<complex_t>>(agc->output_stream, dsp::firdes::band_pass(1, fs, subcarrier - bw/2, subcarrier + bw/2, 10000));
        
        // 2. 第一次 FM 鉴频
        fm1 = std::make_shared<dsp::QuadratureDemodBlock>(filt1->output_stream, 2.0f / float(M_PI));
        
        // 3. 实数转复数
        rtc = std::make_shared<dsp::RealToComplexBlock>(fm1->output_stream);
        
        // 4. 移频 -99kHz
        fsh = std::make_shared<dsp::FreqShiftBlock>(rtc->output_stream, fs, -subcarrier);
        
        // 5. 低通滤波 (80kHz)
        filt_lp = std::make_shared<dsp::FIRBlock<complex_t>>(fsh->output_stream, dsp::firdes::low_pass(1, fs, 40000, 10000));
        
        // 6. 第二次 FM 鉴频
        fm2 = std::make_shared<dsp::QuadratureDemodBlock>(filt_lp->output_stream, 2.0f / float(M_PI));

        // 7. 重采样至 120kHz (纯整数比例)
        int g = std::gcd((int)audio_samplerate, (int)fs);
        rsp = std::make_shared<dsp::RationalResamplerBlock<float>>(fm2->output_stream, audio_samplerate / g, fs / g);
    }

    void NOAA2HRPTDemodModule::process()
    {
        if (input_data_type == DATA_FILE) filesize = file_source->getFilesize();
        else filesize = 0;

        const bool write_wav = save_wav || output_data_type == DATA_FILE;
        if (write_wav) {
            data_out = std::ofstream(d_output_file_hint + ".wav", std::ios::binary);
            d_output_files.push_back(d_output_file_hint + ".wav");
        }

        BaseDemodModule::start();
        filt1->start(); fm1->start(); rtc->start(); fsh->start(); filt_lp->start(); fm2->start(); rsp->start();

        std::vector<int16_t> output_wav_buffer(std::max<size_t>(static_cast<size_t>(d_buffer_size) * 100, 65536));
        uint64_t final_data_size = 0;
        dsp::WavWriter wave_writer(data_out);
        if (write_wav && data_out.is_open()) wave_writer.write_header(audio_samplerate, 1);

        if (input_data_type != DATA_FILE && audio::has_sink()) {
            enable_audio = true;
            audio_sink = audio::get_default_sink();
            audio_sink->set_samplerate(audio_samplerate);
            audio_sink->start();
        }

        while (demod_should_run())
        {
            int dat_size = rsp->output_stream->read();
            if (dat_size <= 0) {
                rsp->output_stream->flush();
                if (input_data_type == DATA_FILE && file_source->eof()) break;
                continue;
            }

            if (dat_size > static_cast<int>(output_wav_buffer.size())) break;

            constellation.pushFloatAndGaussian(rsp->output_stream->readBuf, dat_size);

            for (int i = 0; i < dat_size; i++) {
                if (rsp->output_stream->readBuf[i] > 1.0f) rsp->output_stream->readBuf[i] = 1.0f;
                if (rsp->output_stream->readBuf[i] < -1.0f) rsp->output_stream->readBuf[i] = -1.0f;
            }

            volk_32f_s32f_convert_16i(output_wav_buffer.data(), (float *)rsp->output_stream->readBuf, 32767, dat_size);

            if (write_wav && data_out.is_open()) {
                data_out.write(reinterpret_cast<const char *>(output_wav_buffer.data()), dat_size * sizeof(int16_t));
                final_data_size += dat_size * sizeof(int16_t);
            }
            if (output_data_type != DATA_FILE && output_fifo)
                output_fifo->write(reinterpret_cast<uint8_t *>(output_wav_buffer.data()), dat_size * sizeof(int16_t));

            rsp->output_stream->flush();

            if (input_data_type == DATA_FILE && filesize > 0) {
                progress = std::min<uint64_t>(file_source->getPosition(), filesize);
            }
        }

        if (enable_audio && audio_sink) audio_sink->stop();
        if (write_wav && data_out.is_open()) { wave_writer.finish_header(final_data_size); data_out.close(); }
        
        BaseDemodModule::stop();
        filt1->stop(); fm1->stop(); rtc->stop(); fsh->stop(); filt_lp->stop(); fm2->stop(); rsp->stop();
        rsp->output_stream->stopReader();
    }

    void NOAA2HRPTDemodModule::stop() {
        BaseDemodModule::stop();
        filt1->stop(); fm1->stop(); rtc->stop(); fsh->stop(); filt_lp->stop(); fm2->stop(); rsp->stop();
        rsp->output_stream->stopReader();
    }

    void NOAA2HRPTDemodModule::drawUI(bool window)
    {
        ImGui::Begin(name.c_str(), NULL, window ? 0 : NOWINDOW_FLAGS);
        ImGui::BeginGroup(); constellation.draw(); ImGui::EndGroup();
        ImGui::SameLine();
        ImGui::BeginGroup();
        {
            ImGui::Button("Signal", { 200 * ui_scale, 20 * ui_scale });
            if (!streamingInput)
                if (ImGui::Checkbox("Show FFT", &show_fft)) fft_splitter->set_enabled("fft", show_fft);
            ImGui::Checkbox("Save Audio (u8)", &save_audio);
        }
        ImGui::EndGroup();
        if (!streamingInput) ImGui::ProgressBar((double)progress / (double)filesize, ImVec2(ImGui::GetContentRegionAvail().x, 20 * ui_scale));
        drawStopButton();
        ImGui::End();
        drawFFT();
    }

    std::string NOAA2HRPTDemodModule::getID() { return "noaa2_hrpt_demod"; }
    std::vector<std::string> NOAA2HRPTDemodModule::getParameters() { return {"save audio"}; }
    std::shared_ptr<ProcessingModule> NOAA2HRPTDemodModule::getInstance(std::string input_file, std::string output_file_hint, nlohmann::json parameters) { return std::make_shared<NOAA2HRPTDemodModule>(input_file, output_file_hint, parameters); }
}