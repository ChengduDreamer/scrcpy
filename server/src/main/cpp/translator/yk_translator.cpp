#include "yk_translator.h"
#include <iostream>
#include "cpp_base_lib/yk_logger.h"

#ifdef __ANDROID__
    #include <fstream>
    #include "NlohmannJson/json.hpp"
#else
    #include <QFile>
    #include <QIODevice>
    #include <QDebug>
    #include <QLocale>
    #include <QApplication>
    #include <QEvent>
    #include "NlohmannJson/json.hpp"
#endif

using namespace nlohmann;

namespace yk {

#ifndef __ANDROID__
    YKTranslator::YKTranslator(QObject *self_, TranslateUIType type) {
        this->self_ = self_;
        this->type_ = type;
        YKTranslatorManager::Instance()->AddUIObject(this->self_);
    }

    YKTranslator::~YKTranslator() {
        YKTranslatorManager::Instance()->RemoveUIObject(self_);
    }

    QWidget *YKTranslator::AsWidget() {
        return (QWidget *) self_;
    }

    QLayout *YKTranslator::AsLayout() {
        return (QLayout *) self_;
    }

    TranslateUIType YKTranslator::GetUIType() {
        return type_;
    }

    void YKTranslator::SetTextId(const QString &id) {
        text_id_ = id;
    }

    QString YKTranslator::GetTextId() {
        return text_id_;
    }

    void YKTranslator::OnTranslate(LanguageKind kind) {
    }
#endif

    void YKTranslatorManager::InitLanguage(LanguageKind kind) {
#ifdef __ANDROID__
        YK_LOGI("InitLanguage: {}", static_cast<int>(kind));
        this->LoadLanguage(kind);
#else
        auto sys_name = QLocale::system().bcp47Name();
        YK_LOGI("system.bcp47 name: {}", sys_name.toStdString());
        LanguageKind target_kind = kind;
        this->LoadLanguage((LanguageKind)target_kind);
#endif
    }

#ifdef __ANDROID__
    bool YKTranslatorManager::LoadLanguage(LanguageKind kind) {
        std::string path;
        if (kind == LanguageKind::kSimpleCN) {
            path = "simple_cn.json";
        } else if (kind == LanguageKind::kTraditionalCN) {
            path = "traditional_cn.json";
        } else {
            path = "english.json";
        }
        this->kind_ = kind;
        return LoadLanguage(path);
    }

    bool YKTranslatorManager::LoadLanguage(const std::string &path) {
        YK_LOGI("Load language at: {}", path);
        if (path.empty()) {
            return false;
        }
        std::ifstream file(path);
        if (!file.is_open()) {
            YK_LOGI("Open: {} failed", path);
            return false;
        }
        try {
            std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            auto parsed_content = json::parse(content);
            contents_.clear();
            for (json::iterator it = parsed_content.begin(); it != parsed_content.end(); it++) {
                contents_[it.key()] = it.value().get<std::string>();
            }
            return true;
        }
        catch (json::exception &e) {
            YK_LOGE("Parse language file error: {}", e.what());
        }
        return false;
    }

    std::string YKTranslatorManager::GetTrString(const std::string &id) {
        if (contents_.find(id) != contents_.end()) {
            return contents_[id];
        }
        return id;
    }
#else
    bool YKTranslatorManager::LoadLanguage(LanguageKind kind) {
        QString path;
        if (kind == LanguageKind::kSimpleCN) {
            path = "./resources/language/simple_cn.json";
        } else if (kind == LanguageKind::kTraditionalCN) {
            path = "./resources/language/traditional_cn.json";
        } else if (kind == LanguageKind::kEnglish) {
            path = "./resources/language/english.json";
        } else {
            path = "./resources/language/english.json";
        }
        this->kind_ = kind;

        auto base_dir = QApplication::applicationDirPath();
        return LoadLanguage(base_dir + "/" + path);
    }

    bool YKTranslatorManager::LoadLanguage(const QString &path) {
        YK_LOGI("Load language at: {}", path.toStdString());
        if (path.isEmpty()) {
            return false;
        }
        QFile file(path);
        file.open(QIODevice::ReadOnly);
        if (!file.isOpen()) {
            YK_LOGI("Open: {} failed", path.toStdString());
            return false;
        }

        auto content = file.readAll().toStdString();
        try {
            auto parsed_content = json::parse(content);
            contents_.clear();
            for (json::iterator it = parsed_content.begin(); it != parsed_content.end(); it++) {
                contents_[it.key().c_str()] = it.value().get<std::string>().c_str();
            }
            return true;
        }
        catch (json::exception &e) {
            std::cout << "error : " << e.what() << std::endl;
        }
        return false;
    }

    QString YKTranslatorManager::GetTrString(const QString &id) {
        if (contents_.find(id) != contents_.end()) {
            return contents_[id];
        }
        return id;
    }

    void YKTranslatorManager::Translate() {
        std::lock_guard<std::mutex> guard(ui_objs_mtx_);
        for (auto &pair: ui_objects_) {
            auto translator = dynamic_cast<YKTranslator *>(pair.first);
            translator->OnTranslate(kind_);
        }
    }

    void YKTranslatorManager::AddUIObject(QObject *obj, QObject *val) {
        std::lock_guard<std::mutex> guard(ui_objs_mtx_);
        ui_objects_[obj] = val;
    }

    void YKTranslatorManager::RemoveUIObject(QObject *obj) {
        std::lock_guard<std::mutex> guard(ui_objs_mtx_);
        if (ui_objects_.find(obj) != ui_objects_.end()) {
            ui_objects_.erase(obj);
        }
    }

    void YKTranslatorManager::NotifyLanguageChange() {
        QEvent event(QEvent::LanguageChange);
        const auto widgets = QApplication::allWidgets();
        for (QWidget* w : widgets) {
            QCoreApplication::sendEvent(w, &event);
        }
    }
#endif

    LanguageKind YKTranslatorManager::GetSelectedLanguage() {
        return this->kind_;
    }

}
