//
//

#ifndef GAMMARAY_RTC_PLUGIN_H
#define GAMMARAY_RTC_PLUGIN_H
#include <memory>
//#include "plugin_interface/gr_plugin_interface.h"
//#include "translator/yk_translator.h"

namespace tc
{

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
        void OnSyncPluginSettingsInfo(const GrPluginSettingsInfo& settings);
        //LanguageKind GetCurrentLanguage();

    private:
        std::shared_ptr<FileTransmitMsgInterface> file_trans_msg_interface_ = nullptr;
    };

}

extern "C" __declspec(dllexport) void* GetInstance();

#endif //GAMMARAY_UDP_PLUGIN_H
