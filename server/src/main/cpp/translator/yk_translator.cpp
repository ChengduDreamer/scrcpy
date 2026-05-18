#include "yk_translator.h"
#include <QFile>
#include <QIODevice>
#include <QDebug>
#include <QLocale>
#include <QApplication>
#include <QEvent>
#include <iostream>
#include "NlohmannJson/json.hpp"
#include "cpp_base_lib/yk_logger.h"

using namespace nlohmann;

namespace yk
{

    const std::string kUsingLanguage = "using_language";

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

    void YKTranslatorManager::InitLanguage(LanguageKind kind) {
        auto sys_name = QLocale::system().bcp47Name();
        YK_LOGI("system.bcp47 name: {}", sys_name.toStdString());
//      sys_name = QLocale(QLocale::Chinese, QLocale::China).bcp47Name();    // zh
//      YK_LOGI("system.bcp47 name: {}", sys_name.toStdString());
//      sys_name = QLocale(QLocale::Chinese, QLocale::Taiwan).bcp47Name();   // zh-TW
//      YK_LOGI("system.bcp47 name: {}", sys_name.toStdString());
//      sys_name = QLocale(QLocale::Chinese, QLocale::HongKong).bcp47Name();   // zh-HK
//      YK_LOGI("system.bcp47 name: {}", sys_name.toStdString());
//      sys_name = QLocale(QLocale::Chinese, QLocale::Macao).bcp47Name();   // zh-MO
//      YK_LOGI("system.bcp47 name: {}", sys_name.toStdString());
//      sys_name = QLocale(QLocale::English, QLocale::UnitedStates).bcp47Name(); // en
//      YK_LOGI("system.bcp47 name: {}", sys_name.toStdString());
//      sys_name =  QLocale(QLocale::Japanese, QLocale::Japan).bcp47Name();   // ja
//      YK_LOGI("system.bcp47 name: {}", sys_name.toStdString());
        LanguageKind target_kind = kind;
        this->LoadLanguage((LanguageKind)target_kind);
    }

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
                //YK_LOGI("{} => {}", it.key(), it.value());
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

    LanguageKind YKTranslatorManager::GetSelectedLanguage() { 
        return this->kind_;
    }

    void YKTranslatorManager::NotifyLanguageChange() {
        QEvent event(QEvent::LanguageChange);
        const auto widgets = QApplication::allWidgets();
        for (QWidget* w : widgets) {
            QCoreApplication::sendEvent(w, &event);
        }
    }

}