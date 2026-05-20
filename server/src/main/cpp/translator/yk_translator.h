#pragma once
#include <vector>
#include <map>
#include <mutex>
#include <string>

#define ykTr(x) yk::YKTranslatorManager::Instance()->GetTrString(x)
#define ykTrMgr() yk::YKTranslatorManager::Instance()

namespace yk
{

    enum LanguageKind {
        kDefaultLang,
        kSimpleCN,
        kTraditionalCN,
        kEnglish
    };

#ifndef __ANDROID__
    enum TranslateUIType {
        kWidget,
        kLayout,
    };

    class YKTranslator {
    public:
        explicit YKTranslator(QObject *self, TranslateUIType type = TranslateUIType::kWidget);
        virtual ~YKTranslator();
        virtual void SetTextId(const QString &id);
        virtual void OnTranslate(LanguageKind kind);
        QWidget *AsWidget();
        QLayout *AsLayout();
        TranslateUIType GetUIType();
        QString GetTextId();
    protected:
        QObject *self_ = nullptr;
        TranslateUIType type_;
        QString text_id_;
    };
#endif

    class YKTranslatorManager {
    public:
        static YKTranslatorManager *Instance() {
            static YKTranslatorManager mgr;
            return &mgr;
        }

        void InitLanguage(LanguageKind kind = LanguageKind::kDefaultLang);
#ifdef __ANDROID__
        bool LoadLanguage(LanguageKind kind);
        bool LoadLanguage(const std::string &path);
        std::string GetTrString(const std::string &id);
#else
        bool LoadLanguage(LanguageKind kind);
        bool LoadLanguage(const QString &path);
        QString GetTrString(const QString &id);
#endif
#ifndef __ANDROID__
        void Translate();
        void AddUIObject(QObject *obj, QObject * = nullptr);
        void RemoveUIObject(QObject *obj);
        void NotifyLanguageChange();
#endif
        LanguageKind GetSelectedLanguage();
    private:
#ifdef __ANDROID__
        std::map<std::string, std::string> contents_;
#else
        std::map<QString, QString> contents_;
#endif
#ifndef __ANDROID__
        std::mutex ui_objs_mtx_;
        std::map<QObject *, QObject *> ui_objects_;
#endif
        LanguageKind kind_ = kDefaultLang;
    };

}
