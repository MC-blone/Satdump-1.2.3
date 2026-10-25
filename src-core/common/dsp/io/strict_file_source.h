#pragma once

#include "common/dsp/block.h"
#include "common/dsp/complex.h"
#include <fstream>
#include <string>
#include <thread>
#include <atomic>
#include <vector>

namespace dsp
{
    enum StrictBasebandType
    {
        S_CF32,
        S_CS32,
        S_CS16,
        S_CS8,
        S_CU8,
        S_WAV_16
    };

    class StrictFileSource : public Block<complex_t, complex_t>
    {
    private:
        std::ifstream input_file;
        StrictBasebandType format;
        uint64_t filesize;
        uint64_t progress;

        std::vector<int32_t> buffer_s32;
        std::vector<int16_t> buffer_s16;
        std::vector<int8_t> buffer_s8;
        std::vector<uint8_t> buffer_u8;

        std::atomic<bool> is_running;
        std::thread worker_thread;

        void work();

    public:
        StrictFileSource();
        ~StrictFileSource();

        // 打开文件并配置格式
        bool open(const std::string& file_path, const std::string& format_str);
        
        // 启动/停止读取线程
        void start();
        void stop();

        // 获取当前进度
        uint64_t get_progress() { return progress; }
        uint64_t get_filesize() { return filesize; }
    };
}