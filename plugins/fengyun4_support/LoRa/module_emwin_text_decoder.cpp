#include "module_emwin_text_decoder.h"
#include "logger.h"
#include "imgui/imgui.h"
#include <filesystem>
#include <cstring>
#include <ctime>
#include <cstdio>
#include <cmath>

namespace
{
    std::string fmt_time(double t)
    {
        time_t tv = (time_t)t;
        std::tm *tm_v = gmtime(&tv);
        char buf[64] = {0};
        if (tm_v)
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm_v);
        else
            snprintf(buf, sizeof(buf), "unknown");
        return std::string(buf);
    }
}

namespace fengyun4
{
    namespace emwin
    {
        EmwinTextDecoderModule::EmwinTextDecoderModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters)
            : ProcessingModule(input_file, output_file_hint, parameters)
        {
            if (parameters.count("sf") > 0)
                sf = parameters["sf"].get<int>();
            if (sf < 7 || sf > 12)
                sf = 9;
        }

        EmwinTextDecoderModule::~EmwinTextDecoderModule()
        {
        }

        std::vector<ModuleDataType> EmwinTextDecoderModule::getInputTypes()
        {
            return {DATA_FILE};
        }

        std::vector<ModuleDataType> EmwinTextDecoderModule::getOutputTypes()
        {
            return {DATA_FILE};
        }

        void EmwinTextDecoderModule::process_final_text(const std::string &text, double timestamp)
        {
            if (text.empty())
                return;

            if (!d_output_file_hint.empty())
            {
                std::string base_dir = d_output_file_hint;
                size_t slash = base_dir.find_last_of("/\\");
                if (slash != std::string::npos)
                    base_dir = base_dir.substr(0, slash);
                if (base_dir.empty())
                    base_dir = ".";
                base_dir += "/EMWIN";

                std::error_code ec;
                std::filesystem::create_directories(base_dir, ec);

                time_t time_v = (time_t)timestamp;
                std::tm *tm_v = gmtime(&time_v);
                char name[160] = {0};
                if (tm_v)
                    snprintf(name, sizeof(name), "%04d%02d%02dT%02d%02d%02dZ_%04d.txt",
                             tm_v->tm_year + 1900, tm_v->tm_mon + 1, tm_v->tm_mday,
                             tm_v->tm_hour, tm_v->tm_min, tm_v->tm_sec,
                             file_seq % 10000);
                else
                    snprintf(name, sizeof(name), "unknown_%04d.txt", file_seq % 10000);
                file_seq++;

                std::string full_path = base_dir + "/" + name;
                std::ofstream outf(full_path, std::ios::binary | std::ios::app);
                if (outf)
                {
                    outf << text;
                    if (!text.empty() && text.back() != '\n')
                        outf << "\n";
                    outf.close();
                    d_output_files.push_back(full_path);
                }
            }

            logger->info("EMWIN text segment saved (%d bytes)", (int)text.size());

            if (is_gui)
            {
                std::lock_guard<std::mutex> lk(msg_mtx);
                nlohmann::json j;
                j["timestamp"] = timestamp;
                j["message"] = text;
                msg_history.push_back(j);
                if (msg_history.size() > 200)
                    msg_history.erase(msg_history.begin());
                last_msg_count = (int)msg_history.size();
            }
        }

        void EmwinTextDecoderModule::process()
        {
            if (input_data_type == DATA_FILE)
            {
                std::ifstream tmp(d_input_file, std::ios::binary | std::ios::ate);
                if (tmp.is_open())
                {
                    filesize = (uint64_t)tmp.tellg();
                    tmp.close();
                }
                else
                {
                    filesize = 0;
                }
                data_in = std::ifstream(d_input_file, std::ios::binary);
            }
            else
            {
                filesize = 0;
            }

            logger->info("Using input cadu " + d_input_file);

            double base_time = (double)time(NULL);
            std::string text_accum;
            text_accum.reserve(4096);

            const int buf_size = 8192;
            std::vector<uint8_t> rd((size_t)buf_size, 0);

            time_t lastTime = 0;

            while (input_data_type == DATA_FILE ? !data_in.eof() : input_active.load())
            {
                int n = 0;

                if (input_data_type == DATA_FILE)
                {
                    data_in.read((char *)rd.data(), (std::streamsize)rd.size());
                    std::streamsize got = data_in.gcount();
                    if (got <= 0)
                        break;
                    n = (int)got;
                }
                else
                {
                    if (!input_fifo)
                        break;
                    input_fifo->read((uint8_t *)rd.data(), buf_size);
                    if (!input_active.load())
                        break;
                    n = buf_size;
                }

                for (int i = 0; i < n; i++)
                {
                    uint8_t v = rd[(size_t)i];
                    bool printable = (v >= 0x20 && v <= 0x7E) || v == '\n' || v == '\r' || v == '\t';

                    if (printable)
                    {
                        text_accum.push_back((char)v);
                        if (text_accum.size() > 4096)
                        {
                            process_final_text(text_accum, base_time);
                            text_accum.clear();
                        }
                    }
                    else
                    {
                        if (text_accum.size() > 8)
                            process_final_text(text_accum, base_time);
                        text_accum.clear();
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

            if (!text_accum.empty())
            {
                process_final_text(text_accum, base_time);
                text_accum.clear();
            }

            if (input_data_type == DATA_FILE && data_in.is_open())
                data_in.close();
        }

        void EmwinTextDecoderModule::drawUI(bool window)
        {
            is_gui = true;

            ImGuiIO &io = ImGui::GetIO();

            ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.6f), ImGuiCond_FirstUseEver);
            bool main_open = ImGui::Begin("FY-4A EMWIN Text Decoder", NULL, (window ? 0 : NOWINDOW_FLAGS) | ImGuiWindowFlags_NoCollapse);

            if (main_open)
            {
                ImGui::Text("SF: %d", sf);
                ImGui::Text("Last message count: %d", last_msg_count);

                if (input_data_type == DATA_FILE && filesize > 0)
                    ImGui::ProgressBar((double)progress / (double)filesize, ImVec2(ImGui::GetContentRegionAvail().x, 20 * ui_scale));

                ImGui::Text("Decoded text is shown in the floating window.");
            }
            ImGui::End();

            ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.6f), ImGuiCond_FirstUseEver);
            bool msg_open = ImGui::Begin("EMWIN Messages", NULL, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoCollapse);

            if (msg_open)
            {
                std::lock_guard<std::mutex> lk(msg_mtx);

                float wrap_pos = ImGui::GetContentRegionMax().x;
                ImGui::BeginTable("##emwinmsgtable", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit);
                ImGui::TableSetupColumn("Timestamp", ImGuiTableColumnFlags_NoResize, 160 * ui_scale);
                ImGui::TableSetupColumn("Message", 0, -1);
                ImGui::TableHeadersRow();

                for (int i = (int)msg_history.size() - 1; i >= 0; i--)
                {
                    try
                    {
                        auto &msg = msg_history[(size_t)i];
                        double ts = msg["timestamp"].get<double>();
                        std::string text = msg["message"].get<std::string>();

                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextColored(style::theme.yellow, "%s", fmt_time(ts).c_str());
                        ImGui::TableSetColumnIndex(1);
                        ImGui::PushTextWrapPos(wrap_pos);
                        ImGui::TextColored(style::theme.green, "%s", text.c_str());
                        ImGui::PopTextWrapPos();
                    }
                    catch (std::exception &)
                    {
                    }
                }

                ImGui::EndTable();
            }
            ImGui::End();
        }

        std::string EmwinTextDecoderModule::getID()
        {
            return "emwin_text_decoder";
        }

        std::vector<std::string> EmwinTextDecoderModule::getParameters()
        {
            return {"sf"};
        }

        std::shared_ptr<ProcessingModule> EmwinTextDecoderModule::getInstance(std::string input_file, std::string output_file_hint, nlohmann::json parameters)
        {
            return std::make_shared<EmwinTextDecoderModule>(input_file, output_file_hint, parameters);
        }
    }
}