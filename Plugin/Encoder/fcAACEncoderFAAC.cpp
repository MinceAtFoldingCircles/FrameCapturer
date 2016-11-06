﻿#include "pch.h"
#include "fcFoundation.h"
#include "fcMP4Internal.h"
#include "fcAACEncoder.h"

#ifdef fcSupportAAC_FAAC

#include <libfaac/faac.h>
#ifdef fcWindows
    #ifdef _M_AMD64
        #define FAACDLL "libfaac-win64.dll"
    #elif _M_IX86
        #define FAACDLL "libfaac-win32.dll"
    #endif
#else
    #define FAACDLL "libfaac" fcDLLExt
#endif

class fcAACEncoderFAAC : public fcIAACEncoder
{
public:
    fcAACEncoderFAAC(const fcAACEncoderConfig& conf);
    ~fcAACEncoderFAAC() override;
    const char* getEncoderInfo() override;
    const Buffer& getDecoderSpecificInfo() override;
    bool encode(fcAACFrame& dst, const float *samples, size_t num_samples, fcTime timestamp) override;

private:
    fcAACEncoderConfig m_conf;
    void *m_handle;
    unsigned long m_num_read_samples;
    unsigned long m_output_size;
    Buffer m_aac_tmp_buf;
    Buffer m_aac_header;
    RawVector<float> m_tmp_data;
};


namespace {

    typedef faacEncConfigurationPtr
        (FAACAPI* faacEncGetCurrentConfiguration_t)(faacEncHandle hEncoder);


    typedef int(FAACAPI* faacEncSetConfiguration_t)(faacEncHandle hEncoder,
        faacEncConfigurationPtr config);


    typedef faacEncHandle(FAACAPI* faacEncOpen_t)(unsigned long sampleRate,
        unsigned int numChannels,
        unsigned long *inputSamples,
        unsigned long *maxOutputBytes);


    typedef int(FAACAPI* faacEncGetDecoderSpecificInfo_t)(faacEncHandle hEncoder, unsigned char **ppBuffer,
        unsigned long *pSizeOfDecoderSpecificInfo);


    typedef int(FAACAPI* faacEncEncode_t)(faacEncHandle hEncoder, int32_t * inputBuffer, unsigned int samplesInput,
        unsigned char *outputBuffer,
        unsigned int bufferSize);

    typedef int(FAACAPI* faacEncClose_t)(faacEncHandle hEncoder);

#define EachFAACFunctions(Body)\
    Body(faacEncGetCurrentConfiguration)\
    Body(faacEncSetConfiguration)\
    Body(faacEncOpen)\
    Body(faacEncGetDecoderSpecificInfo)\
    Body(faacEncEncode)\
    Body(faacEncClose)


#define decl(name) name##_t name##_i;
    EachFAACFunctions(decl)
#undef decl

module_t g_mod_faac;

} // namespace

fcAACEncoderFAAC::fcAACEncoderFAAC(const fcAACEncoderConfig& conf)
    : m_conf(conf), m_handle(nullptr), m_num_read_samples(), m_output_size()
{
    m_handle = faacEncOpen_i(conf.sample_rate, conf.num_channels, &m_num_read_samples, &m_output_size);

    faacEncConfigurationPtr config = faacEncGetCurrentConfiguration_i(m_handle);
    config->bitRate = conf.target_bitrate / conf.num_channels;
    config->quantqual = 100;
    config->inputFormat = FAAC_INPUT_FLOAT;
    config->mpegVersion = MPEG4;
    config->aacObjectType = LOW;
    config->allowMidside = 0;
    config->useLfe = 0;
    config->outputFormat = 1;
    faacEncSetConfiguration_i(m_handle, config);
}

fcAACEncoderFAAC::~fcAACEncoderFAAC()
{
    faacEncClose_i(m_handle);
    m_handle = nullptr;
}
const char* fcAACEncoderFAAC::getEncoderInfo() { return "FAAC"; }

bool fcAACEncoderFAAC::encode(fcAACFrame& dst, const float *samples, size_t num_samples, fcTime timestamp)
{
    m_aac_tmp_buf.resize(m_output_size);

    m_tmp_data.assign(samples, num_samples);
    for (auto& v : m_tmp_data) { v *= 32767.0f; }
    samples = m_tmp_data.data();

    int total = 0;
    for (;;) {
        int process_size = std::min<int>((int)m_num_read_samples, (int)num_samples - total);
        int packet_size = faacEncEncode_i(m_handle, (int32_t*)samples, process_size, (unsigned char*)&m_aac_tmp_buf[0], m_output_size);
        if (packet_size > 0) {
            dst.data.append(m_aac_tmp_buf.data(), packet_size);

            double duration = (double)process_size / (double)m_conf.sample_rate;
            double timepos = (double)total / (double)m_conf.sample_rate + timestamp;
            dst.packets.push_back({ (uint32_t)packet_size, duration, timepos });
        }
        total += process_size;
        samples += process_size;
        if (total >= num_samples) { break; }
    }
    return true;
}

const Buffer& fcAACEncoderFAAC::getDecoderSpecificInfo()
{
    if (m_aac_header.empty()) {
        unsigned char *buf;
        unsigned long num_buf;
        faacEncGetDecoderSpecificInfo_i(m_handle, &buf, &num_buf);
        m_aac_header.append((char*)buf, num_buf);
        //free(buf);
    }
    return m_aac_header;
}


bool fcLoadFAACModule()
{
    if (g_mod_faac != nullptr) { return true; }

    g_mod_faac = DLLLoad(FAACDLL);
    if (g_mod_faac == nullptr) { return false; }

#define imp(name) (void*&)name##_i = DLLGetSymbol(g_mod_faac, #name);
    EachFAACFunctions(imp)
#undef imp
    return true;
}


fcIAACEncoder* fcCreateAACEncoderFAAC(const fcAACEncoderConfig& conf)
{
    if (!fcLoadFAACModule()) { return nullptr; }
    return new fcAACEncoderFAAC(conf);
}

#ifdef fcEnableFAACSelfBuild
namespace {
    std::thread *g_download_thread;

    std::string fcGetFAACModulePath()
    {
        std::string ret = !fcMP4GetModulePath().empty() ? fcMP4GetModulePath() : DLLGetDirectoryOfCurrentModule();
        if (!ret.empty() && (ret.back() != '/' && ret.back() != '\\')) {
            ret += "/";
        }
        ret += FAACDLL;
        return ret;
    }

    void fcDownloadFAACBody(fcDownloadCallback cb)
    {
        namespace fs = std::experimental::filesystem;

#ifdef fcWindows
        std::string dir = !fcMP4GetModulePath().empty() ? fcMP4GetModulePath() : DLLGetDirectoryOfCurrentModule();
        std::string package_path = fcMP4GetFAACPackagePath();
        bool http = false;

        // download self-build package package_path is http
        if (strncmp(package_path.c_str(), "http://", 7) == 0) {
            http = true;
            std::string local_package = dir + "/FAAC_SelfBuild.zip";
            std::fstream fs(local_package.c_str(), std::ios::out | std::ios::binary);
            bool succeeded = HTTPGet(package_path.c_str(), [&](const char* data, size_t size) {
                fs.write(data, size);
                return true;
            });
            fs.close();
            if (succeeded) {
                cb(fcDownloadState_InProgress, "succeeded to download package");
            }
            else {
                cb(fcDownloadState_Error, "failed to download package");
                return;
            }
            package_path = local_package;
        }

        bool ret = false;

        // unzip package
        if (Unzip(dir.c_str(), package_path.c_str()) > 0)
        {
            cb(fcDownloadState_InProgress, "succeeded to unzip package");

            // buld
#ifdef _M_IX86
            std::string build_command = std::string("cmd.exe /C ") + dir + "\\FAAC_SelfBuild\\build_x86.bat";
            std::string dst_path = dir + "/FAAC_SelfBuild/out_x86/libfaac.dll";
#elif _M_X64
            std::string build_command = std::string("cmd.exe /C ") + dir + "\\FAAC_SelfBuild\\build_x86_64.bat";
            std::string dst_path = dir + "/FAAC_SelfBuild/out_x86_64/libfaac.dll";
#endif

            if (Execute(build_command.c_str()) == 0) {
                cb(fcDownloadState_InProgress, "succeeded to build");
                // copy built module
                if (fs::copy_file(dst_path, fcGetFAACModulePath())) {
                    cb(fcDownloadState_Completed, "succeeded to copy module");
                    ret = true;
                }
                else {
                    cb(fcDownloadState_Error, "failed to copy module");
                }
            }
            else {
                cb(fcDownloadState_Error, "failed to build");
            }
        }
        else {
            cb(fcDownloadState_Error, "failed to unzip package");
        }

#ifdef fcMaster
        // remove self-build package and intermediate files
        fs::remove_all(dir + "/FAAC_SelfBuild");
        if (http) {
            fs::remove_all(package_path);
        }
#endif // fcMaster

#else // fcWindows
#endif // fcWindows

        g_download_thread->detach();
        delete g_download_thread;
        g_download_thread = nullptr;
    }
}

bool fcDownloadFAAC(fcDownloadCallback cb)
{
    // no need to build if DLL already exists
    if (fcLoadFAACModule()) { return true; }

    // download thread is already running
    if (g_download_thread) { return false; }

    g_download_thread = new std::thread([=]() { fcDownloadFAACBody(cb); });
    return true;
}

#endif // fcEnableFAACSelfBuild
#else  // fcSupportAAC_FAAC

bool fcLoadFAACModule() { return false; }
fcIAACEncoder* fcCreateAACEncoderFAAC(const fcAACEncoderConfig& conf) { return nullptr; }

#endif // fcSupportAAC_FAAC

#if !defined(fcEnableFAACSelfBuild) || !defined(fcSupportAAC_FAAC)

bool fcDownloadFAAC(fcDownloadCallback cb) { return false; }

#endif