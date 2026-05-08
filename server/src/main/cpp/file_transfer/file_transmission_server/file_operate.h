#pragma once
#include <vector>
#include <memory>
#include <string>
#include <tuple>
#include <QString>
#include "tc_message.pb.h"

namespace tc {
	class FileOperate {
	public:
		FileOperate();
		
		std::tuple<bool, std::vector<tc::FileDescInfo>, std::string, std::string> GetFilesList(std::string path);

		std::tuple<bool, std::vector<tc::FileDescInfo>, std::string, std::string> RecursiveGetFilesList(std::string path);

		std::tuple<bool, std::string> CreateFolder(std::string folder_path);

		std::tuple<bool, std::vector<std::string>, std::string> Remove(const std::vector<std::string>& paths);

		std::tuple<bool, std::string, std::string> CreateNewFolder(const std::string& parent_path);

		std::tuple<bool, uint64_t, uint64_t> IsExists(const std::string& path);

		std::tuple<bool, std::string, std::string> Rename(const std::string& old_path, const std::string& new_name);

	private:
		// win32: 获取此电脑的内容
		std::vector<tc::FileDescInfo> GetThisPCFiles();

		const std::string root_path_ = "/";

		std::string desktop_path_ = "";

		const std::string folder_split_ = "<path_split>";

	private:
		std::vector<tc::FileDescInfo> GetFilesListImpl(const std::string& path);
		// 递归遍历
		void TraverseDirectory(const QString& path, std::vector<QString>& folders, std::vector<QString>& files);
	};

	

}