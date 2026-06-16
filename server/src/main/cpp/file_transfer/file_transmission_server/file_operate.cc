#include "file_operate.h"

#ifdef WIN32
#include <Windows.h>
#include <Knownfolders.h>
#include <shlobj_core.h>
#include <wtsapi32.h>
#pragma comment(lib, "Wtsapi32.lib")
#endif

#include <filesystem>
#include <sys/stat.h>
#include <chrono>
#include "cpp_base_lib/yk_logger.h"
#include "mirror_message.pb.h"

namespace tc {
	static std::string s_file_permission_path_ = "/";

	FileOperate::FileOperate() {}

	std::vector<tc::FileDescInfo> FileOperate::GetFilesListImpl(const std::string& path) {
		namespace fs = std::filesystem;
		std::vector<tc::FileDescInfo> file_infos;
		fs::path dir_path(path);
		std::error_code ec;

		YK_LOGI("[Android] GetFilesListImpl START path={}", path);
		if (!fs::exists(dir_path, ec) || !fs::is_directory(dir_path, ec)) {
			YK_LOGI("[Android] GetFilesListImpl path not exists or not dir, path={}, ec={}", path, ec.message());
			return file_infos;
		}

		int entry_count = 0;
		int skip_count = 0;
		for (const auto& entry : fs::directory_iterator(dir_path, fs::directory_options::skip_permission_denied, ec)) {
			if (ec) {
				YK_LOGI("[Android] GetFilesListImpl iterator ec, path={}, ec={}", path, ec.message());
				ec.clear();
				continue;
			}
			entry_count++;
			tc::FileDescInfo info;
			const fs::path& p = entry.path();
			info.set_name(p.filename().string());
			info.set_path(fs::absolute(p, ec).string());
			if (ec) {
				YK_LOGI("[Android] GetFilesListImpl absolute failed, name={}, ec={}", info.name(), ec.message());
				ec.clear();
				continue;
			}

			struct stat st{};
			if (stat(info.path().c_str(), &st) != 0) {
				YK_LOGI("[Android] GetFilesListImpl stat failed, name={}, path={}", info.name(), info.path());
				skip_count++;
				continue;
			}

			if (entry.is_directory()) {
				info.set_type(tc::FileDescInfo::kFolder);
				if (!desktop_path_.empty() && info.path() == desktop_path_) {
					info.set_type(tc::FileDescInfo::kDeskFolder);
				}
			}
			else if (entry.is_regular_file()) {
				info.set_type(tc::FileDescInfo::kFile);
				info.set_size(static_cast<uint64_t>(st.st_size));
				info.set_date(static_cast<uint64_t>(st.st_mtim.tv_sec));
			}
			else {
				skip_count++;
				continue;
			}
			file_infos.emplace_back(std::move(info));
		}
		YK_LOGI("[Android] GetFilesListImpl END path={}, entry_count={}, valid_count={}, skip_count={}", path, entry_count, file_infos.size(), skip_count);
		return file_infos;
	}

	std::tuple<bool, std::vector<tc::FileDescInfo>, std::string, std::string> FileOperate::GetFilesList(std::string path) {
		try {
			std::string permission_log;
			namespace fs = std::filesystem;
			fs::path visit_path(path);
			std::error_code ec;

			YK_LOGI("[Android] GetFilesList called path={}", path);
			if (!fs::exists(visit_path, ec)) {
				YK_LOGI("[Android] GetFilesList path not exists, path={}", path);
				return { false, {}, permission_log + "The accessed directory no longer exists", s_file_permission_path_ };
			}
			if (!fs::is_directory(visit_path, ec)) {
				YK_LOGI("[Android] GetFilesList path not dir, path={}", path);
				return { false, {}, permission_log + "The accessed path is not a valid folder or disk directory", s_file_permission_path_ };
			}

			std::vector<tc::FileDescInfo> file_infos = GetFilesListImpl(path);
			YK_LOGI("[Android] GetFilesList returning path={}, count={}", path, file_infos.size());
			return { true, std::move(file_infos), permission_log, s_file_permission_path_ };
		}
		catch (std::exception& e) {
			YK_LOGE("[Android] GetFilesList EXCEPTION path={}, error={}", path, e.what());
			return { false, {}, e.what(), s_file_permission_path_ };
		}
	}

	std::tuple<bool, std::vector<tc::FileDescInfo>, std::string, std::string> FileOperate::RecursiveGetFilesList(std::string path) {
		try {
			std::string permission_log;
			namespace fs = std::filesystem;
			std::error_code ec;
			fs::path visit_path(path);

			if (!fs::exists(visit_path, ec)) {
				return { false, {}, permission_log + "The accessed directory no longer exists", s_file_permission_path_ };
			}
			if (!fs::is_directory(visit_path, ec)) {
				return { false, {}, permission_log + "The accessed path is not a valid folder or disk directory", s_file_permission_path_ };
			}

			if (root_path_ == path) {
				std::vector<tc::FileDescInfo> file_infos = GetFilesListImpl(path);
				return { true, std::move(file_infos), permission_log, s_file_permission_path_ };
			}

			std::vector<std::string> folders;
			std::vector<std::string> files;
			TraverseDirectory(path, folders, files);
			std::vector<tc::FileDescInfo> file_infos;
			file_infos.reserve(folders.size() + files.size());

			for (const auto& folder : folders) {
				fs::path p(folder);
				tc::FileDescInfo info;
				info.set_type(tc::FileDescInfo::kFolder);
				info.set_name(p.filename().string());
				info.set_path(fs::absolute(p, ec).string());
				info.set_size(0);
				info.set_date(0);
				file_infos.emplace_back(std::move(info));
			}

			for (const auto& file : files) {
				fs::path p(file);
				tc::FileDescInfo info;
				info.set_type(tc::FileDescInfo::kFile);
				info.set_name(p.filename().string());
				info.set_path(fs::absolute(p, ec).string());
				info.set_size(fs::file_size(p, ec));
				auto ftime = fs::last_write_time(p, ec);
				if (!ec) {
					auto secs = std::chrono::time_point_cast<std::chrono::seconds>(ftime).time_since_epoch().count();
					info.set_date(static_cast<uint64_t>(secs));
				}
				file_infos.emplace_back(std::move(info));
			}
			return { true, std::move(file_infos), "", s_file_permission_path_ };
		}
		catch (const std::exception& e) {
			YK_LOGE("RecursiveGetFilesList path is {}, error is {}", path, e.what());
			return { false, {}, e.what(), s_file_permission_path_ };
		}
	}

	void FileOperate::TraverseDirectory(const std::string& path, std::vector<std::string>& folders, std::vector<std::string>& files) {
		namespace fs = std::filesystem;
		std::error_code ec;

		for (const auto& entry : fs::directory_iterator(path, fs::directory_options::skip_permission_denied, ec)) {
			if (ec) continue;
			if (entry.is_symlink(ec)) continue;

			fs::path abs_path = fs::absolute(entry.path(), ec);
			if (ec) continue;

			if (entry.is_directory(ec)) {
				folders.emplace_back(abs_path.string());
				TraverseDirectory(abs_path.string(), folders, files);
			}
			else if (entry.is_regular_file(ec)) {
				files.emplace_back(abs_path.string());
			}
		}
	}

	std::tuple<bool, std::string> FileOperate::CreateFolder(std::string folder_path) {
		bool ret = true;
		std::string err_msg;
		try {
			namespace fs = std::filesystem;
			std::error_code ec;
			fs::path target_path(folder_path);

			if (!fs::exists(target_path, ec)) {
				if (!fs::create_directories(target_path, ec)) {
					ret = false;
					err_msg = "Failed to create directory: " + folder_path;
				}
			}

			if (!fs::exists(target_path, ec)) {
				ret = false;
				if (err_msg.empty()) {
					err_msg = "Directory does not exist after creation: " + folder_path;
				}
			}
		}
		catch (const std::exception& e) {
			ret = false;
			err_msg = e.what();
			YK_LOGE("CreateFolder folder_path is {}, error is {}", folder_path, err_msg);
		}
		return { ret, err_msg };
	}

	std::tuple<bool, std::vector<std::string>, std::string> FileOperate::Remove(const std::vector<std::string>& paths) {
		bool ret = true;
		std::vector<std::string> err_paths;
		std::string err_msg;
		namespace fs = std::filesystem;

		for (const auto& ph : paths) {
			try {
				std::error_code ec;
				fs::path p(ph);

				if (!fs::exists(p, ec)) {
					continue;
				}

				bool removed = false;

				if (fs::is_directory(p, ec)) {
					fs::remove_all(p, ec);
					removed = !fs::exists(p, ec);
				}
				else if (fs::is_regular_file(p, ec)) {
					removed = fs::remove(p, ec);
				}
				else {
					removed = fs::remove(p, ec);
				}

				if (!removed || ec) {
					ret = false;
					err_paths.emplace_back(ph);
					err_msg = ec ? ec.message() : "remove failed";
				}
			}
			catch (const std::exception& e) {
				ret = false;
				err_paths.emplace_back(ph);
				err_msg = e.what();
				YK_LOGE("Remove error, remove {}, failed is {}.", ph, err_msg);
			}
		}

		return { ret, err_paths, err_msg };
	}

	std::tuple<bool, std::string, std::string> FileOperate::CreateNewFolder(const std::string& parent_path_str) {
		try {
			namespace fs = std::filesystem;
			std::error_code ec;
			fs::path parent_path(parent_path_str);

			if (!fs::exists(parent_path, ec) || !fs::is_directory(parent_path, ec)) {
				return { false, "", parent_path_str + " path does not exist" };
			}

			int temp_count = 1;
			std::string prefix = "new_folder";
			fs::path created_path;
			bool created = false;

			while (true) {
				std::string suffix;
				if (temp_count == 1) {
					suffix = "";
				} else {
					suffix = "(" + std::to_string(temp_count) + ")";
				}

				std::string folder_name = prefix + suffix;
				fs::path target_path = parent_path / folder_name;

				if (fs::exists(target_path, ec)) {
					++temp_count;
					continue;
				}

				if (fs::create_directory(target_path, ec)) {
					created = true;
					created_path = target_path;
				}
				break;
			}

			return { created, created ? created_path.string() : "", "" };
		}
		catch (const std::exception& e) {
			std::string s = e.what();
			YK_LOGE("FileOperate::CreateNewFolder parent_path is {} error is {}", parent_path_str, s);
			return { false, "", "Create new folder failed: " + s };
		}
	}

	std::tuple<bool, uint64_t, uint64_t> FileOperate::IsExists(const std::string& u8_path) {
		try {
			namespace fs = std::filesystem;
			std::error_code ec;
			fs::path p(u8_path);

			if (!fs::exists(p, ec)) {
				return { false, 0, 0 };
			}

			uint64_t file_size = 0;
			uint64_t date_changed = 0;

			if (fs::is_regular_file(p, ec)) {
				file_size = fs::file_size(p, ec);
			}

			auto ftime = fs::last_write_time(p, ec);
			if (!ec) {
				auto secs = std::chrono::time_point_cast<std::chrono::seconds>(ftime).time_since_epoch().count();
				date_changed = static_cast<uint64_t>(secs);
			}

			return { true, file_size, date_changed };
		}
		catch (const std::exception& e) {
			std::string s = e.what();
			YK_LOGE("FileOperate::IsExists error is {}", s);
			return { false, 0, 0 };
		}
	}

	std::tuple<bool, std::string, std::string> FileOperate::Rename(const std::string& u8_old_path, const std::string& u8_new_name) {
		try {
			namespace fs = std::filesystem;
			std::error_code ec;
			fs::path old_path(u8_old_path);

			if (!fs::exists(old_path, ec)) {
				return { false, "", "Target path does not exist" };
			}

			fs::path new_name(u8_new_name);
			if (new_name.has_parent_path()) {
				return { false, "", "Rename failed: new name contains path" };
			}

			fs::path new_path = old_path.parent_path() / new_name;

			if (fs::exists(new_path, ec)) {
				return { false, "", "Name already exists" };
			}

			fs::rename(old_path, new_path, ec);
			if (ec) {
				return { false, "", "Rename failed" };
			}

			return { true, new_path.string(), "" };
		}
		catch (const std::exception& e) {
			std::string s = e.what();
			YK_LOGE("FileOperate::Rename error is {}", s);
			return { false, "", "Rename failed: " + s };
		}
	}
}
