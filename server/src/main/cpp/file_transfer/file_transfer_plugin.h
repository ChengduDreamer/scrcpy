#pragma once

#include <memory>
#include <string>

#ifdef _WIN32
    #define PLUGIN_API __declspec(dllexport)
#else
    #define PLUGIN_API __attribute__((visibility("default")))
#endif
//#include "plugin_interface/gr_plugin_interface.h"
//#include "translator/yk_translator.h"

namespace tc
{

    class FileTransferPluginSettings {
    public:
        int language_ = 1;
        uint64_t max_transmit_speed_ = 0;
        uint64_t max_receive_speed_ = 0;
        std::string serial_;        // adb serial
        std::string device_name_;   // 设备昵称
    };

    class Message;
    class FileTransmitMsgInterface;

    class FileTransferPlugin {
    public:
//        std::string GetPluginId() override;
//        std::string GetPluginName() override;
//        std::string GetVersionName() override;
//        uint32_t GetVersionCode() override;
//        std::string GetPluginDescription() override;

//        bool OnCreate(const GrPluginParam& param);

        bool Create();
        void OnMessage(std::shared_ptr<Message> msg);
        void SendProtoMessage(std::string stream_id, std::shared_ptr<Message> msg); 
        void OnSyncPluginSettingsInfo(const FileTransferPluginSettings& settings);
        //LanguageKind GetCurrentLanguage();

    private:
        std::shared_ptr<FileTransmitMsgInterface> file_trans_msg_interface_ = nullptr;
    };

}

extern "C" PLUGIN_API void* GetInstance();


