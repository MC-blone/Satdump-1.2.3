#include "module_noaa2_hrpt_demod.h"
#include "common/dsp/filter/firdes.h"
#include "core/config.h"
#include "logger.h"
#include "imgui/imgui.h"
#include <volk/volk.h>

namespace noaa2_hrpt
{
    NOAA2HRPTDemodModule::NOAA2HRPTDemodModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters) 
        : BaseDemodModule(input_file, output_file_hint, parameters)
    {
        if (parameters.count("save_wav") > 0)
            save_wav = parameters["save_wav"].get<bool>();

        // 读取 save audio 配置
        if (parameters.count("save audio") > 0)
            save_audio = parameters["save audio"].get<bool>();

        name = "NOAA-2 HRPT Demodulator (FM)";
        show_freq = false;
        
        play_audio = satdump::config::main_cfg["user_interface"]["play_audio"]["value"].get<bool>();
        
        constellation.d_hscale = 1.0;
        constellation.d_vscale = 0.5;
        MIN_SPS = 1;
        MAX_SPS = 1000.0;
    }

    void NOAA2HRPTDemodModule::init()
    {
        BaseDemodModule::initb();

        res1 = std::make_shared<dsp::RationalResamplerBlock<complex_t>>(agc->output_stream, d_symbolrate, 1000000);
        qua1 = std::make_shared<dsp::QuadratureDemodBlock>(res1->output_stream, dsp::hz_to_rad(1000000 / 2, 1000000));
        rtc = std::make_shared<dsp::RealToComplexBlock>(qua1->output_stream);
        fshift = std::make_shared<dsp::FreqShiftBlock>(rtc->output_stream, 1000000, -99000.0);
        res2 = std::make_shared<dsp::RationalResamplerBlock<complex_t>>(fshift->output_stream, 6, 100);
        qua2 = std::make_shared<dsp::QuadratureDemodBlock>(res2->output_stream, dsp::hz_to_rad(60000 / 2, 60000));
    }

    NOAA2HRPTDemodModule::~NOAA2HRPTDemodModule() {}

    void NOAA2HRPTDemodModule::process()
    {
        if (input_data_type == DATA_FILE)
            filesize = file_source->getFilesize();
        else
            filesize = 0;

        if (save_wav || output_data_type == DATA_FILE)
        {
            data_out = std::ofstream(d_output_file_hint + ".wav", std::ios::binary);
            d_output_files.push_back(d_output_file_hint + ".wav");
        }

        // 打开原始音频文件
        if (save_audio)
        {
            audio_out = std::ofstream(d_output_file_hint + "_audio.u8", std::ios::binary);
            d_output_files.push_back(d_output_file_hint + "_audio.u8");
            logger->info("Saving raw audio to " + d_output_file_hint + "_audio.u8");
        }

        logger->info("Demodulating to " + d_output_file_hint + ".wav");

        time_t lastTime = 0;
        BaseDemodModule::start();
        
        res1->start();
        qua1->start();
        rtc->start();
        fshift->start();
        res2->start();
        qua2->start();

        int16_t *output_wav_buffer = new int16_t[d_buffer_size * 100];
        uint8_t *output_u8_buffer = new uint8_t[d_buffer_size * 100]; 
        uint64_t final_data_size = 0;
        dsp::WavWriter wave_writer(data_out);
        if (save_wav || output_data_type == DATA_FILE)
            wave_writer.write_header(60000, 1);

        if (input_data_type != DATA_FILE && audio::has_sink())
        {
            enable_audio = true;
            audio_sink = audio::get_default_sink();
            audio_sink->set_samplerate(60000);
            audio_sink->start();
        }

        while (demod_should_run())
        {
            int dat_size = qua2->output_stream->read();
            if (dat_size <= 0)
            {
                qua2->output_stream->flush();
                continue;
            }

            constellation.pushFloatAndGaussian(qua2->output_stream->readBuf, dat_size);

            for (int i = 0; i < dat_size; i++)
            {
                if (qua2->output_stream->readBuf[i] > 1.0f) qua2->output_stream->readBuf[i] = 1.0f;
                if (qua2->output_stream->readBuf[i] < -1.0f) qua2->output_stream->readBuf[i] = -1.0f;
            }

            volk_32f_s32f_convert_16i(output_wav_buffer, (float *)qua2->output_stream->readBuf, 32767, dat_size);

            if (save_audio && audio_out.is_open())
            {
                for (int i = 0; i < dat_size; i++)
                {
                    int32_t val = (output_wav_buffer[i] >> 8) + 128;
                    if (val < 0) val = 0;
                    if (val > 255) val = 255;
                    output_u8_buffer[i] = (uint8_t)val;
                }
                audio_out.write((char *)output_u8_buffer, dat_size);
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
            {
                audio_sink->push_samples(output_wav_buffer, dat_size);
            }

            if (save_wav || output_data_type == DATA_FILE)
            {
                data_out.write((char *)output_wav_buffer, dat_size * sizeof(int16_t));
                final_data_size += dat_size * sizeof(int16_t);
            }
            if (output_data_type != DATA_FILE)
            {
                output_fifo->write((uint8_t *)output_wav_buffer, dat_size * sizeof(int16_t));
            }

            qua2->output_stream->flush();

            if (input_data_type == DATA_FILE)
                progress = file_source->getPosition();

            if (time(NULL) % 10 == 0 && lastTime != time(NULL))
            {
                lastTime = time(NULL);
                logger->info("Progress " + std::to_string(round(((double)progress / (double)filesize) * 1000.0) / 10.0) + "%%");
            }
        }

        if (enable_audio)
            audio_sink->stop();

        if (save_wav || output_data_type == DATA_FILE)
        {
            wave_writer.finish_header(final_data_size);
            data_out.close();
        }
        
        if (audio_out.is_open())
            audio_out.close();

        delete[] output_wav_buffer;
        delete[] output_u8_buffer;
        
        BaseDemodModule::stop();
        res1->stop();
        qua1->stop();
        rtc->stop();
        fshift->stop();
        res2->stop();
        qua2->stop();
        qua2->output_stream->stopReader();
    }

    // ★ 修复：补充缺失的 stop() 实现
    void NOAA2HRPTDemodModule::stop()
    {
        // 停止所有 DSP 模块
        BaseDemodModule::stop();
        res1->stop();
        qua1->stop();
        rtc->stop();
        fshift->stop();
        res2->stop();
        qua2->stop();
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
                if (play_audio)
                {
                    color = style::theme.green.Value;
                    btn_icon = u8"\uF028##audio";
                    label = "Audio Playing";
                }
                else
                {
                    color = style::theme.red.Value;
                    btn_icon = u8"\uF026##audio";
                    label = "Audio Muted";
                }

                ImGui::PushStyleColor(ImGuiCol_Text, color);
                if (ImGui::Button(btn_icon))
                    play_audio = !play_audio;
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