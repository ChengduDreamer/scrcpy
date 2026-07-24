#include "file_transmit_impl.h"
#include <filesystem>
#include <vector>
#include "cpp_base_lib/yk_logger.h"
#include "cpp_base_lib/time_util.h"
#include "cpp_base_lib/file.h"
#include "cpp_base_lib/data.h"
#include "cpp_base_lib/md5.h"

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
            YK_LOGI("g_file_index_deque start print");
			for (uint64_t index : dq_) {
				YK_LOGI("g_file_index_deque index: {}", index);
			}
			YK_LOGI("g_file_index_deque end print");
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
		// BUG-2 修复（降级为内存治理）：原实现只对 is_ended_ 任务关句柄、从不 erase，
		// 长连接下 id_with_upload_task_ 持续增长（缓慢泄漏）。这里与 PC 侧
			// FileTransmitSDK::On6000msTimer 对称：收集已结束任务的 task_id，循环后 erase。
			// 注：task_id 单调递增不复用，故不存在"第二次传输误判丢包"（详见审查补充文档）。
			std::vector<std::string> ended_task_ids;
			{
				std::lock_guard<std::mutex> lg(id_with_upload_task_mutex_);
				for (auto it = id_with_upload_task_.begin(); it != id_with_upload_task_.end(); ++it) {
					if (it->second->is_ended_) {
						if (it->second->file_ptr_) {
							if (it->second->file_ptr_->IsOpen()) {
								it->second->file_ptr_->Close();
							}
						}
						// [DIAG] 记录被清理的已结束任务，便于与后续包到达日志拼出时间线。
						YK_LOGI("[Android-Upload][{}] TIMEOUT_CLEANUP reason=ended, target={}", it->first, it->second->target_file_path_);
						ended_task_ids.emplace_back(it->first);
						continue;
					}
					auto now = yk::TimeUtil::GetCurrentTimestamp();
					auto diff = now - it->second->last_update_time_;
					if (diff >= 14 * 1000) {
						it->second->is_ended_ = true;
						if (it->second->file_ptr_) {
							if (it->second->file_ptr_->IsOpen()) {
								it->second->file_ptr_->Close();
							}
						}
						// [DIAG] 超时清理：14 秒无数据包到达。若此 task 后续包再到达，
						// 会被 HandleUpload 的 is_ended_ 预检拦截（IGNORE_ENDED_TASK）。
						YK_LOGI("[Android-Upload][{}] TIMEOUT_CLEANUP reason=stale(14s), last_update={}ms ago, target={}", it->first, diff, it->second->target_file_path_);
						ended_task_ids.emplace_back(it->first);
					}
				}
				for (auto& id : ended_task_ids) {
					id_with_upload_task_.erase(id);
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

			// FIX-6 修复：原实现全程持 id_with_upload_task_mutex_，锁内调用
			// send_file_trans_data_packet_response_func_（ACK）与 call_upload_callback
			//（最终走到 SendProtoMessage->SendBinary，队列满会阻塞），会卡住整个上传接收线程。
			// 重构为：锁内只完成任务状态更新，把"要发的 ACK index / 要回调的状态 /
			// 要删除的半成品文件"记录到局部变量，解锁后再执行发送与回调。每包仍只进一次锁。
			bool need_send_ack = false;
			uint64_t ack_index = 0;
			auto ack_state = tc::FileTransDataPacket::kTransmitting;
			bool need_task_created_cb = false;
			bool need_upload_cb = false;
			FileUploadTask::EFileUploadState upload_cb_state = FileUploadTask::EFileUploadState::kUnknownError;
			// FIX-1 修复：取消/对端出错时删除半成品目标文件所需的信息
			bool need_remove_target = false;
			std::string remove_target_path;
			{
				std::lock_guard<std::mutex> lck{ id_with_upload_task_mutex_ };
				// FIX-6：标记是否提前结束本次处理（对应原实现的各个 return 分支），无回调的纯忽略也用它
				bool exit_early = false;

				if (0 == index % 100 && send_file_trans_data_packet_response_func_) {
					need_send_ack = true;
					ack_index = index;
					ack_state = transmit_state;
				}

				if (!id_with_upload_task_.count(task_id)) {
					//新的上传任务
					YK_LOGI("[Android-Upload][{}] TASK_START index={}, src_size={}, src={}, target={}", task_id, index, src_file_size, src_file_path, target_file_path);
					auto upload_task = std::make_shared<FileUploadTask>();
					id_with_upload_task_[task_id] = upload_task;
					upload_task->task_id_ = task_id;
					upload_task->src_file_path_ = src_file_path;
					upload_task->target_file_path_ = target_file_path;
					upload_task->current_packet_index_ = index;

					namespace fs = std::filesystem;
					fs::path target_file_path_fs(target_file_path);
					fs::path parent_path = target_file_path_fs.parent_path();

					if (!fs::exists(parent_path)) {
						fs::create_directories(parent_path);
					}

	                // report file transfer（FIX-6：延迟到解锁后回调）
	                if (upload_task_created_func_) {
	                    need_task_created_cb = true;
	                }

					if (!fs::exists(parent_path)) {
						YK_LOGE("[Android-Upload][{}] DIR_CREATE_FAIL parent={}", task_id, parent_path.string());
						id_with_upload_task_[task_id]->is_ended_ = true;
						need_upload_cb = true;
						upload_cb_state = FileUploadTask::EFileUploadState::kDirFailedCreate;
						exit_early = true;
					}
					else {
						upload_task->file_ptr_ = yk::File::OpenForWriteB(target_file_path);
						if (!id_with_upload_task_[task_id]->file_ptr_->IsOpen()) {
							YK_LOGE("[Android-Upload][{}] FILE_OPEN_FAIL target={}", task_id, target_file_path);
							upload_task->is_ended_ = true;
							need_upload_cb = true;
							upload_cb_state = FileUploadTask::EFileUploadState::kFailedOpen;
							exit_early = true;
						}
					}
				}
				else {
					g_file_index_deque.Push(index);
					if (index - id_with_upload_task_[task_id]->current_packet_index_ != 1) {
						// 发生了丢包
						// [DIAG] 关键失败点：上传接收侧判定丢包。这是上传成功率低的最可能直接原因。
						// 记录期望 index 与实际 index，定位是乱序到达还是真丢包。
						YK_LOGE("[Android-Upload][{}] PACKET_LOSS target={}, expected_index={}, got_index={}, gap={}",
							task_id, id_with_upload_task_[task_id]->target_file_path_,
							id_with_upload_task_[task_id]->current_packet_index_ + 1, index,
							index - id_with_upload_task_[task_id]->current_packet_index_ - 1);
						g_file_index_deque.Pint();
						g_file_index_deque.Clear();
						if (id_with_upload_task_[task_id]->file_ptr_) {
							id_with_upload_task_[task_id]->file_ptr_->Close();
						}
						id_with_upload_task_[task_id]->is_ended_ = true;
						need_upload_cb = true;
						upload_cb_state = FileUploadTask::EFileUploadState::kPacketLoss;
						exit_early = true;
					}
					else {
						id_with_upload_task_[task_id]->current_packet_index_ = index;
					}
				}

				if (!exit_early && id_with_upload_task_[task_id]->is_ended_) {
					YK_LOGW("[Android-Upload][{}] IGNORE_ENDED_TASK index={}", task_id, index);
					exit_early = true;
				}
				if (!exit_early) {
					id_with_upload_task_[task_id]->last_update_time_ = yk::TimeUtil::GetCurrentTimestamp();
					if (id_with_upload_task_[task_id]->file_ptr_ && id_with_upload_task_[task_id]->file_ptr_->IsOpen()) {
						if (!data.empty()) {
							auto append_size = id_with_upload_task_[task_id]->file_ptr_->Append(data.data(), data.size());
							if (append_size != data.size()) {
								YK_LOGE("[Android-Upload][{}] WRITE_FAIL index={}, append={}, expect={}, target={}", task_id, index, append_size, data.size(), id_with_upload_task_[task_id]->target_file_path_);
								id_with_upload_task_[task_id]->file_ptr_->Close();
								id_with_upload_task_[task_id]->is_ended_ = true;
								need_upload_cb = true;
								upload_cb_state = FileUploadTask::EFileUploadState::kFailedWrite;
								exit_early = true;
							}
							// [DIAG] 每 500 包打一次接收进度。
							else if (index % 500 == 0) {
								YK_LOGI("[Android-Upload][{}] PROGRESS index={}, data_size={}", task_id, index, data.size());
							}
						}
					}
					else {
						YK_LOGW("[Android-Upload][{}] FILE_CLOSED_IGNORE_WRITE index={}", task_id, index);
					}
				}
				if (!exit_early) {
					if (tc::FileTransDataPacket::kTransmitting != transmit_state) {
						if (id_with_upload_task_[task_id]->file_ptr_) {
							id_with_upload_task_[task_id]->file_ptr_->Close();
						}
						id_with_upload_task_[task_id]->is_ended_ = true;
					}
					switch (transmit_state)
					{
					case tc::FileTransDataPacket::kEnd: { // 对端已经上传完毕
		                namespace fs = std::filesystem;
		                fs::path target_file(id_with_upload_task_[task_id]->target_file_path_);
		                std::error_code size_ec;
		                auto target_file_size = fs::file_size(target_file, size_ec);
		                if (size_ec) {
		                    YK_LOGE("[Android-Upload][{}] RESULT=VERIFY_FAIL(cannot stat) target={}, ec={}", task_id, target_file.string(), size_ec.message());
							need_upload_cb = true;
							upload_cb_state = FileUploadTask::EFileUploadState::kFailedVerify;
		                    break;
		                }
		                YK_LOGI("[Android-Upload][{}] kEnd src_size={}, target_size={}, file={}", task_id, src_file_size, target_file_size, target_file.string());
						if (src_file_size == target_file_size) { // to do 先校验下大小，后面再考虑校验md5
							YK_LOGI("[Android-Upload][{}] RESULT=VERIFY_OK", task_id);
							need_upload_cb = true;
							upload_cb_state = FileUploadTask::EFileUploadState::kSuccess;
						}
						else {
							YK_LOGE("[Android-Upload][{}] RESULT=VERIFY_FAIL src_size={}, target_size={}", task_id, src_file_size, target_file_size);
							need_upload_cb = true;
							upload_cb_state = FileUploadTask::EFileUploadState::kFailedVerify;
						}
						break;
					}
					case tc::FileTransDataPacket::kError: // 一般是对端读文件异常了
						YK_LOGW("[Android-Upload][{}] REMOTE_READ_ERROR index={}", task_id, index);
						// FIX-1 修复：删除半成品目标文件（句柄已在上面 Close）。只清理不回调，对端已知自己读错误。
						need_remove_target = true;
						remove_target_path = id_with_upload_task_[task_id]->target_file_path_;
						break;
					case tc::FileTransDataPacket::kCancel:
						YK_LOGI("[Android-Upload][{}] CANCEL_RECEIVED index={}", task_id, index);
						// FIX-1 修复：删除半成品目标文件（句柄已在上面 Close），并回一个明确终态给 PC。
						// EFileUploadState 有 kCancel，但 proto FileTransRespUpload.UploadErrorCause
						// 无取消枚举，call_upload_callback 中走 default -> kUnknow（不改 proto 前提下最接近的失败态）。
						need_remove_target = true;
						remove_target_path = id_with_upload_task_[task_id]->target_file_path_;
						need_upload_cb = true;
						upload_cb_state = FileUploadTask::EFileUploadState::kCancel;
						break;
					default:
						break;
					}
				}
			}

			// FIX-6：以下为解锁后执行的延迟动作（发送 ACK / 回调 / 删除半成品文件）。
			// 顺序与原锁内触发顺序一致：先 ACK，再任务创建上报，再上传结果回调。
			if (need_send_ack) {
				// [DIAG] 每 100 包回 ACK，记录已收 index，便于与 PC 发送侧 SENT 行对齐。
				YK_LOGI("[Android-Upload][{}] ACK_SENT index={}, state={}", task_id, ack_index, static_cast<int>(ack_state));
				auto message = std::make_shared<tc::Message>();
				message->set_type(tc::kFileTransDataPacketResponse);
				auto response = new FileTransDataPacketResponse();
				response->set_task_id(task_id);
				response->set_index(ack_index);
				message->set_allocated_file_trans_data_packet_response(response);
				send_file_trans_data_packet_response_func_(stream_id, message);
			}
			if (need_task_created_cb) {
				upload_task_created_func_(task_id, device_id, src_file_path, target_file_path);
			}
			if (need_upload_cb) {
				call_upload_callback(stream_id, task_id, upload_cb_state);
			}
			if (need_remove_target && !remove_target_path.empty()) {
				// FIX-1 修复：带 error_code 版本删除半成品文件，失败仅记日志。
				std::error_code remove_ec;
				std::filesystem::remove(remove_target_path, remove_ec);
				if (remove_ec) {
					YK_LOGW("[Android-Upload][{}] REMOVE_PARTIAL_FAIL target={}, ec={}", task_id, remove_target_path, remove_ec.message());
				}
				else {
					YK_LOGI("[Android-Upload][{}] REMOVE_PARTIAL_OK target={}", task_id, remove_target_path);
				}
			}
		}
		catch (std::exception& e) {
			std::string s = e.what();
			YK_LOGE("[Android-Upload][{}] EXCEPTION what={}", task_id, s);
			{
				std::lock_guard<std::mutex> lck{ id_with_upload_task_mutex_ };
				if (id_with_upload_task_.count(task_id) > 0) {
					if (id_with_upload_task_[task_id]->file_ptr_) {
						id_with_upload_task_[task_id]->file_ptr_->Close();
					}
					id_with_upload_task_[task_id]->is_ended_ = true;
				}
			}
			// FIX-6：异常路径的回调同样移到锁外执行。
			if (!task_id.empty()) {
				call_upload_callback(stream_id, task_id, FileUploadTask::EFileUploadState::kUnknownError);
			}
		}
	}

	void FileTransmitImpl::call_upload_callback(const std::string& stream_id, const std::string& task_id, FileUploadTask::EFileUploadState state) {
		if (!upload_resp_func_) {
			YK_LOGE("FileTransmitImpl upload_callback_ is null.");
			return;
		}
		auto resp_upload = new tc::FileTransRespUpload();
		resp_upload->set_task_id(task_id);
		resp_upload->set_res(false);
		switch (state)
		{
		case tc::FileUploadTask::EFileUploadState::kUnknownError:
			resp_upload->set_error_cause(tc::FileTransRespUpload::kUnknow);
			YK_LOGI("[Android-Upload][{}] RESP state=kUnknownError", task_id);
			break;
		case tc::FileUploadTask::EFileUploadState::kFailedOpen:
			resp_upload->set_error_cause(tc::FileTransRespUpload::kFailedOpen);
			YK_LOGI("[Android-Upload][{}] RESP state=kFailedOpen", task_id);
			break;
		case tc::FileUploadTask::EFileUploadState::kFailedWrite:
			resp_upload->set_error_cause(tc::FileTransRespUpload::kFailedWrite);
			YK_LOGI("[Android-Upload][{}] RESP state=kFailedWrite", task_id);
			break;
		case tc::FileUploadTask::EFileUploadState::kDirFailedCreate:
			resp_upload->set_error_cause(tc::FileTransRespUpload::kDirFailedCreate);
			YK_LOGI("[Android-Upload][{}] RESP state=kDirFailedCreate", task_id);
			break;
		case tc::FileUploadTask::EFileUploadState::kFailedVerify:
			resp_upload->set_error_cause(tc::FileTransRespUpload::kFailedVerify);
			YK_LOGI("[Android-Upload][{}] RESP state=kFailedVerify", task_id);
			break;
		case tc::FileUploadTask::EFileUploadState::kPacketLoss:
			resp_upload->set_error_cause(tc::FileTransRespUpload::kPacketLoss);
			YK_LOGI("[Android-Upload][{}] RESP state=kPacketLoss", task_id);
			break;
		case tc::FileUploadTask::EFileUploadState::kSuccess:
			resp_upload->set_res(true);
			YK_LOGI("[Android-Upload][{}] RESP state=kSuccess", task_id);
			break;
		default:
			resp_upload->set_error_cause(tc::FileTransRespUpload::kUnknow);
			YK_LOGI("[Android-Upload][{}] RESP state=unknown({})", task_id, static_cast<int>(state));
			break;
		}
		upload_resp_func_(stream_id, resp_upload);
	}

	// 有异常的时候才会调用call_download_callback， 没有异常的话 直接发送下载数据包了
	void FileTransmitImpl::call_download_callback(const std::string& device_id, const std::string& stream_id, const std::string& task_id, FileDownloadTask::EFileDownloadState state) {
		// BUG-6 修复：download_except_func_ 是裸 std::function，注册前触发会抛 std::bad_function_call。
		// 与 call_upload_callback 的判空保持一致。
		if (!download_except_func_) {
			YK_LOGE("FileTransmitImpl download_except_func_ is null.");
			return;
		}
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
		// FIX-3 修复：file_transmit_task_with_simple_state_ 原只插不删，长连接下缓慢泄漏。
		// 用 RAII 保证所有 return/break/异常路径都在函数结束时 erase 该 task_id；
		// erase 必然发生在发送循环退出之后，不会与循环内 operator[] 冲突
		//（避免 erase 后被 operator[] 重插 kNormal，OnConnectionLost 的置 kOppositeEndError 逻辑不受影响）。
		// FIX-5 配套：task_id_with_recved_index_ 同样在此 erase（循环内只用 find 访问，
		// 迟到 ACK 由 HandleFileTransDataPacketResponse 的 find 守卫忽略，不会重插）。
		std::shared_ptr<void> auto_erase_task_state{ nullptr, [this, task_id](void*) {
			{
				std::lock_guard<std::mutex> lck{ file_transmit_mutex_ };
				file_transmit_task_with_simple_state_.erase(task_id);
			}
			{
				std::lock_guard<std::mutex> lck{ grant_token_mutex_ };
				task_id_with_recved_index_.erase(task_id);
			}
		} };
		// BUG-3/BUG-4：本次下载开始时标记链路可用，清除上次断连遗留的 false。
		connection_active_.store(true);
		// [DIAG] 下载发送侧：记录任务起始时间戳，用于全程耗时与速率统计。
		const uint64_t download_start_ms = yk::TimeUtil::GetCurrentTimestamp();
		try {
			const std::size_t buffer_size = kSingleBufferSize;
			char buffer[buffer_size] = { 0, };

            namespace fs = std::filesystem;
            fs::path download_path_fs(download_path);
            if (!fs::exists(download_path_fs)) {
                YK_LOGE("[Android-Download][{}] NO_EXISTS path={}", task_id, download_path);
                call_download_callback(device_id, stream_id, task_id, tc::FileDownloadTask::EFileDownloadState::kNoExists);
                return;
            }
            uint64_t file_size = fs::file_size(download_path_fs);
            YK_LOGI("[Android-Download][{}] START device_id={}, download_path={}, save_path={}, file_size={}, speed_by_bit_per_1000ms_={}", task_id, device_id, download_path, save_path, file_size, speed_by_bit_per_1000ms_);

#ifdef _WIN32
            std::wstring wpath = download_path_fs.wstring();
            FILE* pf = _wfopen(wpath.c_str(), L"rb");
#else
            FILE* pf = fopen(download_path.c_str(), "rb");
#endif
			if (!pf) {
				YK_LOGE("[Android-Download][{}] FILE_OPEN_FAIL path={}", task_id, download_path);
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
			// FIX-5 修复：冷启动背压窗口。循环开始前初始化对端已确认序号为 0，
			// 使背压从第 0 包起受控（否则首个 ACK 到达前 find 不到记录，背压等待不生效）。
			// 配合 FIX-3：task_id_with_recved_index_ 的该条目在 HandleFileTransDataPacketResponse 中持续更新。
			{
				std::unique_lock<std::mutex> lck{ grant_token_mutex_ };
				task_id_with_recved_index_[task_id] = 0;
			}
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
				// NEW-3 修复：lambda 捕获的 index 是 set_index(index++) 自增后的值（旧值+1），
				// 与对端 ACK 回报的实际包序号差 1。这里取实际包序号参与背压比较。
				const uint64_t sent_index = file_data_packet->index();
				std::shared_ptr<void> auto_send{ nullptr, [=, &is_send_msg, &is_abort](void* buf) {
					// BUG-9 修复：把「不发送」判断提到最前，避免取消/对端异常时仍先阻塞在令牌桶/背压等待上。
					if (!is_send_msg) {
						return;
					}

					// BUG-3 修复：令牌桶等待谓词加入 connection_active_ 短路，断连时及时退出。
					if (0 >= token_bucket_) {
						int loop_count = 0;
						while (true) {
							std::unique_lock<std::mutex> lck{ grant_token_mutex_ };
							auto res = grant_token_cv_.wait_for(lck, std::chrono::milliseconds(1), [=]() ->bool {
								if (!connection_active_.load() || token_bucket_ > 0) {
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

					// BUG-4 修复（最小加固，保留 %100 ACK）：背压谓词用 find 避免 operator[] 误插入 0；
					// 用 sent_index 修 off-by-one；加 connection_active_ 短路。
					{
						bool need_wait = false;
						{
							std::unique_lock<std::mutex> lck{ grant_token_mutex_ };
							auto it = task_id_with_recved_index_.find(task_id);
							if (it != task_id_with_recved_index_.end()) {
								YK_LOGW("sent_index - recved_index = {}", sent_index - it->second);
								if (sent_index - it->second >= 180) {
									need_wait = true;
								}
							}
						}
						if (need_wait) {
							// FIX-4 修复：原背压等待 wait_for(1ms) 最多约 10 次（~10ms）就放行，背压形同虚设。
							// 改为单次 wait_for(50ms, 谓词)，总等待上限 5 秒（循环 100 次）；谓词不变
							//（!connection_active_ 短路 + sent_index - recved < 180）。
							int loop_count = 0;
							while (true) {
								std::unique_lock<std::mutex> lck{ grant_token_mutex_ };
								auto res = grant_token_cv_.wait_for(lck, std::chrono::milliseconds(50), [=]() ->bool {
									if (!connection_active_.load()) {
										return true;
									}
									auto it = task_id_with_recved_index_.find(task_id);
									if (it == task_id_with_recved_index_.end() || sent_index - it->second < 180) {
										return true;
									}
									return false;
								});
								if (res || loop_count >= 100) {
									break;
								}
								++loop_count;
							}
						}
					}

					if (!connection_active_.load()) {
						YK_LOGW("[Android-Download][{}] connection inactive, skip send. index={}", task_id, sent_index);
						return;
					}
					if (!send_data_packet_func_) {
						YK_LOGE("[Android-Download][{}] send_data_packet_func_ null, abort. index={}", task_id, sent_index);
						is_abort = true;
						return;
					}
					if (!send_data_packet_func_(stream_id, msg)) {
						is_abort = true;
						YK_LOGE("[Android-Download][{}] SEND_FAIL(TIMEOUT) index={}, sent={}B/{}, token={}", task_id, sent_index, statistics_readed_size, file_size, token_bucket_.load());
						return;
					}
					// [DIAG] 每 100 包记录一次发送成功 + 对端已确认序号，便于判断背压/速率。
					if (sent_index % 100 == 0) {
						uint64_t acked = 0;
						{
							std::unique_lock<std::mutex> lck{ grant_token_mutex_ };
							auto it = task_id_with_recved_index_.find(task_id);
							if (it != task_id_with_recved_index_.end()) acked = it->second;
						}
						YK_LOGI("[Android-Download][{}] SENT index={}, in_flight={}, token={}, acked_by_remote={}", task_id, sent_index, sent_index - acked, token_bucket_.load(), acked);
					}
					// BUG-3 修复：安全消费令牌，避免超时退出后仍 -- 致令牌桶走负。
					TryConsumeToken();
					//std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}};
				{ // 判断任务是否被取消 或者 对端发生了异常
					std::lock_guard<std::mutex> lck{ file_transmit_mutex_ };
					if (EFileTransmitTaskSimpleState::kNormal != file_transmit_task_with_simple_state_[task_id]) {
						is_send_msg = false;
						uint64_t elapsed_ms = yk::TimeUtil::GetCurrentTimestamp() - download_start_ms;
						YK_LOGW("[Android-Download][{}] ABORT(cancel/error), sent={}B/{}, elapsed={}ms, download_path={}", task_id, statistics_readed_size, file_size, elapsed_ms, download_path);
						return;
					}
				}
				std::size_t readed_size = fread(buffer, 1, buffer_size, pf);
				if (readed_size > 0) {
					statistics_readed_size += readed_size;
					file_data_packet->set_data(buffer, readed_size);
					// [DIAG] 每 500 包打一次进度，含已发字节/总大小/累计耗时/瞬时速率。
					if (index % 500 == 0) {
						uint64_t elapsed_ms = yk::TimeUtil::GetCurrentTimestamp() - download_start_ms;
						// FIX-10 修复（与 PC 侧对称）：原 (elapsed_ms / 1000) 不足 1 秒时为 0，除零崩溃。
						// bits/ms 数值上等于 kbit/s，直接用毫秒做除数。
						uint64_t rate_kbps = (elapsed_ms > 0) ? (statistics_readed_size * 8 / elapsed_ms) : 0;
						YK_LOGI("[Android-Download][{}] PROGRESS index={}, sent={}B/{}, elapsed={}ms, rate={}kbps", task_id, index, statistics_readed_size, file_size, elapsed_ms, rate_kbps);
					}
					if (feof(pf)) { // 文件结束
						uint64_t elapsed_ms = yk::TimeUtil::GetCurrentTimestamp() - download_start_ms;
						YK_LOGI("[Android-Download][{}] READ_END index={}, sent={}B/{}, elapsed={}ms", task_id, index, statistics_readed_size, file_size, elapsed_ms);
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
						uint64_t elapsed_ms = yk::TimeUtil::GetCurrentTimestamp() - download_start_ms;
						YK_LOGI("[Android-Download][{}] READ_END(empty fread) index={}, sent={}B/{}, elapsed={}ms", task_id, index, statistics_readed_size, file_size, elapsed_ms);
						file_data_packet->set_transmit_state(tc::FileTransDataPacket::kEnd);

                        // 下载正常结束
                        if (download_end_func_) {
                            download_end_func_(task_id, device_id, stream_id, download_path);
                        }
					}
					else {
						file_data_packet->set_transmit_state(tc::FileTransDataPacket::kError);
						YK_LOGE("[Android-Download][{}] READ_ERROR path={}, index={}, sent={}B/{}", task_id, download_path, index, statistics_readed_size, file_size);
						// FIX-2 修复：读文件失败原只发 kError 包不回调，PC 侧收不到明确终态。
						// 这里补回调通知 PC。EFileDownloadState::kFailedRead 可表达"读文件失败"，
						// 但 proto FileTransRespDownload.ErrorCause 无对应枚举，call_download_callback
						// 中走 default 映射为 kUnknow（不改 proto 前提下最接近的失败态）。
						// kError 包仍由本轮 auto_send 析构发出，保持不变。
						call_download_callback(device_id, stream_id, task_id, tc::FileDownloadTask::EFileDownloadState::kFailedRead);
						return;
					}
					break;
				}
			} // end while
		}
		catch (std::exception& e) {
			std::string s = e.what();
			YK_LOGE("[Android-Download][{}] EXCEPTION what={}, path={}", task_id, s, download_path);
		}
	}

	void FileTransmitImpl::HandleSaveFileException(const std::string& stream_id, tc::FileTransSaveFileException save_exception) {
		auto error_cause = save_exception.error_cause();
		auto src_file_path = save_exception.src_file_path();
		auto target_file_path = save_exception.target_file_path();
		auto task_id = save_exception.task_id();
		// [DIAG] 收到对端(PC 下载接收侧)上报的保存异常/取消。这是对端失败原因的直接来源。
		// 原 LOGD 默认不输出，这里提升到 LOGW 并加结构化前缀，确保失败原因可见。
		YK_LOGW("[Android-Download][{}] SAVE_FILE_EXCEPTION src={}, target={}, cause={}",
			task_id, src_file_path, target_file_path, static_cast<int>(error_cause));
		switch (error_cause)
		{
		case tc::FileTransSaveFileException::kFailedOpen:
			YK_LOGW("[Android-Download][{}] cause=kFailedOpen", task_id);
			break;
		case tc::FileTransSaveFileException::kFailedWrite:
			YK_LOGW("[Android-Download][{}] cause=kFailedWrite", task_id);
			break;
		case tc::FileTransSaveFileException::kCancel:
			YK_LOGW("[Android-Download][{}] cause=kCancel", task_id);
			break;
		case tc::FileTransSaveFileException::kDirFailedCreate:
			YK_LOGW("[Android-Download][{}] cause=kDirFailedCreate", task_id);
			break;
		case tc::FileTransSaveFileException::kPacketLoss:
			YK_LOGW("[Android-Download][{}] cause=kPacketLoss (PC side detected packet loss)", task_id);
			break;
		case tc::FileTransSaveFileException::kUnknow:
			YK_LOGW("[Android-Download][{}] cause=kUnknow", task_id);
			break;
		default:
			YK_LOGW("[Android-Download][{}] cause=unknown", task_id);
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

	void FileTransmitImpl::HandleCancelTransmit(const std::string& task_id, bool is_download) {
		if (is_download) {
			// FIX-7 修复：仅当任务存在时置 kCancel（find 而非 operator[]，避免误插入），
			// 并唤醒可能阻塞在令牌桶/背压等待上的下载发送循环。
			bool found = false;
			{
				std::lock_guard<std::mutex> lck{ file_transmit_mutex_ };
				auto it = file_transmit_task_with_simple_state_.find(task_id);
				if (it != file_transmit_task_with_simple_state_.end()) {
					it->second = EFileTransmitTaskSimpleState::kCancel;
					found = true;
				}
			}
			YK_LOGI("[Android-Download][{}] CANCEL_TRANSMIT found={}", task_id, found);
			{
				std::unique_lock<std::mutex> lck{ grant_token_mutex_ };
				grant_token_cv_.notify_all();
			}
		}
		else {
			// FIX-7 修复：防御性处理。取消上传实际主要靠数据包 kCancel 态（HandleUpload），
			// 这里仅兜底：若任务仍存在则置 is_ended_ 并关句柄，后续到达的包会被 IGNORE_ENDED_TASK 拦截。
			std::lock_guard<std::mutex> lck{ id_with_upload_task_mutex_ };
			auto it = id_with_upload_task_.find(task_id);
			if (it != id_with_upload_task_.end()) {
				if (it->second->file_ptr_ && it->second->file_ptr_->IsOpen()) {
					it->second->file_ptr_->Close();
				}
				it->second->is_ended_ = true;
				YK_LOGI("[Android-Upload][{}] CANCEL_TRANSMIT defensive end, target={}", task_id, it->second->target_file_path_);
			}
		}
	}

	void FileTransmitImpl::OnConnectionLost() {
		YK_LOGI("[Android] OnConnectionLost: connection_active_=false, cancelling all tasks");
		// BUG-3/BUG-4：置链路不可用，让阻塞在令牌桶/背压上的下载发送循环及时退出。
		connection_active_.store(false);
		// Close all upload file handles and mark tasks ended.
		{
			std::lock_guard<std::mutex> lck(id_with_upload_task_mutex_);
			for (auto& pair : id_with_upload_task_) {
				if (pair.second->file_ptr_ && pair.second->file_ptr_->IsOpen()) {
					pair.second->file_ptr_->Close();
				}
				pair.second->is_ended_ = true;
			}
			id_with_upload_task_.clear();
		}
		// Mark all download tasks as opposite-end error so loops exit.
		// Do NOT clear the map here: HandleDownload uses operator[] which would
		// re-insert a default-constructed kNormal entry if the key is missing.
		{
			std::lock_guard<std::mutex> lck(file_transmit_mutex_);
			for (auto& pair : file_transmit_task_with_simple_state_) {
				pair.second = EFileTransmitTaskSimpleState::kOppositeEndError;
			}
		}
		// Wake up any thread waiting on token bucket or recv index.
		{
			std::unique_lock<std::mutex> lck(grant_token_mutex_);
			grant_token_cv_.notify_all();
		}
	}

	void FileTransmitImpl::HandleFileTransDataPacketResponse(const std::string& stream_id, tc::FileTransDataPacketResponse data_packet_resp) {
		uint64_t recved_index = data_packet_resp.index();
		std::string task_id = data_packet_resp.task_id();
			// [DIAG] 收到对端(PC 下载接收侧)回的 ACK。下载发送侧据此判断 in_flight、更新背压窗口。
			YK_LOGI("[Android-Download][{}] ACK_RECV recved_index={}", task_id, recved_index);
			std::unique_lock<std::mutex> lck{ grant_token_mutex_ };
			// FIX-3 配套：find 守卫——任务结束后条目已被 HandleDownload 的 RAII 守卫 erase，
			// 迟到的 ACK 不再 operator[] 重插（防泄漏），直接忽略。
			auto it = task_id_with_recved_index_.find(task_id);
			if (it == task_id_with_recved_index_.end()) {
				YK_LOGI("[Android-Download][{}] ACK_RECV_IGNORED(stale) recved_index={}", task_id, recved_index);
				return;
			}
			it->second = recved_index;
			grant_token_cv_.notify_all();
	}

	void FileTransmitImpl::GrantTokenBucket() {
		// BUG-3 修复（与 PC 侧对称）：floor 保底 +1 治低速饥饿；fetch_add 治并发 -- 丢更新；CAS 封顶防无限累积。
		int64_t grant = static_cast<int64_t>(speed_by_MB_per_100ms_ / kSingleBufferSize);
		if (grant < 1) {
			grant = 1;
		}
		token_bucket_.fetch_add(grant);
		int64_t cur = token_bucket_.load();
		while (cur > kTokenBucketCap) {
			if (token_bucket_.compare_exchange_weak(cur, kTokenBucketCap)) {
				break;
			}
		}
		std::unique_lock<std::mutex> lck{ grant_token_mutex_ };
		grant_token_cv_.notify_all();
	}

	void FileTransmitImpl::ResetTokenBucket() {
		token_bucket_ = 10;
	}

	bool FileTransmitImpl::TryConsumeToken() {
		// BUG-3 修复：仅当 token>0 时 CAS 递减，避免超时退出后仍 -- 致令牌桶走负。
		int64_t expected = token_bucket_.load();
		while (expected > 0) {
			if (token_bucket_.compare_exchange_weak(expected, expected - 1)) {
				return true;
			}
		}
		return false;
	}

	void FileTransmitImpl::SetMaxSpeedBybitPerSecond(uint64_t speed) {
		if (0 == speed) {
			// BUG-12 修复：0 语义=不限速，而非无效输入。令牌补充量置上限，等效无节流。
			speed_by_bit_per_1000ms_ = 0;
			speed_by_MB_per_100ms_ = kMaxSpeedByMBPer100ms;
			YK_LOGI("speed_by_MB_per_100ms_ is {} (unlimited)", speed_by_MB_per_100ms_);
			return;
		}
		speed_by_bit_per_1000ms_ = speed;
		speed_by_MB_per_100ms_ = speed_by_bit_per_1000ms_ * 0.1 * 0.1 * 0.85;
		if (speed_by_MB_per_100ms_ > kMaxSpeedByMBPer100ms) {
			speed_by_MB_per_100ms_ = kMaxSpeedByMBPer100ms;
		}
		YK_LOGI("speed_by_MB_per_100ms_ is {}", speed_by_MB_per_100ms_);
	}

	uint64_t FileTransmitImpl::GetMaxSpeedBybitPerSecond() {
		return speed_by_bit_per_1000ms_;
	}
}
