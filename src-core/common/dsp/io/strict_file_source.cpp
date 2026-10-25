#include "strict_file_source.h"
#include "common/wav.h"
#include "logger.h"
#include <volk/volk.h>
#include <cstring>

namespace dsp
{
    StrictFileSource::StrictFileSource() : Block<complex_t, complex_t>(nullptr) 
    {
        // 初始化输出流，下游模块直接接这里
        output_stream = std::make_shared<dsp::stream<complex_t>>();
        is_running = false;
        filesize = 0;
        progress = 0;
    }

    StrictFileSource::~StrictFileSource()
    {
        stop();
    }

    bool StrictFileSource::open(const std::string& file_path, const std::string& format_str)
    {
        input_file.open(file_path, std::ios::binary);
        if (!input_file.is_open()) return false;

        input_file.seekg(0, std::ios::end);
        filesize = input_file.tellg();
        input_file.seekg(0, std::ios::beg);

        if (format_str == "cf32") format = S_CF32;
        else if (format_str == "cs32") format = S_CS32;
        else if (format_str == "cs16") format = S_CS16;
        else if (format_str == "cs8") format = S_CS8;
        else if (format_str == "cu8") format = S_CU8;
        else if (format_str == "wav") format = S_WAV_16;
        else return false;

        if (format == S_WAV_16)
        {
            wav::WavHeader hdr = wav::parseHeaderFromFileWav(file_path);
            if (!wav::isValidWav(hdr)) return false;
            input_file.seekg(sizeof(wav::WavHeader));
        }

        buffer_s32.resize(65536 * 2);
        buffer_s16.resize(65536 * 2);
        buffer_s8.resize(65536 * 2);
        buffer_u8.resize(65536 * 2);

        return true;
    }

    void StrictFileSource::start()
    {
        if (is_running) return;
        is_running = true;
        worker_thread = std::thread(&StrictFileSource::work, this);
    }

    void StrictFileSource::stop()
    {
        is_running = false;
        if (worker_thread.joinable())
            worker_thread.join();
        
        output_stream->stopWriter();
    }

    void StrictFileSource::work()
    {
        int buffer_size = 65536;
        
        while (is_running && input_file.is_open() && !input_file.eof())
        {
            size_t bytes_to_read = 0;
            switch (format) {
                case S_CF32: bytes_to_read = buffer_size * sizeof(complex_t); break;
                case S_CS32: bytes_to_read = buffer_size * sizeof(int32_t) * 2; break;
                case S_CS16: case S_WAV_16: bytes_to_read = buffer_size * sizeof(int16_t) * 2; break;
                case S_CS8: bytes_to_read = buffer_size * sizeof(int8_t) * 2; break;
                case S_CU8: bytes_to_read = buffer_size * sizeof(uint8_t) * 2; break;
            }

            if (format == S_CF32) input_file.read((char*)output_stream->writeBuf, bytes_to_read);
            else if (format == S_CS32) input_file.read((char*)buffer_s32.data(), bytes_to_read);
            else if (format == S_CS16 || format == S_WAV_16) input_file.read((char*)buffer_s16.data(), bytes_to_read);
            else if (format == S_CS8) input_file.read((char*)buffer_s8.data(), bytes_to_read);
            else if (format == S_CU8) input_file.read((char*)buffer_u8.data(), bytes_to_read);

            size_t actual_bytes = input_file.gcount();
            if (actual_bytes == 0) break;

            int actual_samples = 0;
            switch (format) {
                case S_CF32: actual_samples = actual_bytes / sizeof(complex_t); break;
                case S_CS32: actual_samples = actual_bytes / (sizeof(int32_t) * 2); break;
                case S_CS16: case S_WAV_16: actual_samples = actual_bytes / (sizeof(int16_t) * 2); break;
                case S_CS8: actual_samples = actual_bytes / (sizeof(int8_t) * 2); break;
                case S_CU8: actual_samples = actual_bytes / (sizeof(uint8_t) * 2); break;
            }

            // 转换数据到 output_stream->writeBuf
            switch (format) {
                case S_CS32:
                    volk_32i_s32f_convert_32f_u((float *)output_stream->writeBuf, (const int32_t*)buffer_s32.data(), 2147483647, actual_samples * 2);
                    break;
                case S_CS16: case S_WAV_16:
                    volk_16i_s32f_convert_32f_u((float *)output_stream->writeBuf, (const int16_t*)buffer_s16.data(), 32767, actual_samples * 2);
                    break;
                case S_CS8:
                    volk_8i_s32f_convert_32f_u((float *)output_stream->writeBuf, (const int8_t *)buffer_s8.data(), 127, actual_samples * 2);
                    break;
                case S_CU8:
                    for (int i = 0; i < actual_samples; i++) {
                        float imag = (buffer_u8[i * 2 + 1] - 127) * (1.0 / 127.0);
                        float real = (buffer_u8[i * 2 + 0] - 127) * (1.0 / 127.0);
                        output_stream->writeBuf[i] = complex_t(real, imag);
                    }
                    break;
            }

            // 将真实有效的数据 Swap 给下游
            output_stream->swap(actual_samples);
            progress = input_file.tellg();
        }
        
        logger->info("StrictFileSource: Finished reading file.");
        output_stream->stopWriter(); // 通知下游结束
    }
}