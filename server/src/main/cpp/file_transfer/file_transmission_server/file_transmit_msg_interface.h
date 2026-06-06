#pragma once
#include <string>
#include <memory>
#include "mirror_message.pb.h"

namespace tc {

class FileTransferPlugin;
class FileTransmitManager;

class FileTransmitMsgInterface {
public:
	static std::shared_ptr<FileTransmitMsgInterface> Make(FileTransferPlugin* file_trans_plugin);
	FileTransmitMsgInterface(FileTransferPlugin* file_trans_plugin);
	~FileTransmitMsgInterface();

	void OnMessage(const std::shared_ptr<tc::Message>& msg);

	void OnConnectionLost();

	void RegisterFileTransmitCallback();

	uint64_t GetMaxSpeedBybitPerSecond();
	void SetMaxSpeedBybitPerSecond(uint64_t speed);
private:
    bool IsFileTransferEnabled();

private:
	FileTransferPlugin* file_trans_plugin_ = nullptr;
	std::shared_ptr<FileTransmitManager> file_trans_manager_ = nullptr;
};


}