#include "file_transmit_impl.h"
#include <qdir.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qstring.h>
#include "tc_common_new/log.h"
#include "tc_common_new/time_util.h"
#include "tc_common_new/file.h"
#include "tc_common_new/data.h"
#include "tc_common_new/md5.h"

namespace tc {

	class FixedSizeDeque {
	private:
		std::deque<uint64_t> dq_;
		const size_t max_size_ = 20;

	public:
		void Push(int value) {
			if (dq_.size() >= max_size_) {
				dq_.pop_front(); 
			}
			dq_.push_back(value);
		}
		void Pint() const {
            LOGI("g_file_index_deque start print");
			for (uint64_t index : dq_) {
				LOGI("g_file_index_deque index: {}", index);
			}
			LOGI("g_file_index_deque end print");
		}
		void Clear() {
			dq_.clear();
		}
	};

	FixedSizeDeque g_file_index_deque;

	std::mutex FileTransmitImpl::file_transmit_mutex_;

	std::map<std::string, FileTransmitImpl::EFileTransmitTaskSimpleState> FileTransmitImpl::file_transmit_task_with_simple_state_;

	FileTransmitImpl::FileTransmitImpl() : io_ctx_(1), asio_timer_(io_ctx_) {
		io_ctx_.start();
		AddTimer(std::chrono::milliseconds(6000), std::bind(&FileTransmitImpl::On6000msTimer, this));
	}

	FileTransmitImpl::~FileTransmitImpl() {
		asio_timer_.stop();
		io_ctx_.stop();
	}

	void FileTransmitImpl::On6000msTimer() {
		std::lock_guard<std::mutex> lg(id_with_upload_task_mutex_);
		for (auto it = id_with_upload_task_.begin(); it != id_with_upload_task_.end(); ++it) {
			if (it->second->is_ended_) {
				if (it->second->file_ptr_) {
					if (it->second->file_ptr_->IsOpen()) {
						it->second->file_ptr_->Close();
					}
				}
				continue;
			}
			auto now = tc::TimeUtil::GetCurrentTimestamp();
			if (now - it->second->last_update_time_ >= 14 * 1000) {
				it->second->is_ended_ = true;
				if (it->second->file_ptr_) {
					if (it->second->file_ptr_->IsOpen()) {
						it->second->file_ptr_->Close();
					}
				}
			}
		}
	}

	void FileTransmitImpl::HandleUpload(const std::string& device_id, const std::string& stream_id, tc::FileTransDataPacket file_data_packet) {
		std::string task_id;
		try {
			task_id = file_data_packet.task_id();
			std::string file_data = file_data_packet.data();
			uint64_t index = file_data_packet.index();
			std::string src_file_path = file_data_packet.src_file_path();
			std::string target_file_path = file_data_packet.target_file_path();
			auto transmit_state = file_data_packet.transmit_state();
			uint64_t src_file_size = file_data_packet.file_size();
			std::string data = file_data_packet.data();
			std::lock_guard<std::mutex> lck{ id_with_upload_task_mutex_ };

			if (0 == index % 100 && send_file_trans_data_packet_response_func_) {
				auto message = std::make_shared<tc::Message>();
				message->set_type(tc::kFileTransDataPacketResponse);
				auto response = new FileTransDataPacketResponse();
				response->set_task_id(task_id);
				response->set_index(index);
				message->set_allocated_file_trans_data_packet_response(response);
				send_file_trans_data_packet_response_func_(stream_id, message);
			}

			if (!id_with_upload_task_.count(task_id)) {
				//新的上传任务
				auto upload_task = std::make_shared<FileUploadTask>();
				id_with_upload_task_[task_id] = upload_task;
				upload_task->task_id_ = task_id;
				upload_task->src_file_path_ = src_file_path;
				upload_task->target_file_path_ = target_file_path;
				upload_task->current_packet_index_ = index;
				QDir temp_path{ QString::fromStdString(target_file_path) };
				QString temp_path_str = temp_path.absoluteFilePath("..");
				QString path_str = QDir(temp_path_str).absolutePath();
				QDir target_dir{path_str};

				if (!target_dir.exists()) {
					target_dir.mkpath(".");
				}

                // report file transfer
                if (upload_task_created_func_) {
                    upload_task_created_func_(upload_task->task_id_, device_id, src_file_path, target_file_path);
                }

				if (!target_dir.exists()) {
					LOGE("HandleUplaod error, target_dir = {} , can not be created.", target_dir.path().toStdString());
					id_with_upload_task_[task_id]->is_ended_ = true;
					call_upload_callback(stream_id, task_id, FileUploadTask::EFileUploadState::kDirFailedCreate);
					return;
				}
				upload_task->file_ptr_ = File::OpenForWriteB(target_file_path);
				if (!id_with_upload_task_[task_id]->file_ptr_->IsOpen()) {
					upload_task->is_ended_ = true;
					call_upload_callback(stream_id, task_id, FileUploadTask::EFileUploadState::kFailedOpen);
					return;
				}
			}
			else {
				g_file_index_deque.Push(index);
				if (index - id_with_upload_task_[task_id]->current_packet_index_ != 1) {
					// 发生了丢包
					LOGE("kPacketLoss file name : {}", id_with_upload_task_[task_id]->target_file_path_);
					LOGE("index : {}", index);
					LOGE("id_with_upload_task_[task_id]->current_packet_index_ : {}", id_with_upload_task_[task_id]->current_packet_index_);
					g_file_index_deque.Pint();
					g_file_index_deque.Clear();
					if (id_with_upload_task_[task_id]->file_ptr_) {
						id_with_upload_task_[task_id]->file_ptr_->Close();
					}
					id_with_upload_task_[task_id]->is_ended_ = true;
					call_upload_callback(stream_id, task_id, FileUploadTask::EFileUploadState::kPacketLoss);
					return;
				}
				id_with_upload_task_[task_id]->current_packet_index_ = index;
			}

			if (id_with_upload_task_[task_id]->is_ended_) {
				return;
			}
			id_with_upload_task_[task_id]->last_update_time_ = tc::TimeUtil::GetCurrentTimestamp();
			if (id_with_upload_task_[task_id]->file_ptr_ && id_with_upload_task_[task_id]->file_ptr_->IsOpen()) {
				if (!data.empty()) {
					auto append_size = id_with_upload_task_[task_id]->file_ptr_->Append(data.data(), data.size());
					if (append_size != data.size()) {
						LOGE("FileTransmitImpl::HandleUplaod append_size != data.size");
						id_with_upload_task_[task_id]->file_ptr_->Close();
						id_with_upload_task_[task_id]->is_ended_ = true;
						call_upload_callback(stream_id, task_id, FileUploadTask::EFileUploadState::kFailedWrite);
						return;
					}
				}
			}
			if (tc::FileTransDataPacket::kTransmitting != transmit_state) {
				if (id_with_upload_task_[task_id]->file_ptr_) {
					id_with_upload_task_[task_id]->file_ptr_->Close();
				}
				id_with_upload_task_[task_id]->is_ended_ = true;
			}
			switch (transmit_state)
			{
			case tc::FileTransDataPacket::kEnd: { // 对端已经上传完毕
                // TODO: 使用QFileInfo读取
				QFile file{QString::fromStdString(id_with_upload_task_[task_id]->target_file_path_)};
				auto target_file_size = file.size();
                LOGI("FileTransDataPacket::kEnd, src size: {}, target size: {}, file: {}", src_file_size, target_file_size, file.fileName().toStdString());
				if (src_file_size == target_file_size) { // to do 先校验下大小，后面再考虑校验md5
					call_upload_callback(stream_id, task_id, FileUploadTask::EFileUploadState::kSuccess);
				}
				else {
					call_upload_callback(stream_id, task_id, FileUploadTask::EFileUploadState::kFailedVerify);
				}
				break;
			}
			case tc::FileTransDataPacket::kError: // 一般是对端读文件异常了
				break;
			case tc::FileTransDataPacket::kCancel:
				break;
			default:
				break;
			}
		}
		catch (std::exception& e) {
			std::string s = e.what();
			LOGE("FileTransmitImpl::HandleUplaod error is {}", s);
			std::lock_guard<std::mutex> lck{ id_with_upload_task_mutex_ };
			if (id_with_upload_task_.count(task_id) > 0) {
				if (id_with_upload_task_[task_id]->file_ptr_) {
					id_with_upload_task_[task_id]->file_ptr_->Close();
				}
				id_with_upload_task_[task_id]->is_ended_ = true;
			}
			if (!task_id.empty()) {
				call_upload_callback(stream_id, task_id, FileUploadTask::EFileUploadState::kUnknownError);
			}
		}
	}

	void FileTransmitImpl::call_upload_callback(const std::string& stream_id, const std::string& task_id, FileUploadTask::EFileUploadState state) {
		if (!upload_resp_func_) {
			LOGE("FileTransmitImpl upload_callback_ is null.");
			return;
		}
		auto resp_upload = new tc::FileTransRespUpload();
		resp_upload->set_task_id(task_id);
		resp_upload->set_res(false);
		switch (state)
		{
		case tc::FileUploadTask::EFileUploadState::kUnknownError:
			resp_upload->set_error_cause(tc::FileTransRespUpload::kUnknow);
			break;
		case tc::FileUploadTask::EFileUploadState::kFailedOpen:
			resp_upload->set_error_cause(tc::FileTransRespUpload::kFailedOpen);
			break;
		case tc::FileUploadTask::EFileUploadState::kFailedWrite:
			resp_upload->set_error_cause(tc::FileTransRespUpload::kFailedWrite);
			break;
		case tc::FileUploadTask::EFileUploadState::kDirFailedCreate:
			resp_upload->set_error_cause(tc::FileTransRespUpload::kDirFailedCreate);
			break;
		case tc::FileUploadTask::EFileUploadState::kFailedVerify:
			resp_upload->set_error_cause(tc::FileTransRespUpload::kFailedVerify);
			break;
		case tc::FileUploadTask::EFileUploadState::kPacketLoss:
			resp_upload->set_error_cause(tc::FileTransRespUpload::kPacketLoss);
			break;
		case tc::FileUploadTask::EFileUploadState::kSuccess:
			resp_upload->set_res(true);
			break;
		default:
			resp_upload->set_error_cause(tc::FileTransRespUpload::kUnknow);
			break;
		}
		upload_resp_func_(stream_id, resp_upload);
	}

	// 有异常的时候才会调用call_download_callback， 没有异常的话 直接发送下载数据包了
	void FileTransmitImpl::call_download_callback(const std::string& device_id, const std::string& stream_id, const std::string& task_id, FileDownloadTask::EFileDownloadState state) {
		auto resp_download = new tc::FileTransRespDownload();
		resp_download->set_task_id(task_id);
		resp_download->set_res(false);
		switch (state)
		{
		case tc::FileDownloadTask::EFileDownloadState::kNoExists:
			resp_download->set_error_cause(tc::FileTransRespDownload::kNoExists);
			break;
		case tc::FileDownloadTask::EFileDownloadState::kFailedOpen:
			resp_download->set_error_cause(tc::FileTransRespDownload::kFailedOpen);
			break;
		default:
			resp_download->set_error_cause(tc::FileTransRespDownload::kUnknow);
			break;
		}
		download_except_func_(task_id, device_id, stream_id, resp_download);
	}

	void FileTransmitImpl::HandleDownload(const std::string& device_id, const std::string& stream_id, const std::string& download_path, const std::string& save_path, const std::string& task_id) {
		{
			std::lock_guard<std::mutex> lck{ file_transmit_mutex_ };
			file_transmit_task_with_simple_state_[task_id] = EFileTransmitTaskSimpleState::kNormal;
		}
		try {
			const std::size_t buffer_size = kSingleBufferSize;
			char buffer[buffer_size] = { 0, };
			QString download_path_qstr = QString::fromStdString(download_path);
			QFile file{ download_path_qstr };
			if (!file.exists()) {
				LOGD("File no exists %s error", download_path.c_str());
				call_download_callback(device_id, stream_id, task_id, tc::FileDownloadTask::EFileDownloadState::kNoExists);
				return;
			}
			uint64_t file_size = file.size();
			std::wstring download_pathw = download_path_qstr.toStdWString();
			FILE* pf = _wfopen(download_pathw.c_str(), L"rb");
			if (!pf) {
				LOGE("File open %s error", download_path.c_str());
				call_download_callback(device_id, stream_id, task_id, tc::FileDownloadTask::EFileDownloadState::kFailedOpen);
				return;
			}
			std::shared_ptr<void> auto_close_file{nullptr, [=](void* buf) {
				fclose(pf);
			}};

			ResetTokenBucket();
			int timer_id = AddTimer(std::chrono::milliseconds(100), std::bind(&FileTransmitImpl::GrantTokenBucket, this));
			std::shared_ptr<void> auto_close_timer{ nullptr, [=](void*) {
				StopTimer(timer_id);
				ResetTokenBucket();
			} };

            // 即将开始下载
            if (download_begin_func_) {
                download_begin_func_(task_id, device_id, stream_id, download_path);
            }

			uint64_t statistics_readed_size = 0; // 已读取字节数
			uint64_t index = 0; // 消息序列
			bool is_abort = false;
			while (true) {
				if (is_abort) {
					return;
				}
				bool is_send_msg = true;
				auto msg = std::make_shared<tc::Message>();
				msg->set_type(tc::kFileTransDataPacket);
				auto file_data_packet = new tc::FileTransDataPacket();
				file_data_packet->set_index(index++);
				file_data_packet->set_transmit_direction(tc::FileTransDataPacket::kDownload);
				file_data_packet->set_task_id(task_id);
				file_data_packet->set_src_file_path(download_path);
				file_data_packet->set_target_file_path(save_path);
				file_data_packet->set_file_size(file_size);
				msg->set_allocated_file_trans_data_packet(file_data_packet);
				std::shared_ptr<void> auto_send{ nullptr, [=, &is_send_msg, &is_abort](void* buf) {

					if (0 >= token_bucket_) {
						int loop_count = 0;
						while (true) {
							std::unique_lock<std::mutex> lck{ grant_token_mutex_ };
							auto res = grant_token_cv_.wait_for(lck, std::chrono::milliseconds(1), [=]() ->bool {
								if (token_bucket_ > 0) {
									return true;
								}
								return false;
							});
							if (res || loop_count > 100) {
								break;
							}
							++loop_count;
						}
					}

					{
						bool need_wait = false;
						{
							std::unique_lock<std::mutex> lck{ grant_token_mutex_ };
							if (task_id_with_recved_index_.count(task_id)) {
								LOGW("index - task_id_with_recved_index_[task_id] = {}", index - task_id_with_recved_index_[task_id]);
								if (index - task_id_with_recved_index_[task_id] >= 180) {
									need_wait = true;
								}
							}
						}
						if (need_wait) {
							int loop_count = 0;
							while (true) {
								std::unique_lock<std::mutex> lck{ grant_token_mutex_ };
								auto res = grant_token_cv_.wait_for(lck, std::chrono::milliseconds(1), [=]() ->bool {
									if (index - task_id_with_recved_index_[task_id] < 180) {
										return true;
									}
									return false;
								});
								if (res || loop_count > 10) {
									break;
								}
								++loop_count;
							}
						}
					}

					if (!is_send_msg) {
						return;
					}
					if (!send_data_packet_func_) {
						LOGI("HandleFileoperateMsg send_data_packet_callback_ is nullptr.");
						is_abort = true;
						return;
					}
					if (!send_data_packet_func_(stream_id, msg)) {
						is_abort = true;
						LOGE("HandleDownload send msg time out.");
						return;
					}
					--token_bucket_;
					//std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}};
				{ // 判断任务是否被取消 或者 对端发生了异常
					std::lock_guard<std::mutex> lck{ file_transmit_mutex_ };
					if (EFileTransmitTaskSimpleState::kNormal != file_transmit_task_with_simple_state_[task_id]) {
						is_send_msg = false;
						LOGW(" FileTransmitImpl::HandleDownload cancel or error, download_path = {}", download_path.c_str());
						return;
					}
				}
				std::size_t readed_size = fread(buffer, 1, buffer_size, pf);
				if (readed_size > 0) {
					statistics_readed_size += readed_size;
					file_data_packet->set_data(buffer, readed_size);
					if (feof(pf)) { // 文件结束
                        LOGI("File at end: {}, total bytes: {}", download_path_qstr.toStdString(), statistics_readed_size);
						file_data_packet->set_transmit_state(tc::FileTransDataPacket::kEnd);

                        // 下载正常结束
                        if (download_end_func_) {
                            download_end_func_(task_id, device_id, stream_id, download_path);
                        }
						break;
					}
					else {
						file_data_packet->set_transmit_state(tc::FileTransDataPacket::kTransmitting);
						continue;
					}
				}
				else {
					if (feof(pf)) {
                        LOGI("File at end: {}, total bytes: {}", download_path_qstr.toStdString(), statistics_readed_size);
						file_data_packet->set_transmit_state(tc::FileTransDataPacket::kEnd);

                        // 下载正常结束
                        if (download_end_func_) {
                            download_end_func_(task_id, device_id, stream_id, download_path);
                        }
					}
					else {
						file_data_packet->set_transmit_state(tc::FileTransDataPacket::kError);
						LOGE("File read %s error", download_path.c_str());
						return;
					}
					break;
				}
			} // end while
		}
		catch (std::exception& e) {
			std::string s = e.what();
			LOGE("FileTransmitImpl::HandleDownload error is {}.", s);
		}
	}

	void FileTransmitImpl::HandleSaveFileException(const std::string& stream_id, tc::FileTransSaveFileException save_exception) {
		auto error_cause = save_exception.error_cause();
		auto src_file_path = save_exception.src_file_path();
		auto target_file_path = save_exception.target_file_path();
		auto task_id = save_exception.task_id();
		LOGD("HandleSaveFileException task_id is {}, src_file_path is {}, target_file_path is {}, error is ", task_id, src_file_path, target_file_path);
		switch (error_cause)
		{
		case tc::FileTransSaveFileException::kFailedOpen:
			LOGD("FileTransSaveFileException::kFailedOpen");
			break;
		case tc::FileTransSaveFileException::kFailedWrite:
			LOGD("FileTransSaveFileException::kFailedWrite");
			break;
		case tc::FileTransSaveFileException::kCancel:
			LOGD("FileTransSaveFileException::kCancel");
			break;
		case tc::FileTransSaveFileException::kDirFailedCreate:
			LOGD("FileTransSaveFileException::kDirFailedCreate");
			break;
		case tc::FileTransSaveFileException::kPacketLoss:
			LOGD("FileTransSaveFileException::kPacketLoss");
			break;
		case tc::FileTransSaveFileException::kUnknow:
			LOGD("FileTransSaveFileException::kUnknow");
			break;
		default:
			LOGD("HandleSaveFileException unknow");
			break;
		}
		{
			std::lock_guard<std::mutex> lck{ file_transmit_mutex_ };
			if (file_transmit_task_with_simple_state_.count(task_id)) {
				if (tc::FileTransSaveFileException::kCancel == error_cause) {
					file_transmit_task_with_simple_state_[task_id] = EFileTransmitTaskSimpleState::kCancel;
				}	
				else {
					file_transmit_task_with_simple_state_[task_id] = EFileTransmitTaskSimpleState::kOppositeEndError;
				}
			}
		}
	}

	void FileTransmitImpl::HandleFileTransDataPacketResponse(const std::string& stream_id, tc::FileTransDataPacketResponse data_packet_resp) {
		uint64_t recved_index = data_packet_resp.index();
		std::string task_id = data_packet_resp.task_id();
		std::unique_lock<std::mutex> lck{ grant_token_mutex_ };
		LOGI("FileTransmitImpl::HandleFileTransDataPacketResponse 0");
		task_id_with_recved_index_[task_id] = recved_index;
		grant_token_cv_.notify_all();
	}

	void FileTransmitImpl::GrantTokenBucket() {
		token_bucket_ = token_bucket_ + speed_by_MB_per_100ms_ / kSingleBufferSize;
		std::unique_lock<std::mutex> lck{ grant_token_mutex_ };
		grant_token_cv_.notify_all();
	}

	void FileTransmitImpl::ResetTokenBucket() {
		token_bucket_ = 10;
	}

	void FileTransmitImpl::SetMaxSpeedBybitPerSecond(uint64_t speed) {
		if (0 == speed) {
			return;
		}
		speed_by_bit_per_1000ms_ = speed;
		speed_by_MB_per_100ms_ = speed_by_bit_per_1000ms_ * 0.1 * 0.1 * 0.85;
		if (speed_by_MB_per_100ms_ > kMaxSpeedByMBPer100ms) {
			speed_by_MB_per_100ms_ = kMaxSpeedByMBPer100ms;
		}
		LOGI("speed_by_MB_per_100ms_ is {}", speed_by_MB_per_100ms_);
	}

	uint64_t FileTransmitImpl::GetMaxSpeedBybitPerSecond() {
		return speed_by_bit_per_1000ms_;
	}
}
