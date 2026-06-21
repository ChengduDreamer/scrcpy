#pragma once
#include <string>
#include <fstream>
#include <map>
#include <mutex>
#include <atomic>
#include <functional>
#include <condition_variable>
#include <asio2/asio2.hpp>
#include "mirror_message.pb.h"
#include "cpp_base_lib/file.h"


namespace tc {

    constexpr uint64_t kSingleBufferSize = 1024 * 4;

	class FileUploadTask {
	public:
		enum class EFileUploadState {
			kNoStart, //未开始
			kUploading, 
			kUnknownError, // 未知错误
			kFailedOpen, // 打开文件失败
			kFailedWrite,    // 写文件出错
			kCancel,         // 用户取消
			kDirFailedCreate,// 目录创建失败
			kFailedVerify,   // 校验失败
			kSuccess,        // 上传成功
			kPacketLoss,     // 发生了丢包
		};
		std::string task_id_;
		std::string src_file_path_;
		std::string target_file_path_;
		std::shared_ptr<yk::File> file_ptr_;
		std::atomic<bool> is_ended_ = false;
		std::atomic<uint64_t> last_update_time_ = 0;
		uint64_t current_packet_index_ = 0; 
	};

	class FileDownloadTask {
	public:
		enum class EFileDownloadState {
			kNoStart, //未开始
			kDownloading,
			kUnknownError, // 未知错误
			kNoExists,     // 文件不存在
			kFailedOpen,   // 打开文件失败
			kFailedRead,   // 读文件出错
			kCancel,       // 用户取消
			kEnd,          // 读取完成
		};
	};

	class FileTransmitImpl {
	public:
		FileTransmitImpl();
		~FileTransmitImpl();

		using UploadResponseFuncType = std::function<void(const std::string& stream_id, tc::FileTransRespUpload*)>;

        // 开始下载
        using DownloadBeginFuncType = std::function<void(const std::string& task_id, const std::string& device_id, const std::string& stream_id, const std::string& file_path)>;
        // 下载结束
        using DownloadEndFuncType = std::function<void(const std::string& task_id, const std::string& device_id, const std::string& stream_id, const std::string& file_path)>;
        // 用户取消下载
        using DownloadCanceledByUserFuncType = std::function<void(const std::string& task_id, const std::string& device_id, const std::string& stream_id)>;
        // 下载失败
		using DownloadExceptFuncType = std::function<void(const std::string& task_id, const std::string& device_id, const std::string& stream_id, tc::FileTransRespDownload*)>;

		using SendDataPacketFuncType = std::function<bool(const std::string& stream_id, std::shared_ptr<tc::Message>)>;

		using SendFileTransDataPacketResponseFuncType = std::function<bool(const std::string& stream_id, std::shared_ptr<tc::Message>)>;

        //
        using OnUploadTaskCreatedType = std::function<void(const std::string& task_id, const std::string& device_id, const std::string& src_path, const std::string& dst_path)>;

		void HandleUpload(const std::string& device_id, const std::string& stream_id, tc::FileTransDataPacket data_packet);

		void HandleDownload(const std::string& device_id, const std::string& stream_id, const std::string& download_path, const std::string& save_path, const std::string& task_id);
		
		void HandleSaveFileException(const std::string& stream_id, tc::FileTransSaveFileException data_packet);

		void HandleFileTransDataPacketResponse(const std::string& stream_id, tc::FileTransDataPacketResponse data_packet_resp);

		void OnConnectionLost();

		//将 文件流 对象保存起来, 因为要持续写文件
		std::mutex id_with_upload_task_mutex_;
		std::map<std::string, std::shared_ptr<FileUploadTask>> id_with_upload_task_;

        // 上传
		UploadResponseFuncType upload_resp_func_ = nullptr;
        // 下载
        DownloadBeginFuncType download_begin_func_ = nullptr;
        DownloadEndFuncType  download_end_func_ = nullptr;
        DownloadCanceledByUserFuncType download_canceled_func_ = nullptr;
		DownloadExceptFuncType download_except_func_ = nullptr;

		SendDataPacketFuncType send_data_packet_func_ = nullptr;

		SendFileTransDataPacketResponseFuncType send_file_trans_data_packet_response_func_ = nullptr;

        OnUploadTaskCreatedType upload_task_created_func_ = nullptr;

		void SetMaxSpeedBybitPerSecond(uint64_t speed);

		uint64_t GetMaxSpeedBybitPerSecond();
	private:
		void call_upload_callback(const std::string& stream_id, const std::string& task_id, FileUploadTask::EFileUploadState state);

		// 有异常的时候才会调用call_download_callback， 没有异常的话 直接发送下载数据包了
		void call_download_callback(const std::string& device_id, const std::string& stream_id, const std::string& task_id, FileDownloadTask::EFileDownloadState state);

		enum class EFileTransmitTaskSimpleState {
			kNormal,
			kCancel,            //用户取消
			kOppositeEndError,  //对端异常
		};
		// task_id  任务
		static std::mutex file_transmit_mutex_;
		static std::map<std::string, EFileTransmitTaskSimpleState> file_transmit_task_with_simple_state_;

		// 定时检测
		void On6000msTimer();
		int AddTimer(const std::chrono::milliseconds& duration, const std::function<void()>& func) {
			asio_timer_.start_timer(++next_timer_id_, duration, func);
			return next_timer_id_.load();
		}
		void StopTimer(int timer_id) {
			asio_timer_.stop_timer(timer_id);
		}
		std::atomic_int next_timer_id_ = -1;
		asio2::iopool io_ctx_;
		asio2::timer asio_timer_;


		// 限速设计:
		//const uint64_t single_buffer_size_ = 1024 * 64;
		// MB / 100ms
		const uint64_t kMaxSpeedByMBPer100ms = 4 * 1 * 1000 * 1000;
		uint64_t speed_by_MB_per_100ms_ = 4 * 1 * 1000 * 1000;

		uint64_t speed_by_bit_per_1000ms_ = 10 * 50 * 1 * 1000 * 1000;

		std::atomic<int64_t> token_bucket_ = { 0 };
		// BUG-3：令牌桶上限，防止发送方停滞时令牌无限累积（大于背压窗口 180 即可）。
		static constexpr int64_t kTokenBucketCap = 2048;
		void GrantTokenBucket();
		void ResetTokenBucket();
		// BUG-3：安全消费一个令牌，仅当 token>0 时 CAS 递减，避免超时退出后仍 -- 致负值。
		bool TryConsumeToken();
		std::mutex grant_token_mutex_;
		std::condition_variable grant_token_cv_;

		// 链路是否可用（单一职责：供 token/背压等待谓词短路退出）。
		// BUG-3/BUG-4：断连或取消时置 false，让阻塞在令牌桶/背压上的下载发送循环及时退出。
		std::atomic<bool> connection_active_{true};

		std::map<std::string, uint64_t> task_id_with_recved_index_;
	};

}