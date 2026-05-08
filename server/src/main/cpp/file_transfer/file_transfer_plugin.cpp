//
// Created RGAA on 15/11/2024.
//

#include "file_transfer_plugin.h"
#include "tc_message.pb.h"
#include "tc_common_new/log.h"
#include "render/plugins/plugin_ids.h"
#include "render/plugin_interface/gr_net_plugin.h"
#include "file_transmission_server/file_transmit_msg_interface.h"

extern "C" {
    __declspec(dllimport) uint64_t GenNextGlobalId();
}

void* GetInstance() {
    static tc::FileTransferPlugin plugin;
    return (void*)&plugin;
}

namespace tc
{

    std::string FileTransferPlugin::GetPluginId() {
        return kNetFileTransferPluginId;
    }

    std::string FileTransferPlugin::GetPluginName() {
        return "File Transfer";
    }

    std::string FileTransferPlugin::GetVersionName() {
        return "1.0.2";
    }

    uint32_t FileTransferPlugin::GetVersionCode() {
        return 102;
    }

    std::string FileTransferPlugin::GetPluginDescription() {
        return "Full featured file transferring";
    }

    void FileTransferPlugin::OnMessage(std::shared_ptr<Message> msg) {
        //LOGI("OnMessage, file transfer enabled: {}", sys_settings_.file_transfer_enabled_);
        if (!file_trans_msg_interface_ || !sys_settings_.file_transfer_enabled_) {
            return;
        }
        file_trans_msg_interface_->OnMessage(msg);
    }

    bool FileTransferPlugin::OnCreate(const GrPluginParam& param) {
        if (!GrPluginInterface::OnCreate(param)) {
            return false;
        }
        file_trans_msg_interface_ = FileTransmitMsgInterface::Make(this);
        file_trans_msg_interface_->RegisterFileTransmitCallback();

        // translator
        LOGI("Init language: {}", (int)GetCurrentLanguage());
        tcTrMgr()->InitLanguage(GetCurrentLanguage());

        // test //
        for (int i = 0; i < 10; i++) {
            LOGI("GlobalId: {}", ::GenNextGlobalId());
        }
        // test //

        return true;
    }

    void FileTransferPlugin::OnSyncPluginSettingsInfo(const GrPluginSettingsInfo& settings) {
        GrPluginInterface::OnSyncPluginSettingsInfo(settings);
        //LOGI("Max transmit speed: {}, Max receive speed: {}", settings.max_transmit_speed_, settings.max_receive_speed_);
        if (!file_trans_msg_interface_) {
            return;
        }
        if (settings.max_transmit_speed_ != file_trans_msg_interface_->GetMaxSpeedBybitPerSecond()) {
            file_trans_msg_interface_->SetMaxSpeedBybitPerSecond(settings.max_transmit_speed_);
        }
    }

    LanguageKind FileTransferPlugin::GetCurrentLanguage() {
        return (LanguageKind)sys_settings_.language_;
    }

}
