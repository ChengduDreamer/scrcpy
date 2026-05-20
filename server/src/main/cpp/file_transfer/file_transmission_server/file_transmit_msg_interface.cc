#include "file_transmit_msg_interface.h"
//#include <qstring.h>
#include <string>
//#include "tc_label.h"
//#include "tc_common_new/log.h"
#include "cpp_base_lib/yk_logger.h"
//#include "tc_common_new/time_util.h"
#include "cpp_base_lib/time_util.h"
#include "file_transfer_plugin.h"
#include "file_transmit_manager.h"
#include "file_transmit_impl.h"
//#include "render/plugin_interface/gr_net_plugin.h"
//#include "render/plugin_interface/gr_plugin_events.h"
//#include "tc_message_new/proto_converter.h"
#include "yk_translator.h"

namespace tc {

std::shared_ptr<FileTransmitMsgInterface> FileTransmitMsgInterface::Make(FileTransferPlugin* file_trans_plugin) {
	return std::make_shared<FileTransmitMsgInterface>(file_trans_plugin);
}

FileTransmitMsgInterface::FileTransmitMsgInterface(FileTransferPlugin* file_trans_plugin) : file_trans_plugin_(file_trans_plugin) {
	file_trans_manager_ = std::make_shared<FileTransmitManager>();
}

FileTransmitMsgInterface::~FileTransmitMsgInterface() {

}

void FileTransmitMsgInterface::OnMessage(const std::shared_ptr<tc::Message>& message) {
	if (!file_trans_manager_ || !IsFileTransferEnabled()) {
		return;
	}
	//auto type = message->type();
	//auto stream_id = message->stream_id();
    auto device_id = message->device_id();
	switch (message->type())
	{
	case MessageType::kFileOperationEvent: {
		file_trans_manager_->HandleFileOperateMsg(message);
		break;
	}
	case MessageType::kFileTransDataPacket: {
		file_trans_manager_->HandleFileTransmitMessage(message);
		break;
	}
	case MessageType::kFileTransSaveFileException: {
		file_trans_manager_->HandleSaveFileExceptionMessage(message);
		break;
	}
	case MessageType::kFileTransDataPacketResponse: {
		file_trans_manager_->HandleFileTransDataPacketResponseMessage(message);
		break;
	}
	default:
		break;
	}
}

void FileTransmitMsgInterface::RegisterFileTransmitCallback() {
	file_trans_manager_->RegGetFileListCallback([=, this](
		const std::string& stream_id, int resp_seq, bool ret, std::vector<tc::FileDescInfo> file_infos, std::string error_msg, std::string target_path, std::string file_permission_path
	) {
		auto message = std::make_shared<tc::Message>();
		message->set_type(tc::kFileOperateRespGetFileList);
		message->set_file_operate_resp_sequence(resp_seq);
		if (ret) {
			message->set_file_operate_resp_code(tc::RespCode::kRespCodeOk);
			//message->set_resp_message(QStringLiteral("获取文件列表成功").toStdString());
			message->set_file_operate_resp_message( (ykTr("id_file_trans_get_fil_list") + ykTr("id_file_trans_success")) );
		}
		else {
			message->set_file_operate_resp_code(tc::RespCode::kRespCodeError);
			//message->set_resp_message(QStringLiteral("获取文件列表失败").toStdString());
			message->set_file_operate_resp_message((ykTr("id_file_trans_get_fil_list") + ykTr("id_file_trans_log_failed")));
		}
		//message->set_resp_message(QStringLiteral("获取文件列表").toStdString());
		message->set_file_operate_resp_message(ykTr("id_file_trans_get_fil_list"));
		tc::FileOperateRespGetFileList* file_list = new tc::FileOperateRespGetFileList();
		file_list->set_path(target_path);
		file_list->set_ret(ret);
		file_list->set_msg_of_error(error_msg);
		file_list->set_file_permission_path(file_permission_path);
		for (auto file_info : file_infos) {
			auto item = file_list->add_file_infos();
			item->set_name(file_info.name());
			item->set_path(file_info.path());
			item->set_type(file_info.type());
			item->set_date(file_info.date());
			item->set_size(file_info.size());
		}
		// to do  message 消息本身还需要设置stream——id吗
		message->set_allocated_file_operate_resp_get_file_list(file_list);
        // auto buffer = ProtoAsData(message);
		// to do 这里最好加个返回值判断
        if (IsFileTransferEnabled()) {
            // file_trans_plugin_->DispatchTargetFileTransferMessage(stream_id, buffer);

			file_trans_plugin_->SendProtoMessage(stream_id, message);
        }
		// "GetFileList kFileOperateRespGetFileList post error."	
	});

	file_trans_manager_->RegBatchCreateFoldersCallback([=, this](const std::string& stream_id, int resp_seq, std::vector<std::string> error_paths, std::string er_msg) {
		auto message = std::make_shared<tc::Message>();
		message->set_type(tc::kFileOperateRespBatchCreateFolders);
		message->set_file_operate_resp_sequence(resp_seq);
		message->set_file_operate_resp_code(tc::RespCode::kRespCodeOk);
		//message->set_resp_message(QStringLiteral("文件夹创建成功").toStdString());
		message->set_file_operate_resp_message(ykTr("id_file_trans_create_folder_success"));
		if (error_paths.size() > 0) {
			message->set_file_operate_resp_code(tc::RespCode::kRespCodeError);
			//message->set_resp_message(QStringLiteral("文件夹创建失败").toStdString());
			message->set_file_operate_resp_message(ykTr("id_file_trans_create_folder_failed"));
		}
		tc::FileOperateRespBatchCreateFolders* resp = new tc::FileOperateRespBatchCreateFolders();
		resp->set_msg_of_error(er_msg);
		for (auto path : error_paths) {
			resp->add_paths_of_no_create_folder(path);
		}
		message->set_allocated_file_operate_resp_batch_create_folders(resp);
        // auto buffer = ProtoAsData(message);
		// 这里最好加个返回值判断
        if (IsFileTransferEnabled()) {
            // file_trans_plugin_->DispatchTargetFileTransferMessage(stream_id, buffer);
			file_trans_plugin_->SendProtoMessage(stream_id, message);
        }
		// "GetFileList kFileOperateRespBatchCreateFolders post error."
    });

	file_trans_manager_->RegJudgeFileExistsCallback([=, this](const std::string& stream_id, int resp_seq, std::string path, bool exists, uint64_t file_size, uint64_t file_date) {
		auto message = std::make_shared<tc::Message>();
		message->set_type(tc::kFileOperateRespExists);
		message->set_file_operate_resp_sequence(resp_seq);
		message->set_file_operate_resp_code(tc::RespCode::kRespCodeOk);
		tc::FileOperateRespExists* resp = new tc::FileOperateRespExists();
		resp->set_path(path);
		resp->set_ret(exists);
		resp->set_size(file_size);
		resp->set_date(file_date);
		message->set_allocated_file_operate_resp_exists(resp);
        // auto buffer = ProtoAsData(message);
		// 这里最好加个返回值判断
        if (IsFileTransferEnabled()) {
            // file_trans_plugin_->DispatchTargetFileTransferMessage(stream_id, buffer);
			file_trans_plugin_->SendProtoMessage(stream_id, message);
        }
		//"GetFileList kFileOperateRespExists post error."
	});

	file_trans_manager_->file_transmit_impl_->upload_resp_func_ = [=, this](const std::string& stream_id, tc::FileTransRespUpload* resp_upload) {
		auto message = std::make_shared<tc::Message>();
		message->set_type(kFileTransRespUpload);
		message->set_allocated_file_trans_resp_upload(resp_upload);
        // auto buffer = ProtoAsData(message);
		// 这里最好加个返回值判断
        if (IsFileTransferEnabled()) {
            // file_trans_plugin_->DispatchTargetFileTransferMessage(stream_id, buffer);
			file_trans_plugin_->SendProtoMessage(stream_id, message);
        }
		//"GetFileList kFileTransRespUpload post error."

        // report file upload end
        // auto event = std::make_shared<GrPluginFileTransferEnd>();
        // event->the_file_id_ = resp_upload->task_id();
        // event->end_timestamp_ = (int64_t)TimeUtil::GetCurrentTimestamp();
        // event->success_ = resp_upload->res();
        // file_trans_plugin_->CallbackEvent(event);
	};


	file_trans_manager_->file_transmit_impl_->send_file_trans_data_packet_response_func_ = [=, this](const std::string& stream_id, std::shared_ptr<tc::Message> message) -> bool {
        // auto buffer = ProtoAsData(message);
		// 这里最好加个返回值判断
        if (IsFileTransferEnabled()) {
            // file_trans_plugin_->DispatchTargetFileTransferMessage(stream_id, buffer);
			file_trans_plugin_->SendProtoMessage(stream_id, message);
        }
		return true;
	};

    // 下载开始
    file_trans_manager_->file_transmit_impl_->download_begin_func_ = [=, this](const std::string& task_id, const std::string& device_id, const std::string& stream_id, const std::string& file_path) {
        // auto event = std::make_shared<GrPluginFileTransferBegin>();
        // event->the_file_id_ = task_id;
        // event->begin_timestamp_ = (int64_t)TimeUtil::GetCurrentTimestamp();
        // event->visitor_device_id_ = device_id;
        // event->direction_ = "Out";
        // event->file_detail_ = file_path;
        // file_trans_plugin_->CallbackEvent(event);
    };

    // 下载正常结束
    file_trans_manager_->file_transmit_impl_->download_end_func_ = [=, this](const std::string& task_id, const std::string& device_id, const std::string& stream_id, const std::string& file_path) {
        // report file download end
        // auto event = std::make_shared<GrPluginFileTransferEnd>();
        // event->the_file_id_ = task_id;
        // event->end_timestamp_ = (int64_t)TimeUtil::GetCurrentTimestamp();
        // event->success_ = true;
        // file_trans_plugin_->CallbackEvent(event);
    };

    // 用户取消下载
    file_trans_manager_->file_transmit_impl_->download_canceled_func_ = [=, this](const std::string& task_id, const std::string& device_id, const std::string& stream_id) {
        // report file download canceled
        //auto event = std::make_shared<GrPluginFileTransferEnd>();
        // event->the_file_id_ = task_id;
        // event->end_timestamp_ = (int64_t)TimeUtil::GetCurrentTimestamp();
        // event->success_ = false;
        // file_trans_plugin_->CallbackEvent(event);
    };

    // 下载出现错误
	file_trans_manager_->file_transmit_impl_->download_except_func_ = [=, this](const std::string& task_id, const std::string& device_id, const std::string& stream_id, tc::FileTransRespDownload* resp_download) {
		auto message = std::make_shared<tc::Message>();
		message->set_type(kFileTransRespDownload);
		message->set_allocated_file_trans_resp_download(resp_download);
        // auto buffer = ProtoAsData(message);
		// 这里最好加个返回值判断
        if (IsFileTransferEnabled()) {
            // file_trans_plugin_->DispatchTargetFileTransferMessage(stream_id, buffer);
			file_trans_plugin_->SendProtoMessage(stream_id, message);
        }
		// "GetFileList FileTransRespDownload post error."

        // report file download error
        // auto event = std::make_shared<GrPluginFileTransferEnd>();
        // event->the_file_id_ = task_id;
        // event->end_timestamp_ = (int64_t)TimeUtil::GetCurrentTimestamp();
        // event->success_ = false;
        // file_trans_plugin_->CallbackEvent(event);
	};

	file_trans_manager_->file_transmit_impl_->send_data_packet_func_ = [=, this](const std::string& stream_id, std::shared_ptr<tc::Message> message) -> bool {
        // todo: TEST !
//        int64_t queuing_msg_count = file_trans_plugin_->GetQueuingFtMsgCountInNetPlugins();
//        while (queuing_msg_count > 256) {
//            LOGW("too many queuing msgs in net plugins: {}", queuing_msg_count);
//            TimeUtil::DelayBySleep(5);
//            queuing_msg_count = file_trans_plugin_->GetQueuingFtMsgCountInNetPlugins();
//        }

        // auto buffer = ProtoAsData(message);
		// 这里最好加个返回值判断
        if (IsFileTransferEnabled()) {
            // file_trans_plugin_->DispatchTargetFileTransferMessage(stream_id, buffer);
			file_trans_plugin_->SendProtoMessage(stream_id, message);
        }
		return true;
	};

	file_trans_manager_->RegRemoveCallback([=, this](const std::string& stream_id, int resp_seq, bool ret, std::vector<std::string> er_paths, std::string er_msg) {
		auto message = std::make_shared<tc::Message>();
		message->set_type(tc::kFileOperateRespDel);
		message->set_file_operate_resp_sequence(resp_seq);
		if (ret) {
			message->set_file_operate_resp_code(tc::RespCode::kRespCodeOk);
			//message->set_resp_message(QStringLiteral("移除成功").toStdString());
			message->set_file_operate_resp_message(ykTr("id_file_trans_remove_success"));
		}
		else {
			message->set_file_operate_resp_code(tc::RespCode::kRespCodeError);
			//message->set_resp_message(QStringLiteral("移除失败").toStdString());
			message->set_file_operate_resp_message(ykTr("id_file_trans_remove_failed"));
		}
		tc::FileOperateRespDel* resp = new tc::FileOperateRespDel();
		resp->set_msg_of_error(er_msg);
		resp->set_ret(ret);
		for (auto path : er_paths) {
			YK_LOGE("no del path = {}", path);
			resp->add_paths_of_no_del(path);
		}
		message->set_allocated_file_operate_resp_del(resp);
        // auto buffer = ProtoAsData(message);
		// 这里最好加个返回值判断
        if (IsFileTransferEnabled()) {
            // file_trans_plugin_->DispatchTargetFileTransferMessage(stream_id, buffer);
			file_trans_plugin_->SendProtoMessage(stream_id, message);
        }
		//"GetFileList kFileOperateRespDel post error."
	});

	file_trans_manager_->RegCreateNewFolderCallback([=, this](const std::string& stream_id, int resp_seq, bool ret, std::string parent_path, std::string new_path, std::string er_msg) {
		auto message = std::make_shared<tc::Message>();
		message->set_type(tc::kFileOperateRespCreateNewFolder);
		message->set_file_operate_resp_sequence(resp_seq);
		
		if (ret) {
			message->set_file_operate_resp_code(tc::RespCode::kRespCodeOk);
			//message->set_resp_message(QStringLiteral("新建文件夹成功").toStdString());
			message->set_file_operate_resp_message(ykTr("id_file_trans_new_folder_success"));
		}
		else {
			message->set_file_operate_resp_code(tc::RespCode::kRespCodeError);
			//message->set_resp_message(QStringLiteral("新建文件夹失败").toStdString());
			message->set_file_operate_resp_message(ykTr("id_file_trans_new_folder_failed"));
		}
		tc::FileOperateRespCreateNewFolder* resp = new tc::FileOperateRespCreateNewFolder();
		resp->set_path_of_parent(parent_path);
		resp->set_ret(ret);
		resp->set_path_of_new_created(new_path);
		resp->set_msg_of_error(er_msg);
		message->set_allocated_file_operate_resp_create_new_folder(resp);
        // auto buffer = ProtoAsData(message);
		// 这里最好加个返回值判断
        if (IsFileTransferEnabled()) {
            // file_trans_plugin_->DispatchTargetFileTransferMessage(stream_id, buffer);
			file_trans_plugin_->SendProtoMessage(stream_id, message);
        }
		// "GetFileList kFileOperateRespCreateNewFolder post error."
	});

	file_trans_manager_->RegRenameCallback([=, this](const std::string& stream_id, int resp_seq, bool ret, std::string old_path, std::string new_path, std::string er_msg) {
		auto message = std::make_shared<tc::Message>();
		message->set_type(tc::kFileOperateRespRename);
		message->set_file_operate_resp_sequence(resp_seq);
		if (ret) {
			message->set_file_operate_resp_code(tc::RespCode::kRespCodeOk);
			//message->set_resp_message(QStringLiteral("重命名成功").toStdString());
			message->set_file_operate_resp_message(ykTr("id_file_trans_rename_success"));
		}
		else {
			message->set_file_operate_resp_code(tc::RespCode::kRespCodeError);
			//message->set_resp_message(QStringLiteral("重命名失败").toStdString());
			message->set_file_operate_resp_message(ykTr("id_file_trans_rename_failed"));

		}
		tc::FileOperateRespRename* resp = new tc::FileOperateRespRename();
		resp->set_ret(ret);
		resp->set_path_of_old(old_path);
		resp->set_path_of_new(new_path);
		resp->set_msg_of_error(er_msg);
		message->set_allocated_file_operate_resp_rename(resp);
        // auto buffer = ProtoAsData(message);
		// 这里最好加个返回值判断
        if (IsFileTransferEnabled()) {
            // file_trans_plugin_->DispatchTargetFileTransferMessage(stream_id, buffer);
			file_trans_plugin_->SendProtoMessage(stream_id, message);
        }
		//"GetFileList kFileOperateRespRename post error."
	});

    file_trans_manager_->file_transmit_impl_->upload_task_created_func_ = [=, this](const std::string& task_id, const std::string& device_id, const std::string& src_path, const std::string& dst_path) {
        // auto event = std::make_shared<GrPluginFileTransferBegin>();
        // event->the_file_id_ = task_id;
        // event->begin_timestamp_ = (int64_t)TimeUtil::GetCurrentTimestamp();
        // event->visitor_device_id_ = device_id;
        // event->direction_ = "In";
        // event->file_detail_ = dst_path;
        // file_trans_plugin_->CallbackEvent(event);
    };
}

bool FileTransmitMsgInterface::IsFileTransferEnabled() {
    // return file_trans_plugin_ && file_trans_plugin_->GetPluginSettingsInfo().file_transfer_enabled_;

	return true;
}

uint64_t FileTransmitMsgInterface::GetMaxSpeedBybitPerSecond() {
	return file_trans_manager_->GetMaxSpeedBybitPerSecond();
}

void FileTransmitMsgInterface::SetMaxSpeedBybitPerSecond(uint64_t speed) {
	file_trans_manager_->SetMaxSpeedBybitPerSecond(speed);
}

}