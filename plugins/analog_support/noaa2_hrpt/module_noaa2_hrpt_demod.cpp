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
        if (parameters.count("save_wav") > 0)
            save_wav = parameters["save_wav"].get<bool>();
        if (parameters.count("save_audio") > 0)
            save_audio = parameters["save_audio"].get<bool>();
        if (parameters.count("audio_samplerate") > 0)
            audio_samplerate = parameters["audio_samplerate"].get<long>();

        // 放宽 SPS 范围：不触发基类强制重采样（参考 generic_analog_demod）
        MIN_SPS = 0.5f;
        MAX_SPS = 1e9f;

        // d_symbolrate 兜底为 d_samplerate，让 input_sps = 1.0，不会 <1 抛异常
        if (parameters.count("symbolrate") > 0)
            d_symbolrate = parameters["symbolrate"].get<int>();
        else
            d_symbolrate = (int)d_samplerate;

        name = "NOAA-2 HRPT Demodulator (FM)";
        show_freq = false;

        constellation.d_hscale = 1.0f;
        constellation.d_vscale = 0.5f;

        play_audio = satdump::config::main_cfg["user_interface"]["play_audio"]["value"].get<bool>();
    }

    NOAA2HRPTDemodModule::~NOAA2HRPTDemodModule() {}

    void NOAA2HRPTDemodModule::init()
    {
        // 建好基类的文件源/AGC/FFT 链。
        // MIN_SPS=0.5, MAX_SPS=1e9 → 只要 d_samplerate>=1Msps，input_sps=1.0 在范围内 → 不重采样
        // agc->output_stream 采样率 = d_samplerate（原始输入采样率）
        BaseDemodModule::initb();

        double subcarrier = 99000.0;
        double bw = 200000.0;

        // 99 kHz 副载波带通（对应 Python StreamBandpass(fs, 200000, 99000)）
        filt1 = std::make_shared<dsp::FIRBlock<complex_t>>(
            agc->output_stream,
            dsp::firdes::band_pass(1.0, (double)d_samplerate,
                                   subcarrier - bw / 2.0,
                                   subcarrier + bw / 2.0,
                                   10000.0));

        // 第一次 FM 解调（对应 StreamFMDemod）
        fm1 = std::make_shared<dsp::QuadratureDemodBlock>(filt1->output_stream, 2.0f / float(M_PI));

        // real → complex
        rtc = std::make_shared<dsp::RealToComplexBlock>(fm1->output_stream);

        // 移频到 0（对应 StreamFreqShift(-99000, fs)）
        fsh = std::make_shared<dsp::FreqShiftBlock>(rtc->output_stream, (double)d_samplerate, -subcarrier);

        // 40 kHz 低通（对应 StreamBandpass(fs, 80000, 0)）
        filt_lp = std::make_shared<dsp::FIRBlock<complex_t>>(
            fsh->output_stream,
            dsp::firdes::low_pass(1.0, (double)d_samplerate, 40000.0, 10000.0));

        // 第二次 FM 解调
        fm2 = std::make_shared<dsp::QuadratureDemodBlock>(filt_lp->output_stream, 2.0f / float(M_PI));

        // 最后一步：重采样到 audio_samplerate（对应 StreamResampler）
        unsigned g = std::gcd((unsigned)audio_samplerate, (unsigned)d_samplerate);
        rsp = std::make_shared<dsp::RationalResamplerBlock<float>>(
            fm2->output_stream,
            (unsigned)(audio_samplerate / g),
            (unsigned)(d_samplerate / g));
    }

    void NOAA2HRPTDemodModule::process()
    {
        if (input_data_type == DATA_FILE)
            filesize = file_source->getFilesize();
        else
            filesize = 0;

        const bool write_wav = save_wav || output_data_type == DATA_FILE;
        if (write_wav)
        {
            data_out = std::ofstream(d_output_file_hint + ".wav", std::ios::binary);
            d_output_files.push_back(d_output_file_hint + ".wav");
        }
        if (save_audio)
        {
            audio_out = std::ofstream(d_output_file_hint + "_audio.u8", std::ios::binary);
            d_output_files.push_back(d_output_file_hint + "_audio.u8");
        }

        logger->info("Using input baseband " + d_input_file);
        logger->info("Demodulating to " + d_output_file_hint + ".wav");
        logger->info("Buffer size : " + std::to_string(d_buffer_size));

        time_t lastTime = 0;

        BaseDemodModule::start();
        filt1->start();
        fm1->start();
        rtc->start();
        fsh->start();
        filt_lp->start();
        fm2->start();
        rsp->start();

        int16_t *output_wav_buffer = new int16_t[d_buffer_size * 100];
        std::vector<uint8_t> output_u8_buffer(d_buffer_size * 100);
        uint64_t final_data_size = 0;

        dsp::WavWriter wave_writer(data_out);
        if (write_wav)
            wave_writer.write_header(audio_samplerate, 1);

        if (input_data_type != DATA_FILE && audio::has_sink())
        {
            enable_audio = true;
            audio_sink = audio::get_default_sink();
            audio_sink->set_samplerate((uint64_t)audio_samplerate);
            audio_sink->start();
        }

        while (demod_should_run())
        {
            int dat_size = rsp->output_stream->read();
            if (dat_size <= 0)
            {
                rsp->output_stream->flush();
                continue;
            }

            constellation.pushFloatAndGaussian(rsp->output_stream->readBuf, dat_size);

            for (int i = 0; i < dat_size; i++)
            {
                if (rsp->output_stream->readBuf[i] > 1.0f)
                    rsp->output_stream->readBuf[i] = 1.0f;
                if (rsp->output_stream->readBuf[i] < -1.0f)
                    rsp->output_stream->readBuf[i] = -1.0f;
            }

            volk_32f_s32f_convert_16i(output_wav_buffer, (float *)rsp->output_stream->readBuf, 32767, dat_size);

            if (save_audio && audio_out.is_open())
            {
                for (int i = 0; i < dat_size; i++)
                {
                    int32_t val = (output_wav_buffer[i] >> 8) + 128;
                    output_u8_buffer[i] = (uint8_t)std::clamp(val, 0, 255);
                }
                audio_out.write(reinterpret_cast<const char *>(output_u8_buffer.data()), dat_size);
            }

            // 实时播放（用独立缓冲，不污染 wav/fifo）
            if (enable_audio && play_audio && audio_volume > 0.0f)
            {
                static thread_local std::vector<int16_t> play_buf;
                if ((int)play_buf.size() < dat_size)
                    play_buf.resize(dat_size);
                for (int i = 0; i < dat_size; i++)
                {
                    int32_t v = (int32_t)(output_wav_buffer[i] * audio_volume);
                    play_buf[i] = (int16_t)std::clamp(v, -32768, 32767);
                }
                audio_sink->push_samples(play_buf.data(), dat_size);
            }

            if (write_wav)
            {
                data_out.write(reinterpret_cast<char *>(output_wav_buffer), dat_size * sizeof(int16_t));
                final_data_size += dat_size * sizeof(int16_t);
            }
            if (output_data_type != DATA_FILE)
            {
                output_fifo->write(reinterpret_cast<uint8_t *>(output_wav_buffer), dat_size * sizeof(int16_t));
            }

            rsp->output_stream->flush();

            if (input_data_type == DATA_FILE)
                progress = file_source->getPosition();

            if (time(NULL) % 10 == 0 && lastTime != time(NULL))
            {
                lastTime = time(NULL);
                logger->info("Progress " + std::to_string(round(((double)progress / (double)filesize) * 1000.0) / 10.0) + "%%");
            }
        }

        if (enable_audio && audio_sink)
            audio_sink->stop();

        if (write_wav)
        {
            wave_writer.finish_header(final_data_size);
            data_out.close();
        }
        if (audio_out.is_open())
            audio_out.close();

        delete[] output_wav_buffer;

        logger->info("Demodulation finished");

        if (input_data_type == DATA_FILE)
            stop();
    }

    void NOAA2HRPTDemodModule::stop()
    {
        BaseDemodModule::stop();
        filt1->stop();
        fm1->stop();
        rtc->stop();
        fsh->stop();
        filt_lp->stop();
        fm2->stop();
        rsp->stop();
        rsp->output_stream->stopReader();
    }

    void NOAA2HRPTDemodModule::drawUI(bool window)
    {
        ImGui::Begin(name.c_str(), NULL, window ? 0 : NOWINDOW_FLAGS);

        ImGui::BeginGroup();
        constellation.draw();
        ImGui::EndGroup();

        ImGui::SameLine();

        ImGui::BeginGroup();
        {
            ImGui::Button("Signal", {200 * ui_scale, 20 * ui_scale});
            if (!streamingInput)
                if (ImGui::Checkbox("Show FFT", &show_fft))
                    fft_splitter->set_enabled("fft", show_fft);

            if (enable_audio)
            {
                ImGui::SetNextItemWidth(150 * ui_scale);
                ImGui::SliderFloat("Volume", &audio_volume, 0.0f, 2.0f, "%.2fx");
            }
        }
        ImGui::EndGroup();

        if (!streamingInput)
            ImGui::ProgressBar((double)progress / (double)filesize, ImVec2(ImGui::GetContentRegionAvail().x, 20 * ui_scale));

        drawStopButton();
        ImGui::End();
        drawFFT();
    }

    std::string NOAA2HRPTDemodModule::getID()
    {
        return "noaa2_hrpt_demod";
    }

    std::vector<std::string> NOAA2HRPTDemodModule::getParameters()
    {
        return {"save_wav", "save_audio", "audio_samplerate"};
    }

    std::shared_ptr<ProcessingModule> NOAA2HRPTDemodModule::getInstance(std::string input_file, std::string output_file_hint, nlohmann::json parameters)
    {
        return std::make_shared<NOAA2HRPTDemodModule>(input_file, output_file_hint, parameters);
    }
}