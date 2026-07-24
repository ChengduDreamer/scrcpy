#include "file_transmit_manager.h"
// #include "tc_message.pb.h"
// #include "tc_common_new/log.h"
#include "cpp_base_lib/yk_logger.h"
#include "file_operate.h"
#include "file_transmit_impl.h"

namespace tc {
	FileTransmitManager::FileTransmitManager() {
		file_operate_thread_ = std::make_shared<asio2::iopool>(1);
		file_operate_thread_->start();

		file_upload_thread_ = std::make_shared<asio2::iopool>(1);
		file_upload_thread_->start();

		file_download_thread_ = std::make_shared<asio2::iopool>(1);
		file_download_thread_->start();

		file_operate_ = std::make_shared<FileOperate>();

		file_transmit_impl_ = std::make_shared<FileTransmitImpl>();
	}

	FileTransmitManager::~FileTransmitManager() {
		file_operate_thread_->stop();
		file_upload_thread_->stop();
		file_download_thread_->stop();
	}

	void FileTransmitManager::HandleFileTransmitMessage(const std::shared_ptr<tc::Message>& message) {
		if (!message->has_file_trans_data_packet()) {
			YK_LOGE("FileTransmitManager::HandleFileTransmitMessage error : msg !has_file_trans_data_packet");
			return;
		}
		auto file_data_packet = message->file_trans_data_packet();
		auto transmit_direction = file_data_packet.transmit_direction();

		file_upload_thread_->post(std::move([=]() {
			file_transmit_impl_->HandleUpload(message->device_id(), message->stream_id(), file_data_packet);
		}));

	}

	void FileTransmitManager::HandleFileOperateMsg(const std::shared_ptr<tc::Message>& msg) {
		if (!msg->has_file_operateions_event()) {
			YK_LOGE("file_operateions_event is null.");
			return;
		}
		auto operate = msg->file_operateions_event();
		YK_LOGI("HandleFileoperateMsg operate_type {} ",tc::FileOperateionsEvent_OperateType_Name(operate.operate_type()));
		auto seq = msg->file_operate_sequence(); // 指令序号. 消息对应的操作完成以后，要将结果封装为消息反馈给客户端，还要将指令序号返回回去
		if (operate.operate_type() == tc::FileOperateionsEvent::kGetFilesList) {
			std::string path = operate.path_of_filelist();
			YK_LOGI("[Android] HandleFileOperateMsg kGetFilesList seq={}, path={}, thread_tasks={}", seq, path, file_operate_thread_->size());
			file_operate_thread_->post(std::move([=]() {
				YK_LOGI("[Android] GetFilesList START seq={}, path={}", seq, path);
				auto get_files_list_res = file_operate_->GetFilesList(path);
				YK_LOGI("[Android] GetFilesList END seq={}, path={}, ret={}, file_count={}", seq, path, std::get<0>(get_files_list_res), std::get<1>(get_files_list_res).size());
				if (get_file_list_callback_) {
					get_file_list_callback_(msg->stream_id(), seq, std::get<0>(get_files_list_res), std::get<1>(get_files_list_res), std::get<2>(get_files_list_res), path, std::get<3>(get_files_list_res));
				}
			}));
		}
		else if (operate.operate_type() == tc::FileOperateionsEvent::kRecursiveGetFilesList) {
			std::string path = operate.path_of_filelist();
			file_operate_thread_->post(std::move([=]() {
				auto get_files_list_res = file_operate_->RecursiveGetFilesList(path);
				if (get_file_list_callback_) {
					get_file_list_callback_(msg->stream_id(), seq, std::get<0>(get_files_list_res), std::get<1>(get_files_list_res), std::get<2>(get_files_list_res), path, std::get<3>(get_files_list_res));
				}
			}));
		}
		else if (operate.operate_type() == tc::FileOperateionsEvent::kDel) {
			auto paths = operate.paths_of_del();
			file_operate_thread_->post(std::move([=]() {
				std::vector<std::string> paths_vec;
				for (auto& path : paths) {
					paths_vec.emplace_back(path);
				}
				auto remove_res = file_operate_->Remove(paths_vec);
				if (remove_callback_) {
					remove_callback_(msg->stream_id(), seq, std::get<0>(remove_res), std::get<1>(remove_res), std::get<2>(remove_res));
				}
				})
			);
		}
		else if (operate.operate_type() == tc::FileOperateionsEvent::kBatchCreateFolders) {
			auto paths = operate.paths_of_create_folder();
			file_operate_thread_->post(std::move([=]() {
				std::vector<std::string> error_paths;
				std::string er_msg;
				for (auto path : paths) {
					auto create_folder_res = file_operate_->CreateFolder(path);
					if (!(std::get<0>(create_folder_res))) {
						error_paths.emplace_back(path);
						er_msg = er_msg + std::get<1>(create_folder_res) + ";";
					}
				}
				if (batch_create_folders_callback_) {
					batch_create_folders_callback_(msg->stream_id(), seq, error_paths, er_msg);
				}
			}));
		}
		else if (operate.operate_type() == tc::FileOperateionsEvent::kCreateNewFolder) {
			auto u8_parent_path = operate.path_of_create_new_folder();
			file_operate_thread_->post(std::move([=]() {
				auto create_new_folder_res = file_operate_->CreateNewFolder(u8_parent_path);
				if (create_new_folder_callback_) {
					create_new_folder_callback_(msg->stream_id(), seq, std::get<0>(create_new_folder_res), u8_parent_path, std::get<1>(create_new_folder_res), std::get<2>(create_new_folder_res));
				}
			}));
		}
		else if (operate.operate_type() == tc::FileOperateionsEvent::kIsExists) {
			auto u8_path = operate.path_of_judge_exists();
			file_operate_thread_->post(std::move([=]() {
				auto exists_res = file_operate_->IsExists(u8_path);
				if (judge_file_exists_callback_) {
					judge_file_exists_callback_(msg->stream_id(), seq, u8_path, std::get<0>(exists_res), std::get<1>(exists_res), std::get<2>(exists_res));
				}
			}));
		}
		else if (operate.operate_type() == tc::FileOperateionsEvent::kRename) {
			std::string u8_old_path = operate.path_of_rename();
			std::string u8_new_name = operate.name_of_rename();
			file_operate_thread_->post(std::move([=]() {
				auto rename_res = file_operate_->Rename(u8_old_path, u8_new_name);
				if (rename_callback_) {
					rename_callback_(msg->stream_id(), seq, std::get<0>(rename_res), u8_old_path, std::get<1>(rename_res), std::get<2>(rename_res));
				}
			}));
		} 
		else if (operate.operate_type() == tc::FileOperateionsEvent::kDownload) {
			std::string download_path = operate.path_of_download();
			std::string save_path = operate.path_of_save();
			std::string task_id = operate.task_id();
			file_download_thread_->post(std::move([=]() {
				file_transmit_impl_->HandleDownload(msg->device_id(), msg->stream_id(), download_path, save_path, task_id);
			}));
		}
		else if (operate.operate_type() == tc::FileOperateionsEvent::kCancelDownload) {
			// FIX-7 修复：PC 取消下载指令，转发给传输层置 kCancel 并唤醒下载发送循环。
			// 与 PC 取消下载走 kFileTransSaveFileException 的现有路径并存，不删旧路径。
			std::string task_id = operate.task_id();
			YK_LOGI("[Android] HandleFileOperateMsg kCancelDownload task_id={}", task_id);
			file_transmit_impl_->HandleCancelTransmit(task_id, true);
		}
		else if (operate.operate_type() == tc::FileOperateionsEvent::kCancelUpload) {
			// FIX-7 修复：PC 取消上传指令（防御性分支，实际取消上传主要靠数据包 kCancel 态）。
			std::string task_id = operate.task_id();
			YK_LOGI("[Android] HandleFileOperateMsg kCancelUpload task_id={}", task_id);
			file_transmit_impl_->HandleCancelTransmit(task_id, false);
		}
	}

	// 对端保存文件异常或者对端取消任务 会发送此消息
	void FileTransmitManager::HandleSaveFileExceptionMessage(const std::shared_ptr<tc::Message>& message) {
		if (!message->has_file_trans_save_file_exception()) {
			YK_LOGE("FileTransmitManager::HandleSaveFileExceptionMessage error : msg !file_transmit_download_exception");
			return;
		}
		auto save_file_exception = message->file_trans_save_file_exception();
		file_transmit_impl_->HandleSaveFileException(message->stream_id(), save_file_exception);
	}

	void FileTransmitManager::HandleFileTransDataPacketResponseMessage(const std::shared_ptr<tc::Message>& message) {
		if (!message->has_file_trans_data_packet_response()) {
			YK_LOGE("FileTransmitManager::HandleFileTransDataPacketResponseMessage error : msg !has_file_trans_data_packet_response");
			return;
		}
		auto file_trans_data_packet_resp = message->file_trans_data_packet_response();
		file_transmit_impl_->HandleFileTransDataPacketResponse(message->stream_id(), file_trans_data_packet_resp);
	}

	uint64_t FileTransmitManager::GetMaxSpeedBybitPerSecond() {
		return file_transmit_impl_->GetMaxSpeedBybitPerSecond();
	}

	void FileTransmitManager::SetMaxSpeedBybitPerSecond(uint64_t speed) {
		file_transmit_impl_->SetMaxSpeedBybitPerSecond(speed);
	}

	void FileTransmitManager::OnConnectionLost() {
		if (file_transmit_impl_) {
			file_transmit_impl_->OnConnectionLost();
		}
	}
}