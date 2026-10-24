#include "module_noaa2_hrpt_demod.h"
#include "common/dsp/filter/firdes.h"
#include "core/config.h"
#include "logger.h"
#include "imgui/imgui.h"
#include <volk/volk.h>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <cmath>

namespace noaa2_hrpt
{
    NOAA2HRPTDemodModule::NOAA2HRPTDemodModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters) 
        : BaseDemodModule(input_file, output_file_hint, parameters)
    {
        if (parameters.count("save_wav") > 0)
            save_wav = parameters["save_wav"].get<bool>();
        if (parameters.count("save audio") > 0)
            save_audio = parameters["save audio"].get<bool>();

        name = "NOAA-2 HRPT Demodulator (FM)";
        show_freq = false;
        
        play_audio = satdump::config::main_cfg["user_interface"]["play_audio"]["value"].get<bool>();
        
        constellation.d_hscale = 1.0;
        constellation.d_vscale = 0.5;
        MIN_SPS = 1;
        MAX_SPS = 1000.0;

        if (d_symbolrate <= 0) 
            d_symbolrate = 60000;
    }

    void NOAA2HRPTDemodModule::init()
    {
        BaseDemodModule::initb();

        double src_samplerate = d_samplerate;
        if (src_samplerate <= 0) src_samplerate = 2880000;

        audio_samplerate = 60000.0;

        // 1. 降采样：源采样率 -> 1 MHz
        res1 = std::make_shared<dsp::RationalResamplerBlock<complex_t>>(agc->output_stream, src_samplerate, 1000000);
        
        // 2. 第一次鉴频：注意增益改为 2.0 / PI，因为内部会取倒数
        qua1 = std::make_shared<dsp::QuadratureDemodBlock>(res1->output_stream, 2.0f / float(M_PI));
        
        // 3. 实转复
        rtc = std::make_shared<dsp::RealToComplexBlock>(qua1->output_stream);
        
        // 4. 频移 -99 kHz
        fshift = std::make_shared<dsp::FreqShiftBlock>(rtc->output_stream, 1000000, -99000.0);
        
        // 5. 阶段 2：1 MHz 降采样至 60 kHz（采用严格整数比 3/50，防止行数暴涨）
        res2 = std::make_shared<dsp::RationalResamplerBlock<complex_t>>(fshift->output_stream, 3, 50);
        
        // 6. 第二次鉴频：增益同步改为 2.0 / PI
        qua2 = std::make_shared<dsp::QuadratureDemodBlock>(res2->output_stream, 2.0f / float(M_PI));
    }

    NOAA2HRPTDemodModule::~NOAA2HRPTDemodModule() {}

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
            logger->info("Saving raw audio to " + d_output_file_hint + "_audio.u8");
        }

        logger->info("Demodulating to " + d_output_file_hint + ".wav");

        time_t lastTime = 0;
        BaseDemodModule::start();
        
        res1->start(); qua1->start(); rtc->start(); fshift->start(); res2->start(); qua2->start();

        // 改用 vector 自动管理内存
        std::vector<int16_t> output_wav_buffer(std::max<size_t>(static_cast<size_t>(d_buffer_size) * 100, 65536));
        std::vector<uint8_t> output_u8_buffer(output_wav_buffer.size());

        uint64_t final_data_size = 0;
        dsp::WavWriter wave_writer(data_out);
        
        if (write_wav && data_out.is_open())
            wave_writer.write_header(audio_samplerate, 1);

        if (input_data_type != DATA_FILE && audio::has_sink())
        {
            enable_audio = true;
            audio_sink = audio::get_default_sink();
            audio_sink->set_samplerate(audio_samplerate);
            audio_sink->start();
        }

        while (demod_should_run())
        {
            int dat_size = qua2->output_stream->read();
            if (dat_size <= 0)
            {
                qua2->output_stream->flush();
                if (input_data_type == DATA_FILE && file_source->eof())
                    break;
                continue;
            }

            // 越界保护
            if (dat_size > static_cast<int>(output_wav_buffer.size()))
            {
                logger->error("NOAA-2 output block exceeds buffer capacity");
                break;
            }

            constellation.pushFloatAndGaussian(qua2->output_stream->readBuf, dat_size);

            for (int i = 0; i < dat_size; i++)
            {
                if (qua2->output_stream->readBuf[i] > 1.0f) qua2->output_stream->readBuf[i] = 1.0f;
                if (qua2->output_stream->readBuf[i] < -1.0f) qua2->output_stream->readBuf[i] = -1.0f;
            }

            volk_32f_s32f_convert_16i(output_wav_buffer.data(), (float *)qua2->output_stream->readBuf, 32767, dat_size);

            if (save_audio && audio_out.is_open())
            {
                for (int i = 0; i < dat_size; i++)
                {
                    int32_t val = (output_wav_buffer[i] >> 8) + 128;
                    if (val < 0) val = 0;
                    if (val > 255) val = 255;
                    output_u8_buffer[i] = (uint8_t)val;
                }
                audio_out.write(reinterpret_cast<const char *>(output_u8_buffer.data()), static_cast<std::streamsize>(dat_size));
            }

            if (audio_volume != 1.0f && audio_volume != 0.0f)
            {
                for (int i = 0; i < dat_size; i++)
                {
                    int32_t val = (int32_t)(output_wav_buffer[i] * audio_volume);
                    if (val > 32767) val = 32767;
                    if (val < -32768) val = -32768;
                    output_wav_buffer[i] = (int16_t)val;
                }
            }

            if (enable_audio && play_audio && audio_volume > 0.0f)
                audio_sink->push_samples(output_wav_buffer.data(), dat_size);

            if (write_wav && data_out.is_open())
            {
                data_out.write(reinterpret_cast<const char *>(output_wav_buffer.data()), static_cast<std::streamsize>(dat_size * sizeof(int16_t)));
                final_data_size += dat_size * sizeof(int16_t);
            }
            if (output_data_type != DATA_FILE && output_fifo)
                output_fifo->write(reinterpret_cast<uint8_t *>(output_wav_buffer.data()), dat_size * sizeof(int16_t));

            qua2->output_stream->flush();

            if (input_data_type == DATA_FILE && filesize > 0)
            {
                auto pos = file_source->getPosition();
                progress = std::min<uint64_t>(pos, filesize);
            }

            if (time(NULL) % 10 == 0 && lastTime != time(NULL))
            {
                lastTime = time(NULL);
                if (filesize > 0)
                    logger->info("Progress %.1f%%", 100.0 * std::min<double>(1.0, double(progress) / double(filesize)));
            }
        }

        if (enable_audio && audio_sink) audio_sink->stop();

        if (write_wav && data_out.is_open())
        {
            wave_writer.finish_header(final_data_size);
            data_out.close();
        }
        if (audio_out.is_open()) audio_out.close();
        
        BaseDemodModule::stop();
        res1->stop(); qua1->stop(); rtc->stop(); fshift->stop(); res2->stop(); qua2->stop();
        qua2->output_stream->stopReader();
    }

    void NOAA2HRPTDemodModule::stop()
    {
        BaseDemodModule::stop();
        res1->stop(); qua1->stop(); rtc->stop(); fshift->stop(); res2->stop(); qua2->stop();
        qua2->output_stream->stopReader();
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
            ImGui::Button("Signal", { 200 * ui_scale, 20 * ui_scale });
            if (!streamingInput)
                if (ImGui::Checkbox("Show FFT", &show_fft))
                    fft_splitter->set_enabled("fft", show_fft);

            ImGui::Checkbox("Save Audio (u8)", &save_audio);

            if (enable_audio)
            {
                const char *btn_icon, *label;
                ImVec4 color;
                if (play_audio) { color = style::theme.green.Value; btn_icon = u8"\uF028##audio"; label = "Audio Playing"; }
                else { color = style::theme.red.Value; btn_icon = u8"\uF026##audio"; label = "Audio Muted"; }

                ImGui::PushStyleColor(ImGuiCol_Text, color);
                if (ImGui::Button(btn_icon)) play_audio = !play_audio;
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::TextUnformatted(label);

                ImGui::SetNextItemWidth(200 * ui_scale);
                ImGui::SliderFloat("Volume##audiovol", &audio_volume, 0.0f, 2.0f, "%.2fx");
            }
        }
        ImGui::EndGroup();

        if (!streamingInput)
            ImGui::ProgressBar((double)progress / (double)filesize, ImVec2(ImGui::GetContentRegionAvail().x, 20 * ui_scale));

        drawStopButton();
        ImGui::End();
        drawFFT();
    }

    std::string NOAA2HRPTDemodModule::getID() { return "noaa2_hrpt_demod"; }
    std::vector<std::string> NOAA2HRPTDemodModule::getParameters() { return {"save audio"}; }
    std::shared_ptr<ProcessingModule> NOAA2HRPTDemodModule::getInstance(std::string input_file, std::string output_file_hint, nlohmann::json parameters) { return std::make_shared<NOAA2HRPTDemodModule>(input_file, output_file_hint, parameters); }
}