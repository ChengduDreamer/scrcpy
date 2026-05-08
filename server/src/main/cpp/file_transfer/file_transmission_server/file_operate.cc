#include "file_operate.h"

#ifdef WIN32
#include <Windows.h>
#include <Knownfolders.h>
#include <shlobj_core.h>
#include <wtsapi32.h>
#endif // WIN32
#include <qfile.h>
#include <qfileinfo.h>
#include <qdir.h>
#include <qlist.h>
#include <qstorageinfo.h>
#include <qstandardpaths.h>
#include "tc_message.pb.h"
#include "tc_common_new/string_util.h"
#include "tc_common_new/log.h"
#include "tc_common_new/process_util.h"
#include "tc_label.h"

#ifdef WIN32
#pragma comment(lib, "Wtsapi32.lib")
#endif // WIN32

namespace tc {
	//暂时不限制访问路径
	static std::string s_file_permission_path_ = "/";

	FileOperate::FileOperate() {}

	std::vector<tc::FileDescInfo> FileOperate::GetFilesListImpl(const std::string& path) {
		std::vector<tc::FileDescInfo> file_infos;
		QDir dir{QString::fromStdString(path)};
		QFileInfoList fileInfoList = dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
		for (const QFileInfo& fileInfo : fileInfoList) {
			tc::FileDescInfo info;
			info.set_name(fileInfo.fileName().toStdString());
			info.set_path(fileInfo.absoluteFilePath().toStdString());
			if (fileInfo.isDir()) {
				info.set_type(tc::FileDescInfo::kFolder);
				if (desktop_path_ == fileInfo.absoluteFilePath().toStdString()) {
					info.set_type(tc::FileDescInfo::kDeskFolder);
				}
			}
			else if (fileInfo.isFile()) {
				info.set_type(tc::FileDescInfo::FileType::FileDescInfo_FileType_kFile);
				info.set_size(fileInfo.size());
				info.set_date(fileInfo.lastModified().toSecsSinceEpoch());
			}
			else {
				LOGI("Other is {}", fileInfo.absoluteFilePath().toStdString());
				continue;
			}
			file_infos.emplace_back(info);
		}
		return file_infos;
	}

	std::tuple<bool, std::vector<tc::FileDescInfo>, std::string, std::string> FileOperate::GetFilesList(std::string path) {
		// to do: At present, only the Windows system is being considered, and compatibility with Linux will be achieved later
#ifdef WIN32
		try {
			std::string permission_log;
			bool permission = true;
			path = StringUtil::StandardizeWinPath(path);
			//Determine if it is within the scope of access permissions, "/" indicates all permissions
			do {
				if ("/" != s_file_permission_path_) {
					QFileInfo visitFileInfo(QFileInfo(QString::fromStdString(path)).canonicalFilePath());
					QFileInfo filePermissionFileInfo(QFileInfo(QString::fromStdString(s_file_permission_path_)).canonicalFilePath());
					if (visitFileInfo == filePermissionFileInfo) {
						break;
					}
					QString visit_path_str = visitFileInfo.absolutePath();
					QString permission_path_str = visitFileInfo.absolutePath();
					if (visit_path_str.startsWith(permission_path_str, Qt::CaseInsensitive)) {
						permission = false;
						break;
					}
				}
			} while (0);

			if (!permission) {
				permission_log = "The accessed path is not authorized and has been switched to an authorized path The authorization path is:" + s_file_permission_path_;
				path = s_file_permission_path_;
			}
			if (root_path_ == path) { // 此电脑
				return { true, GetThisPCFiles(), "", s_file_permission_path_};
			}
			QString path_qstr = QString::fromStdString(path);
			QFileInfo visit_file_info{ path_qstr };
			if (!visit_file_info.exists()) {
				return { false, {}, permission_log + QStringLiteral("The accessed directory no longer exists").toStdString(), s_file_permission_path_};
			}

			if (!visit_file_info.isDir()) {
				return { false, {}, permission_log + QStringLiteral("The accessed path is not a valid folder or disk directory").toStdString(), s_file_permission_path_};
			}
			std::vector<tc::FileDescInfo> file_infos = GetFilesListImpl(path);
			return { true, file_infos, permission_log, s_file_permission_path_ };
		}
		catch (std::exception& e) {
			std::string s = e.what();
			LOGE("GetFilesList path is {}, error is {}", path, s);
			return { false, {}, s, s_file_permission_path_ };
		}
#else
		return { false, {}, QStringLiteral("linux file trans unsupport"), ""};
#endif // WIN32
	}

	std::tuple<bool, std::vector<tc::FileDescInfo>, std::string, std::string> FileOperate::RecursiveGetFilesList(std::string path) {
		// to do: At present, only the Windows system is being considered, and compatibility with Linux will be achieved later
#ifdef WIN32
		try {
			std::string permission_log;
			bool permission = true;
			path = StringUtil::StandardizeWinPath(path);
			//Determine if it is within the scope of access permissions, "/" indicates all permissions
			do {
				if ("/" != s_file_permission_path_) {
					QFileInfo visitFileInfo(QFileInfo(QString::fromStdString(path)).canonicalFilePath());
					QFileInfo filePermissionFileInfo(QFileInfo(QString::fromStdString(s_file_permission_path_)).canonicalFilePath());
					if (visitFileInfo == filePermissionFileInfo) {
						break;
					}
					QString visit_path_str = visitFileInfo.absolutePath();
					QString permission_path_str = visitFileInfo.absolutePath();
					if (visit_path_str.startsWith(permission_path_str, Qt::CaseInsensitive)) {
						permission = false;
						break;
					}
				}
			} while (0);

			if (!permission) {
				permission_log = "The accessed path is not authorized and has been switched to an authorized path The authorization path is:" + s_file_permission_path_;
				path = s_file_permission_path_;
			}

			if (root_path_ == path) { // 此电脑
				return { true, GetThisPCFiles(), "", s_file_permission_path_ };
			}

			QString path_qstr = QString::fromStdString(path);
			QFileInfo visit_file_info{ path_qstr };
			if (!visit_file_info.exists()) {
				return { false, {}, permission_log + QStringLiteral("The accessed directory no longer exists").toStdString(), s_file_permission_path_ };
			}

			if (!visit_file_info.isDir()) {
				return { false, {}, permission_log + QStringLiteral("The accessed path is not a valid folder or disk directory").toStdString(), s_file_permission_path_ };
			}
			
			std::vector<QString> folders;
			std::vector<QString> files;
			TraverseDirectory(QString::fromStdString(path), folders, files);
			std::vector<tc::FileDescInfo> file_infos;
			for (auto & folder : folders) {
				QFileInfo file_info{ folder };
				tc::FileDescInfo info;
				info.set_type(tc::FileDescInfo::kFolder);
				info.set_name(file_info.fileName().toStdString());
				info.set_path(file_info.absoluteFilePath().toStdString());
				file_infos.emplace_back(info);
			}

			for (auto& file : files) {
				QFileInfo file_info{ file };
				tc::FileDescInfo info;
				info.set_type(tc::FileDescInfo::kFile);
				info.set_name(file_info.fileName().toStdString());
				info.set_path(file_info.absoluteFilePath().toStdString());
				info.set_size(file_info.size());
				info.set_date(file_info.lastModified().toSecsSinceEpoch());
				file_infos.emplace_back(info);
			}
			return { true, file_infos, "", s_file_permission_path_ };
		}
		catch (std::exception& e) {
			std::string s = e.what();
			LOGE("GetFilesList path is {}, error is {}", path, s);
			return { false, {}, s, s_file_permission_path_ };
		}
#else
		return { false, {}, QStringLiteral("linux file trans unsupport"), "" };
#endif // WIN32
	}


	void FileOperate::TraverseDirectory(const QString& path, std::vector<QString>& folders, std::vector<QString>& files) {
		QDir directory(path);
		// 遍历当前目录下的所有项
		QStringList items = directory.entryList(QDir::NoDotAndDotDot | QDir::Files | QDir::Dirs | QDir::Hidden | QDir::NoSymLinks, QDir::DirsFirst);
		foreach(QString item, items) {
			QString itemPath = directory.filePath(item);

			QFileInfo fileInfo(itemPath);

			if (fileInfo.isDir()) {
				// 如果是文件夹，则递归遍历，并将路径存储到文件夹向量中
				folders.push_back(fileInfo.absoluteFilePath());
				TraverseDirectory(itemPath, folders, files);
			}
			else {
				// 如果是文件，则将路径存储到文件向量中
				files.push_back(fileInfo.absoluteFilePath());
			}
		}
	}

	std::vector<tc::FileDescInfo> FileOperate::GetThisPCFiles() {
#ifdef WIN32
		try {
			// 通过获取用户token 可以获取当前用户下的 各种目录，不然获取的就是 system32用户的目录   // to do 未登录状态下,要不要展示出system用户的目录，有待商榷
			bool impersonate = false;
			bool query_token = false;
			HANDLE hToken = nullptr;  
			// 获取活动控制台会话的会话 ID 
			// ImpersonateLoggedOnUser 函数允许调用线程模拟登录用户的安全上下文。 用户由令牌句柄表示。
			DWORD dwSessionId = ProcessUtil::GetCurrentSessionId();
			// 获取指定会话 ID 的用户令牌
			if (WTSQueryUserToken(dwSessionId, &hToken)) {
				// 动态提升权限
				if (ImpersonateLoggedOnUser(hToken)) {
					// 在此处执行以用户权限进行的操作
					impersonate = true;
				}
				else {
					LOGE("Failed to impersonate user. Error code: %d ", GetLastError());
				}
				query_token = true;
			}
			else {
				LOGE("Failed to get user token. Error code: %d", GetLastError());
			}

			std::vector<tc::FileDescInfo> file_infos;
			// 获取系统中的磁盘列表
			QList<QStorageInfo> drives = QStorageInfo::mountedVolumes();

			// 遍历磁盘列表
			for (const QStorageInfo& drive : drives) {
				qDebug() << "Root path:" << drive.rootPath(); // C:/
				tc::FileDescInfo info;
				std::string name = drive.rootPath().toStdString();
				info.set_name(name);
				info.set_path(name);
				info.set_type(tc::FileDescInfo::kDisk);
				file_infos.emplace_back(info);
			}

#if 1       //使用QT获取桌面等路径 
			// 获取桌面路径
			tc::FileDescInfo desktop_info;
			//desktop_info.set_name(QStringLiteral("桌面").toStdString());
			desktop_info.set_name(tcTr("id_file_trans_desktop").toStdString());
			desktop_info.set_path(QStandardPaths::writableLocation(QStandardPaths::DesktopLocation).toStdString());
			desktop_info.set_type(tc::FileDescInfo::kDeskFolder);
			file_infos.emplace_back(desktop_info);
			
			// 获取我的文档路径
			tc::FileDescInfo doc_info;
			doc_info.set_name(tcTr("id_file_trans_my_document").toStdString());
			doc_info.set_path(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation).toStdString());
			doc_info.set_type(tc::FileDescInfo::kFolder);
			file_infos.emplace_back(doc_info);

			// 获取我的音乐路径
			tc::FileDescInfo music_info;
			music_info.set_name(tcTr("id_file_trans_my_music").toStdString());
			music_info.set_path(QStandardPaths::writableLocation(QStandardPaths::MusicLocation).toStdString());
			music_info.set_type(tc::FileDescInfo::kFolder);
			file_infos.emplace_back(music_info);

			// 获取我的图片路径
			tc::FileDescInfo pic_info;
			pic_info.set_name(tcTr("id_file_trans_my_picture").toStdString());
			pic_info.set_path(QStandardPaths::writableLocation(QStandardPaths::PicturesLocation).toStdString());
			pic_info.set_type(tc::FileDescInfo::kFolder);
			file_infos.emplace_back(pic_info);

			// 获取我的视频路径
			tc::FileDescInfo mov_info;
			mov_info.set_name(tcTr("id_file_trans_my_video").toStdString());
			mov_info.set_path(QStandardPaths::writableLocation(QStandardPaths::MoviesLocation).toStdString());
			mov_info.set_type(tc::FileDescInfo::kFolder);
			file_infos.emplace_back(mov_info);
#endif
#if 0       // 使用win32API 获取
			// 获取桌面路径   
			PWSTR desktopPath = nullptr;
			HRESULT result = SHGetKnownFolderPath(FOLDERID_Desktop, KF_FLAG_DEFAULT, nullptr, &desktopPath);
			if (SUCCEEDED(result)) {
				std::wstring path_wstr = desktopPath;
				CoTaskMemFree(desktopPath);
				tc::FileDescInfo desktop_info;
				desktop_info.set_name(QStringLiteral("桌面").toStdString());
				desktop_info.set_path(QString::fromStdWString(path_wstr).toStdString());
				desktop_info.set_type(tc::FileDescInfo::kDeskFolder);
				file_infos.emplace_back(desktop_info);
			}
			else {
				DWORD errorCode = HRESULT_CODE(result);
				auto str = StringUtil::GetErrorStr(errorCode);
				LOGE("SHGetKnownFolderPath GetErrorStr = %s\n", str.c_str());
			}

			// 获取我的文档路径
			PWSTR documentsPath = nullptr;
			if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &documentsPath))) {
				std::wstring path_wstr = documentsPath;
				CoTaskMemFree(documentsPath);
				tc::FileDescInfo doc_info;
				doc_info.set_name(QStringLiteral("我的文档").toStdString());
				doc_info.set_path(QString::fromStdWString(path_wstr).toStdString());
				doc_info.set_type(tc::FileDescInfo::kFolder);
				file_infos.emplace_back(doc_info);
			}

			// 获取我的视频路径
			PWSTR videosPath = nullptr;
			if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, KF_FLAG_DEFAULT, nullptr, &videosPath))) {
				std::wstring path_wstr = videosPath;
				CoTaskMemFree(videosPath);
				tc::FileDescInfo mov_info;
				mov_info.set_name(QStringLiteral("我的视频").toStdString());
				mov_info.set_path(QString::fromStdWString(path_wstr).toStdString());
				mov_info.set_type(tc::FileDescInfo::kFolder);
				file_infos.emplace_back(mov_info);
			}

			// 获取我的音乐路径
			PWSTR musicPath = nullptr;
			if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Music, KF_FLAG_DEFAULT, nullptr, &musicPath))) {
				std::wstring path_wstr = musicPath;
				CoTaskMemFree(musicPath);
				tc::FileDescInfo music_info;
				music_info.set_name(QStringLiteral("我的音乐").toStdString());
				music_info.set_path(QString::fromStdWString(path_wstr).toStdString());
				music_info.set_type(tc::FileDescInfo::kFolder);
				file_infos.emplace_back(music_info);
			}

			// 获取我的图片路径
			PWSTR picturesPath = nullptr;
			if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, KF_FLAG_DEFAULT, nullptr, &picturesPath))) {
				std::wstring path_wstr = picturesPath;
				CoTaskMemFree(picturesPath);
				tc::FileDescInfo pic_info;
				pic_info.set_name(QStringLiteral("我的图片").toStdString());
				pic_info.set_path(QString::fromStdWString(path_wstr).toStdString());
				pic_info.set_type(tc::FileDescInfo::kFolder);
				file_infos.emplace_back(pic_info);
			}
#endif

			if (impersonate) { // 恢复原始身份
				RevertToSelf();
			}
			if (query_token) { // 关闭用户令牌句柄
				CloseHandle(hToken);
			}

			return file_infos;
		}
		catch (std::exception& e) {
			std::string s = e.what();
			LOGE("FileOperate::GetThisPCFiles error is {}.", s);
			return {};
		}
#else
		return {};
#endif
	}
	std::tuple<bool, std::string> FileOperate::CreateFolder(std::string folder_path) {
		bool ret = true;
		std::string er_msg;
		try {
			QDir target_dir{QString::fromStdString(folder_path)};
			if (!target_dir.exists()) {
				target_dir.mkpath(".");
			}
			if (!target_dir.exists()) {
				ret = false;
			}
		}
		catch (std::exception& e) {
			ret = false;
			LOGE("CreateFolderHandle folder_path is {}, error is {}", folder_path, std::string(e.what()));
		}
		return {ret, er_msg};
	}

	std::tuple<bool, std::vector<std::string>, std::string> FileOperate::Remove(const std::vector<std::string>& paths) {
		bool ret = true;
		std::vector<std::string> er_paths;
		std::string er_msg;
		for (auto ph : paths) {
			try {
				QString path = QString::fromStdString(ph);
				QFileInfo info{ path };
				if (info.isDir()) {
					QDir dir{ path };
					if (!dir.exists()) {
						break;
					}
					if (dir.removeRecursively()) {
					}
					else {
						er_paths.emplace_back(ph);
					}
				}
				else if (info.isFile()) {
					QFile file{ path };
					if (!file.exists()) {
						break;
					}
					if (file.remove()) {
					}
					else {
						er_paths.emplace_back(ph);
					}
				}
				else {

				}
			}
			catch (std::exception& e) {
				LOGE("Remove error, remove {}, failed is {}.", ph, er_msg);
			}
		}
		return {ret, er_paths ,er_msg};
	}

	std::tuple<bool, std::string, std::string> FileOperate::CreateNewFolder(const std::string& parent_path_str) {
		try {
			QString parent_path = QString::fromStdString(parent_path_str);
			QDir dir{ parent_path };
			QString create_new_folder_path_ = "";
			bool create_new_folder_res_ = false;
			if (!dir.exists()) {
				//return { false, "", parent_path_str + QStringLiteral("不存在.").toStdString()};
				return { false, "", parent_path_str + tcTr("id_file_trans_no_exists").toStdString() };
			}
			int temp_count = 1;
			//QString prefix = QStringLiteral("新建文件夹");
			QString prefix = tcTr("id_file_trans_new_folder");
			do {
				QString suffix;
				if (1 == temp_count) {
					suffix = "";
				}
				else {
					suffix = "(" + QString::number(temp_count) + ")";
				}

				QString new_folder_name = prefix + suffix;
				create_new_folder_path_ = parent_path + "/" + new_folder_name;
				QDir target_folder{ create_new_folder_path_ };
				if (target_folder.exists()) {
					++temp_count;
					continue;
				}
				create_new_folder_res_ = dir.mkdir(new_folder_name);
				break;
			} while (true);
			return { create_new_folder_res_, create_new_folder_res_ ? create_new_folder_path_.toStdString() : "", ""};
		}
		catch (std::exception& e) {
			std::string s = e.what();
			LOGE("FileOperate::CreateNewFolder parent_path is {} error is {}", parent_path_str, s);
			//return { false, "", QStringLiteral("新建文件夹失败: ").toStdString() + s};
			return { false, "", tcTr("id_file_trans_new_folder_failed").toStdString() + s};
		}
	}

	std::tuple<bool, uint64_t, uint64_t> FileOperate::IsExists(const std::string& u8_path) {
		try { 
			QFileInfo file_info{QString::fromStdString(u8_path)};
			bool ret = file_info.exists();
			uint64_t file_size = 0;
			uint64_t date_changed = 0;
			if (ret) {
				file_size = file_info.size();
				date_changed = file_info.lastModified().toSecsSinceEpoch();
			}
			return {ret, file_size, date_changed};
		}
		catch (std::exception& e) {
			std::string s = e.what();
			LOGE(" FileOperate::IsExists error is {}", s);
			return {false, 0, 0};
		}
	}

	std::tuple<bool, std::string, std::string> FileOperate::Rename(const std::string& u8_old_path, const std::string& u8_new_name) {
		try {
			QString old_path = QString::fromStdString(u8_old_path);
			QFileInfo old_file_info{ old_path };
			if (!old_file_info.exists()) {
				//return { false, "", QStringLiteral("目标路径不存在").toStdString()};
				return { false, "", tcTr("id_file_trans_target_path_no_exists").toStdString()};
			}
			QString new_file_path = old_file_info.path() + "/" + QString::fromStdString(u8_new_name);
			QFileInfo new_file_info{ new_file_path };
			if (new_file_info.exists()) {
				//return { false, "", QStringLiteral("重命名失败, 文件名已经被占用").toStdString()};
				return { false, "", tcTr("id_file_trans_rename_failed_name_occypy").toStdString() };
			}

			if (!QFile::rename(old_path, new_file_path)) {
				//return { false, "", QStringLiteral("重命名失败, 请检查文件是否被占用").toStdString()};
				return { false, "", tcTr("id_file_trans_rename_failed_file_occypy").toStdString()};
			}
			
			return { true, new_file_path.toStdString(), "" };
		}
		catch (std::exception& e) {
			std::string s = e.what();
			LOGE("FileOperate::rename error is {}", s);
			//return { false, "", QStringLiteral("重命名失败").toStdString() + s};
			return { false, "", tcTr("id_file_trans_rename_failed").toStdString() + s};
		}
	}
}