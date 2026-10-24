#include "module_noaa2_hrpt_demod.h"
#include "common/dsp/filter/firdes.h"
#include "core/config.h"
#include "logger.h"
#include "imgui/imgui.h"
#include "common/wav.h" // ★ 用于自动识别 WAV 采样率
#include <volk/volk.h>
#include <vector>
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

        // ★ 确保基类不会因为 symbolrate=0 而除零崩溃
        if (d_symbolrate <= 0) d_symbolrate = 60000;
    }

    NOAA2HRPTDemodModule::~NOAA2HRPTDemodModule() {}

    void NOAA2HRPTDemodModule::init()
    {
        // ★ 核心修改：如果是文件模式，自动读取 WAV 文件头的真实采样率
        if (input_data_type == DATA_FILE)
        {
            wav::WavHeader hdr = wav::parseHeaderFromFileWav(d_input_file);
            if (wav::isValidWav(hdr) && hdr.samplerate > 0)
            {
                d_samplerate = hdr.samplerate;
                logger->info("Auto-detected WAV Samplerate: %d Hz", d_samplerate);
            }
        }

        BaseDemodModule::initb();

        double fs = d_samplerate;
        if (fs <= 0) fs = 2880000;
        
        double subcarrier = 99000.0;
        double bw = 200000.0;

        filt1 = std::make_shared<dsp::FIRBlock<complex_t>>(agc->output_stream, dsp::firdes::band_pass(1, fs, subcarrier - bw/2, subcarrier + bw/2, 10000));
        fm1 = std::make_shared<dsp::QuadratureDemodBlock>(filt1->output_stream, 2.0f / float(M_PI));
        rtc = std::make_shared<dsp::RealToComplexBlock>(fm1->output_stream);
        fsh = std::make_shared<dsp::FreqShiftBlock>(rtc->output_stream, fs, -subcarrier);
        filt_lp = std::make_shared<dsp::FIRBlock<complex_t>>(fsh->output_stream, dsp::firdes::low_pass(1, fs, 40000, 10000));
        fm2 = std::make_shared<dsp::QuadratureDemodBlock>(filt_lp->output_stream, 2.0f / float(M_PI));

        // ★ 初始化自定义重采样参数
        resample_step = fs / audio_samplerate;
        resample_phase = 0.0;
        has_last = false;
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
        filt1->start(); fm1->start(); rtc->start(); fsh->start(); filt_lp->start(); fm2->start();

        // 用于预分配输出缓冲的容器
        std::vector<int16_t> output_wav_buffer;
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
            int dat_size = fm2->output_stream->read();
            if (dat_size <= 0) {
                fm2->output_stream->flush();
                if (input_data_type == DATA_FILE && file_source->eof()) break;
                continue;
            }

            constellation.pushFloatAndGaussian(fm2->output_stream->readBuf, dat_size);

            for (int i = 0; i < dat_size; i++) {
                if (fm2->output_stream->readBuf[i] > 1.0f) fm2->output_stream->readBuf[i] = 1.0f;
                if (fm2->output_stream->readBuf[i] < -1.0f) fm2->output_stream->readBuf[i] = -1.0f;
            }

            // ★ 自定义流式重采样：区间累积插值法
            int max_out = (int)(dat_size / resample_step) + 2;
            output_wav_buffer.resize(max_out);
            int n_out = 0;

            for (int i = 0; i < dat_size; ++i) {
                float current = fm2->output_stream->readBuf[i];
                if (!has_last) {
                    last_sample = current;
                    has_last = true;
                    continue;
                }

                // 在 [last_sample, current] 之间按 step 插值输出
                while (resample_phase < 1.0) {
                    float val = last_sample * (1.0f - resample_phase) + current * resample_phase;
                    output_wav_buffer[n_out++] = (int16_t)(val * 32767);
                    resample_phase += resample_step;
                }
                resample_phase -= 1.0;
                last_sample = current;
            }

            // ★ 音频音量处理
            if (enable_audio && play_audio && audio_volume != 1.0f && audio_volume > 0.0f)
            {
                for (int i = 0; i < n_out; ++i)
                {
                    int32_t val = (int32_t)(output_wav_buffer[i] * audio_volume);
                    if (val > 32767) val = 32767;
                    if (val < -32768) val = -32768;
                    output_wav_buffer[i] = (int16_t)val;
                }
            }

            // 推入声卡
            if (enable_audio && play_audio && audio_volume > 0.0f && n_out > 0)
                audio_sink->push_samples(output_wav_buffer.data(), n_out);

            // 写入 WAV
            if (write_wav && data_out.is_open() && n_out > 0) {
                data_out.write(reinterpret_cast<const char *>(output_wav_buffer.data()), n_out * sizeof(int16_t));
                final_data_size += n_out * sizeof(int16_t);
            }
            
            // 推送到 FIFO 给下游解码器
            if (output_data_type != DATA_FILE && output_fifo && n_out > 0)
                output_fifo->write(reinterpret_cast<uint8_t *>(output_wav_buffer.data()), n_out * sizeof(int16_t));

            fm2->output_stream->flush();

            if (input_data_type == DATA_FILE && filesize > 0) {
                progress = std::min<uint64_t>(file_source->getPosition(), filesize);
            }
        }

        if (enable_audio && audio_sink) audio_sink->stop();
        if (write_wav && data_out.is_open()) { wave_writer.finish_header(final_data_size); data_out.close(); }
        
        BaseDemodModule::stop();
        filt1->stop(); fm1->stop(); rtc->stop(); fsh->stop(); filt_lp->stop(); fm2->stop();
        fm2->output_stream->stopReader();
    }

    void NOAA2HRPTDemodModule::stop() {
        BaseDemodModule::stop();
        filt1->stop(); fm1->stop(); rtc->stop(); fsh->stop(); filt_lp->stop(); fm2->stop();
        fm2->output_stream->stopReader();
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

            // ★ 恢复音频播放和音量控制面板
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