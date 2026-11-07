#include "module_emwin_cadu_decoder.h"
#include "logger.h"
#include "imgui/imgui.h"
#include <cstring>
#include <cmath>
#include <ctime>
#include <algorithm>

namespace fengyun4
{
    namespace emwin
    {
        EmwinCaduDecoderModule::EmwinCaduDecoderModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters)
            : ProcessingModule(input_file, output_file_hint, parameters)
        {
            if (parameters.count("sf") > 0)
                sf = parameters["sf"].get<int>();
            if (sf < 7 || sf > 12)
                sf = 9;
        }

        EmwinCaduDecoderModule::~EmwinCaduDecoderModule()
        {
        }

        std::vector<ModuleDataType> EmwinCaduDecoderModule::getInputTypes()
        {
            return {DATA_FILE};
        }

        std::vector<ModuleDataType> EmwinCaduDecoderModule::getOutputTypes()
        {
            return {DATA_FILE};
        }

        void EmwinCaduDecoderModule::process()
        {
            if (input_data_type == DATA_FILE)
            {
                filesize = getFilesize(d_input_file);
                data_in = std::ifstream(d_input_file, std::ios::binary);
            }
            else
            {
                filesize = 0;
            }

            if (d_output_file_hint.empty())
            {
                logger->error("No output file hint for CADU decoder");
                return;
            }

            cadu_out = std::ofstream(d_output_file_hint + ".cadu", std::ios::binary);
            if (!cadu_out.is_open())
            {
                logger->error("Failed to open output CADU file");
                return;
            }
            d_output_files.push_back(d_output_file_hint + ".cadu");

            logger->info("LoRa soft in : " + d_input_file);
            logger->info("CADU out     : " + d_output_file_hint + ".cadu");

            const int bits_per_sym = sf;
            if (bits_per_sym <= 0 || bits_per_sym > 12)
            {
                logger->error("Invalid SF");
                cadu_out.close();
                return;
            }

            const int syms_per_read = 64;
            const int soft_read_size = bits_per_sym * syms_per_read;
            std::vector<int8_t> soft((size_t)soft_read_size, 0);

            std::vector<uint8_t> cadu_bytes;
            cadu_bytes.reserve(8192);

            uint8_t cur_byte = 0;
            int bit_idx = 0;

            time_t lastTime = 0;

            while (input_data_type == DATA_FILE ? !data_in.eof() : input_active.load())
            {
                int syms = 0;

                if (input_data_type == DATA_FILE)
                {
                    data_in.read((char *)soft.data(), (std::streamsize)soft.size());
                    std::streamsize got = data_in.gcount();
                    if (got <= 0)
                        break;
                    syms = (int)(got / bits_per_sym);
                }
                else
                {
                    if (!input_fifo)
                        break;
                    input_fifo->read((uint8_t *)soft.data(), soft_read_size);
                    syms = syms_per_read;
                }

                for (int s = 0; s < syms; s++)
                {
                    int val = 0;
                    for (int b = 0; b < bits_per_sym; b++)
                    {
                        int8_t v = soft[(size_t)s * bits_per_sym + b];
                        val = (val << 1) | (v > 0 ? 1 : 0);
                    }

                    total_syms++;

                    for (int b = bits_per_sym - 1; b >= 0; b--)
                    {
                        int bit = (val >> b) & 1;
                        cur_byte = (uint8_t)((cur_byte << 1) | (bit & 1));
                        bit_idx++;
                        total_bits++;

                        if (bit_idx == 8)
                        {
                            cadu_bytes.push_back(cur_byte);
                            cur_byte = 0;
                            bit_idx = 0;

                            if (cadu_bytes.size() >= 1024)
                            {
                                cadu_out.write((char *)cadu_bytes.data(), (std::streamsize)cadu_bytes.size());
                                total_cadu_bytes += (int)cadu_bytes.size();
                                cadu_bytes.clear();
                            }
                        }
                    }
                }

                if (input_data_type == DATA_FILE && data_in)
                    progress = (uint64_t)(std::streamoff)data_in.tellg();

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

            if (!cadu_bytes.empty())
            {
                cadu_out.write((char *)cadu_bytes.data(), (std::streamsize)cadu_bytes.size());
                total_cadu_bytes += (int)cadu_bytes.size();
                cadu_bytes.clear();
            }

            cadu_out.close();

            if (input_data_type == DATA_FILE && data_in.is_open())
                data_in.close();
        }

        void EmwinCaduDecoderModule::drawUI(bool window)
        {
            ImGui::Begin("FY-4A EMWIN CADU Decoder", NULL, window ? 0 : NOWINDOW_FLAGS);

            ImGui::Text("SF: %d", sf);
            ImGui::Text("Total symbols: %d", total_syms);
            ImGui::Text("Total bits: %d", total_bits);
            ImGui::Text("CADU bytes: %d", total_cadu_bytes);

            if (input_data_type == DATA_FILE && filesize > 0)
                ImGui::ProgressBar((double)progress / (double)filesize, ImVec2(ImGui::GetContentRegionAvail().x, 20 * ui_scale));

            drawStopButton();
            ImGui::End();
        }

        std::string EmwinCaduDecoderModule::getID()
        {
            return "emwin_cadu_decoder";
        }

        std::vector<std::string> EmwinCaduDecoderModule::getParameters()
        {
            return {"sf"};
        }

        std::shared_ptr<ProcessingModule> EmwinCaduDecoderModule::getInstance(std::string input_file, std::string output_file_hint, nlohmann::json parameters)
        {
            return std::make_shared<EmwinCaduDecoderModule>(input_file, output_file_hint, parameters);
        }
    }
}