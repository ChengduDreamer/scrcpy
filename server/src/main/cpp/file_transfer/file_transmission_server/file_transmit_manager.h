#pragma once

#include <memory>
#include <functional>
#include <asio2/asio2.hpp>
#include "tc_message.pb.h"

namespace tc {
	class Message;
}

namespace tc {

	class FileOperate;

	class FileTransmitImpl;

	class FileTransmitManager {
	public:
		using GetFileListCallbackFuncType = std::function<void(const std::string& stream_id, int resp_seq, bool ret, std::vector<tc::FileDescInfo> file_infos, std::string error_msg, std::string target_path, std::string file_permission_path)>;

		using BatchCreateFoldersCallbackFuncType = std::function<void(const std::string& stream_id, int resp_seq, std::vector<std::string> no_created_paths, std::string error_msg)>;

		using JudgeFileExistsCallbackFuncType = std::function<void(const std::string& stream_id, int resp_seq, std::string path, bool exists, uint64_t file_size, uint64_t file_date)>;

		using RemoveCallbackFuncType = std::function<void(const std::string& stream_id, int resp_seq, bool ret, std::vector<std::string> no_remove_paths, std::string error_msg)>;

		using CreateNewFolderCallbackFuncType = std::function<void(const std::string& stream_id, int resp_seq, bool ret, std::string parent_path, std::string new_created_path, std::string error_msg)>;

		using RenameCallbackFuncType = std::function<void(const std::string& stream_id, int resp_seq, bool ret, std::string old_path, std::string new_path, std::string error_msg)>;

		FileTransmitManager();

		~FileTransmitManager();

		void HandleFileTransmitMessage(const std::shared_ptr<tc::Message>& message);

		void HandleFileOperateMsg(const std::shared_ptr<tc::Message>& msg);

		// 对端保存文件异常或者对端取消任务 会发送此消息
		void HandleSaveFileExceptionMessage(const std::shared_ptr<tc::Message>& message);

		void HandleFileTransDataPacketResponseMessage(const std::shared_ptr<tc::Message>& message);
		
		void RegGetFileListCallback(GetFileListCallbackFuncType callback) {
			get_file_list_callback_ = callback;
		}

		void RegBatchCreateFoldersCallback(BatchCreateFoldersCallbackFuncType callback) {
			batch_create_folders_callback_ = callback;
		}

		void RegJudgeFileExistsCallback(JudgeFileExistsCallbackFuncType callback) {
			judge_file_exists_callback_ = callback;
		}

		void RegRemoveCallback(RemoveCallbackFuncType callback) {
			remove_callback_ = callback;
		}

		void RegCreateNewFolderCallback(CreateNewFolderCallbackFuncType callback) {
			create_new_folder_callback_ = callback;
		}

		void RegRenameCallback(RenameCallbackFuncType callback) {
			rename_callback_ = callback;
		}

		void SetMaxSpeedBybitPerSecond(uint64_t speed);

		uint64_t GetMaxSpeedBybitPerSecond();

		// 文件上传相关
		std::shared_ptr<FileTransmitImpl> file_transmit_impl_;
	private:
		// 文件操作指令相关
		std::shared_ptr<FileOperate> file_operate_ = nullptr;

		// 文件操作线程
		std::shared_ptr<asio2::iopool> file_operate_thread_ = nullptr;

		GetFileListCallbackFuncType get_file_list_callback_ = nullptr;

		BatchCreateFoldersCallbackFuncType batch_create_folders_callback_ = nullptr;

		JudgeFileExistsCallbackFuncType judge_file_exists_callback_ = nullptr;

		RemoveCallbackFuncType remove_callback_ = nullptr;

		CreateNewFolderCallbackFuncType create_new_folder_callback_ = nullptr;

		RenameCallbackFuncType rename_callback_ = nullptr;

		// 接受文件上传线程
		std::shared_ptr<asio2::iopool> file_upload_thread_ = nullptr;

		// 处理文件下载线程
		std::shared_ptr<asio2::iopool> file_download_thread_ = nullptr;
	};
}