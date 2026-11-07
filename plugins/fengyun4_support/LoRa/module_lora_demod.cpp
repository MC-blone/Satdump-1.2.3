#include "module_lora_demod.h"
#include "logger.h"
#include "imgui/imgui.h"
#include "core/config.h"
#include <fftw3.h>
#include <cmath>
#include <cstring>
#include <numeric>

namespace
{
    static inline complex_t cmul(complex_t a, complex_t b)
    {
        complex_t r;
        r.real = a.real * b.real - a.imag * b.imag;
        r.imag = a.real * b.imag + a.imag * b.real;
        return r;
    }

    static inline complex_t cconj(complex_t a)
    {
        complex_t r;
        r.real = a.real;
        r.imag = -a.imag;
        return r;
    }

    static inline float cnorm(complex_t a)
    {
        return a.real * a.real + a.imag * a.imag;
    }
}

namespace fengyun4
{
    namespace emwin
    {
        LoRaDemodModule::LoRaDemodModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters)
            : BaseDemodModule(input_file, output_file_hint, parameters)
        {
            if (parameters.count("sf") > 0)
                sf = parameters["sf"].get<int>();
            if (parameters.count("bw") > 0)
                bw_hz = parameters["bw"].get<int>();

            if (sf < 7 || sf > 12)
                sf = 9;
            if (bw_hz <= 0)
                bw_hz = 812500;
            fft_size = 1 << sf;

            MIN_SPS = 1.0f;
            MAX_SPS = 1e9f;

            if (d_symbolrate <= 0)
                d_symbolrate = (int)d_samplerate;

            name = "FY-4A EMWIN LoRa Demod";
            show_freq = false;

            soft_buffer = new int8_t[fft_size];
            if (soft_buffer)
                memset(soft_buffer, 0, (size_t)fft_size);
        }

        LoRaDemodModule::~LoRaDemodModule()
        {
            if (soft_buffer)
            {
                delete[] soft_buffer;
                soft_buffer = nullptr;
            }
        }

        void LoRaDemodModule::init()
        {
            BaseDemodModule::initb();
        }

        void LoRaDemodModule::process()
        {
            if (input_data_type == DATA_FILE)
                filesize = file_source->getFilesize();
            else
                filesize = 0;

            data_out = std::ofstream(d_output_file_hint + ".soft", std::ios::binary);
            d_output_files.push_back(d_output_file_hint + ".soft");

            logger->info("LoRa demod output: " + d_output_file_hint + ".soft");
            logger->info("SF: " + std::to_string(sf) + " BW: " + std::to_string(bw_hz));

            BaseDemodModule::start();
            if (agc)
                agc->start();

            if (!agc)
            {
                logger->error("AGC block not initialized");
                return;
            }

            unsigned g = std::gcd((unsigned)d_samplerate, (unsigned)bw_hz);
            if (g == 0)
                g = 1;
            auto rsp = std::make_shared<dsp::RationalResamplerBlock<complex_t>>(
                agc->output_stream,
                (unsigned)(bw_hz / g),
                (unsigned)(d_samplerate / g));
            rsp->start();

            const int sym_len = fft_size;
            if (sym_len <= 0 || !soft_buffer)
            {
                rsp->stop();
                BaseDemodModule::stop();
                return;
            }

            std::vector<complex_t> chirp((size_t)fft_size);
            const float pi_f = 3.14159265358979323846f;
            for (int i = 0; i < fft_size; i++)
            {
                float t = (float)i / (float)bw_hz;
                float phase = pi_f * (float)bw_hz * t * t;
                chirp[(size_t)i].real = std::cos(phase);
                chirp[(size_t)i].imag = std::sin(phase);
            }

            std::vector<complex_t> fft_in((size_t)fft_size);
            std::vector<complex_t> fft_out((size_t)fft_size);

            fftwf_plan plan = fftwf_plan_dft_1d(
                fft_size,
                reinterpret_cast<fftwf_complex *>(fft_in.data()),
                reinterpret_cast<fftwf_complex *>(fft_out.data()),
                FFTW_FORWARD, FFTW_ESTIMATE);

            if (!plan)
            {
                logger->error("Failed to create FFT plan");
                rsp->stop();
                BaseDemodModule::stop();
                return;
            }

            std::vector<complex_t> rolling;
            rolling.reserve((size_t)sym_len * 8);

            time_t lastTime = 0;
            int total_syms = 0;

            while (demod_should_run())
            {
                int n = rsp->output_stream->read();
                if (n > 0 && rsp->output_stream->readBuf != nullptr)
                {
                    complex_t *buf = rsp->output_stream->readBuf;
                    for (int i = 0; i < n; i++)
                        rolling.push_back(buf[i]);
                }
                rsp->output_stream->flush();

                while ((int)rolling.size() >= sym_len)
                {
                    for (int i = 0; i < sym_len; i++)
                        fft_in[(size_t)i] = cmul(rolling[(size_t)i], cconj(chirp[(size_t)i]));

                    fftwf_execute(plan);

                    int peak = 0;
                    float peak_val = 0.0f;
                    for (int i = 0; i < fft_size; i++)
                    {
                        float m = cnorm(fft_out[(size_t)i]);
                        if (m > peak_val)
                        {
                            peak_val = m;
                            peak = i;
                        }
                    }

                    for (int b = 0; b < sf; b++)
                    {
                        int bit = (peak >> (sf - 1 - b)) & 1;
                        soft_buffer[b] = bit ? (int8_t)100 : (int8_t)-100;
                    }
                    data_out.write((char *)soft_buffer, sf);

                    total_syms++;
                    rolling.erase(rolling.begin(), rolling.begin() + sym_len);
                }

                last_syms = total_syms;

                if (input_data_type == DATA_FILE && file_source)
                    progress = file_source->getPosition();

                if (time(NULL) % 10 == 0 && lastTime != time(NULL))
                {
                    lastTime = time(NULL);
                    if (filesize > 0)
                    {
                        double pct = round(((double)progress / (double)filesize) * 1000.0) / 10.0;
                        logger->info("Progress " + std::to_string(pct) + "%%");
                    }
                }
            }

            fftwf_destroy_plan(plan);
            data_out.close();
            rsp->stop();
            BaseDemodModule::stop();
        }

        void LoRaDemodModule::drawUI(bool window)
        {
            ImGui::Begin("FY-4A EMWIN LoRa Demod", NULL, window ? 0 : NOWINDOW_FLAGS);

            ImGui::Text("SF: %d   BW: %d Hz", sf, bw_hz);
            ImGui::Text("Symbols processed: %d", last_syms);

            if (input_data_type == DATA_FILE && filesize > 0)
                ImGui::ProgressBar((double)progress / (double)filesize, ImVec2(ImGui::GetContentRegionAvail().x, 20 * ui_scale));

            drawStopButton();
            ImGui::End();
        }

        std::string LoRaDemodModule::getID()
        {
            return "lora_demod";
        }

        std::vector<std::string> LoRaDemodModule::getParameters()
        {
            return {"symbolrate", "sf", "bw"};
        }

        std::shared_ptr<ProcessingModule> LoRaDemodModule::getInstance(std::string input_file, std::string output_file_hint, nlohmann::json parameters)
        {
            return std::make_shared<LoRaDemodModule>(input_file, output_file_hint, parameters);
        }
    }
}