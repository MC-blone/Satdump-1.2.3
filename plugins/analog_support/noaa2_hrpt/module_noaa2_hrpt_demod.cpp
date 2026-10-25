#include "module_noaa2_hrpt_demod.h"
#include "common/dsp/filter/firdes.h"
#include "common/dsp/io/file_source.h"
#include "common/dsp/io/wav_writer.h"
#include "common/dsp/resamp/rational_resampler.h"
#include "common/dsp/filter/fir.h"
#include "core/config.h"
#include "logger.h"
#include "imgui/imgui.h"
#include "common/utils.h"
#include <volk/volk.h>
#include <vector>
#include <numeric>
#include <cmath>
#include <algorithm>

namespace noaa2_hrpt
{
    NOAA2HRPTDemodModule::NOAA2HRPTDemodModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters) 
        : ProcessingModule(input_file, output_file_hint, parameters),
          constellation(1.0, 0.15, demod_constellation_size)
    {
        if (parameters.count("samplerate") > 0) d_samplerate = parameters["samplerate"].get<double>();
        if (parameters.count("baseband_format") > 0) baseband_format = parameters["baseband_format"].get<std::string>();
        if (parameters.count("save_wav") > 0) save_wav = parameters["save_wav"].get<bool>();
        if (parameters.count("save_audio") > 0) save_audio = parameters["save_audio"].get<bool>();

        play_audio = satdump::config::main_cfg["user_interface"]["play_audio"]["value"].get<bool>();
    }

    NOAA2HRPTDemodModule::~NOAA2HRPTDemodModule() {}

    void NOAA2HRPTDemodModule::process()
    {
        double subcarrier = 99000.0;
        double bw = 200000.0;

        if (input_data_type == DATA_FILE) {
            filesize = satdump::getFilesize(d_input_file);
            baseband_io = std::make_shared<dsp::StrictFileSource>();
            if (!baseband_io->open(d_input_file, baseband_format)) {
                logger->error("Failed to open input file!");
                return;
            }
            filt1 = std::make_shared<dsp::FIRBlock<complex_t>>(baseband_io->output_stream, dsp::firdes::band_pass(1, d_samplerate, subcarrier - bw/2, subcarrier + bw/2, 10000));
        } else {
            filesize = 0;
            input_stream = std::make_shared<dsp::stream<complex_t>>();
            filt1 = std::make_shared<dsp::FIRBlock<complex_t>>(input_stream, dsp::firdes::band_pass(1, d_samplerate, subcarrier - bw/2, subcarrier + bw/2, 10000));
        }

        fm1 = std::make_shared<dsp::QuadratureDemodBlock>(filt1->output_stream, 2.0f / float(M_PI));
        rtc = std::make_shared<dsp::RealToComplexBlock>(fm1->output_stream);
        fsh = std::make_shared<dsp::FreqShiftBlock>(rtc->output_stream, d_samplerate, -subcarrier);
        filt_lp = std::make_shared<dsp::FIRBlock<complex_t>>(fsh->output_stream, dsp::firdes::low_pass(1, d_samplerate, 40000, 10000));
        fm2 = std::make_shared<dsp::QuadratureDemodBlock>(filt_lp->output_stream, 2.0f / float(M_PI));

        int g = std::gcd((int)audio_samplerate, (int)d_samplerate);
        rsp = std::make_shared<dsp::RationalResamplerBlock<float>>(fm2->output_stream, audio_samplerate / g, d_samplerate / g);

        if (input_data_type == DATA_FILE) baseband_io->start();
        filt1->start(); fm1->start(); rtc->start(); fsh->start(); filt_lp->start(); fm2->start(); rsp->start();

        const bool write_wav = save_wav || output_data_type == DATA_FILE;
        if (write_wav) {
            data_out = std::ofstream(d_output_file_hint + ".wav", std::ios::binary);
            d_output_files.push_back(d_output_file_hint + ".wav");
        }
        if (save_audio) {
            audio_out = std::ofstream(d_output_file_hint + "_audio.u8", std::ios::binary);
            d_output_files.push_back(d_output_file_hint + "_audio.u8");
        }

        std::vector<int16_t> output_wav_buffer(65536);
        std::vector<uint8_t> output_u8_buffer(65536);
        uint64_t final_data_size = 0;
        dsp::WavWriter wave_writer(data_out);
        if (write_wav && data_out.is_open()) wave_writer.write_header(audio_samplerate, 1);

        if (input_data_type != DATA_FILE && audio::has_sink()) {
            enable_audio = true;
            audio_sink = audio::get_default_sink();
            audio_sink->set_samplerate(audio_samplerate);
            audio_sink->start();
        }

        std::vector<int8_t> raw_in(65536 * 2);
        complex_t* read_buf = (complex_t*)malloc(65536 * sizeof(complex_t));

        while (input_active.load())
        {
            int avail = 0;

            if (input_data_type == DATA_FILE) {
                avail = rsp->output_stream->read();
                if (avail < 0) break;
                if (avail == 0) { rsp->output_stream->flush(); continue; }

                constellation.pushFloatAndGaussian(rsp->output_stream->readBuf, avail);
                for (int i = 0; i < avail; i++) {
                    if (rsp->output_stream->readBuf[i] > 1.0f) rsp->output_stream->readBuf[i] = 1.0f;
                    if (rsp->output_stream->readBuf[i] < -1.0f) rsp->output_stream->readBuf[i] = -1.0f;
                }
                volk_32f_s32f_convert_16i(output_wav_buffer.data(), (float *)rsp->output_stream->readBuf, 32767, avail);
                rsp->output_stream->flush();

                progress = baseband_io->get_progress();
            } else {
                int n = input_fifo->read(reinterpret_cast<uint8_t*>(raw_in.data()), 65536 * 2);
                if (n < 0) break;
                if (n == 0) continue;

                int ncomplex = n / 2;
                for (int i = 0; i < ncomplex; i++) {
                    read_buf[i] = complex_t(raw_in[i*2] / 127.0f, raw_in[i*2+1] / 127.0f);
                }
                memcpy(input_stream->writeBuf, read_buf, ncomplex * sizeof(complex_t));
                input_stream->swap(ncomplex);

                avail = rsp->output_stream->read();
                if (avail <= 0) { rsp->output_stream->flush(); continue; }

                constellation.pushFloatAndGaussian(rsp->output_stream->readBuf, avail);
                for (int i = 0; i < avail; i++) {
                    if (rsp->output_stream->readBuf[i] > 1.0f) rsp->output_stream->readBuf[i] = 1.0f;
                    if (rsp->output_stream->readBuf[i] < -1.0f) rsp->output_stream->readBuf[i] = -1.0f;
                }
                volk_32f_s32f_convert_16i(output_wav_buffer.data(), (float *)rsp->output_stream->readBuf, 32767, avail);
                rsp->output_stream->flush();
            }

            if (avail <= 0) continue;

            // 保存 u8
            if (save_audio && audio_out.is_open()) {
                for (int i = 0; i < avail; i++) {
                    int32_t val = (output_wav_buffer[i] >> 8) + 128;
                    output_u8_buffer[i] = (uint8_t)std::clamp(val, 0, 255);
                }
                audio_out.write(reinterpret_cast<const char*>(output_u8_buffer.data()), avail);
            }

            // 实时播放
            if (enable_audio && play_audio && audio_volume > 0.0f) {
                for (int i = 0; i < avail; i++) {
                    int32_t v = (int32_t)(output_wav_buffer[i] * audio_volume);
                    output_wav_buffer[i] = (int16_t)std::clamp(v, -32768, 32767);
                }
                audio_sink->push_samples(output_wav_buffer.data(), avail);
            }

            // 保存 WAV
            if (write_wav && data_out.is_open()) {
                data_out.write(reinterpret_cast<const char*>(output_wav_buffer.data()), avail * sizeof(int16_t));
                final_data_size += avail * sizeof(int16_t);
            }

            // 推给解码器
            if (output_data_type != DATA_FILE && output_fifo) {
                output_fifo->write(reinterpret_cast<uint8_t*>(output_wav_buffer.data()), avail * sizeof(int16_t));
            }
        }

        if (input_data_type == DATA_FILE) baseband_io->stop();
        rsp->output_stream->stopReader();
        filt1->stop(); fm1->stop(); rtc->stop(); fsh->stop(); filt_lp->stop(); fm2->stop(); rsp->stop();

        if (enable_audio && audio_sink) audio_sink->stop();
        if (write_wav && data_out.is_open()) { wave_writer.finish_header(final_data_size); data_out.close(); }
        if (audio_out.is_open()) audio_out.close();

        free(read_buf);
    }

    void NOAA2HRPTDemodModule::drawUI(bool window)
    {
        ImGui::Begin("NOAA-2 HRPT Demodulator", NULL, window ? 0 : NOWINDOW_FLAGS);

        ImGui::BeginGroup();
        constellation.draw();
        ImGui::EndGroup();

        ImGui::SameLine();
        ImGui::BeginGroup();
        {
            ImGui::TextColored(style::theme.green, "Mode: %s", input_data_type == DATA_FILE ? "File" : "Stream");
            if (enable_audio) {
                ImGui::SetNextItemWidth(150 * ui_scale);
                ImGui::SliderFloat("Volume", &audio_volume, 0.0f, 2.0f, "%.2fx");
            }
        }
        ImGui::EndGroup();

        if (input_data_type == DATA_FILE)
            ImGui::ProgressBar((double)progress / (double)filesize, ImVec2(ImGui::GetContentRegionAvail().x, 20 * ui_scale));

        ImGui::End();
    }

    std::string NOAA2HRPTDemodModule::getID() { return "noaa2_hrpt_demod"; }
    std::vector<std::string> NOAA2HRPTDemodModule::getParameters() { return {"samplerate", "baseband_format", "save_wav", "save_audio"}; }
    std::shared_ptr<ProcessingModule> NOAA2HRPTDemodModule::getInstance(std::string input_file, std::string output_file_hint, nlohmann::json parameters) { return std::make_shared<NOAA2HRPTDemodModule>(input_file, output_file_hint, parameters); }
}