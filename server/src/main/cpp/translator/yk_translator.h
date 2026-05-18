#pragma once
#include <vector>
#include <map>
#include <mutex>

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

//
    class YKTranslatorManager {
    public:
        static YKTranslatorManager *Instance() {
            static YKTranslatorManager mgr;
            return &mgr;
        }

        void InitLanguage(LanguageKind kind = LanguageKind::kDefaultLang);
        bool LoadLanguage(LanguageKind kind);
        bool LoadLanguage(const QString &file);
        QString GetTrString(const QString &id);
        void Translate();
        void AddUIObject(QObject *obj, QObject * = nullptr);
        void RemoveUIObject(QObject *obj);
        LanguageKind GetSelectedLanguage();

        void NotifyLanguageChange();
    private:
        std::map<QString, QString> contents_;
        std::mutex ui_objs_mtx_;
        std::map<QObject *, QObject *> ui_objects_;
        LanguageKind kind_ = kDefaultLang;
    };

}