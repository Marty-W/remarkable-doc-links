#include <cstdlib>
#include <cstring>
#include <functional>

#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QList>
#include <QImage>
#include <QMimeData>
#include <QMimeDatabase>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QMetaType>
#include <QObject>
#include <QJSValue>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSet>
#include <QTextStream>
#include <QThread>
#include <QUrl>
#include <QVariant>
#include <QWindow>

namespace {

constexpr const char *kStagedPath = "/home/root/.cache/desktop-clipboard.json";
constexpr const char *kStagedImagePath = "/home/root/.cache/desktop-clipboard-image.bin";
constexpr const char *kUiDumpPath = "/home/root/.cache/desktop-clipboard.ui.txt";
constexpr const char *kGlobalsDumpPath = "/home/root/.cache/desktop-clipboard.globals.txt";
constexpr const char *kXochitlRootPath = "/home/root/.local/share/remarkable/xochitl";

QQmlEngine *gQmlEngine = nullptr;
QObject *gRemarkableHelper = nullptr;
QObject *gRemarkableLibrary = nullptr;
QObject *gRemarkableLibraryController = nullptr;

bool containsKeyword(const QString &value)
{
    const QString lower = value.toLower();
    static const char *keywords[] = {
        "scene", "document", "page", "text", "toolbar", "selection",
        "navigator", "controller", "input", "clipboard", "image",
        "html", "mime", "markdown"
    };
    for (const char *keyword : keywords) {
        if (lower.contains(QLatin1String(keyword)))
            return true;
    }
    return false;
}

QString describeObject(QObject *object)
{
    if (!object)
        return QStringLiteral("<null>");

    QString line = QString::fromLatin1(object->metaObject()->className());
    const QString objectName = object->objectName();
    if (!objectName.isEmpty())
        line += QStringLiteral("#") + objectName;

    const QVariant visible = object->property("visible");
    if (visible.isValid())
        line += QStringLiteral(" visible=") + visible.toString();

    const QVariant enabled = object->property("enabled");
    if (enabled.isValid())
        line += QStringLiteral(" enabled=") + enabled.toString();

    return line;
}

QString formatVariantValue(const QVariant &value, QObject **referencedObject = nullptr)
{
    if (referencedObject)
        *referencedObject = nullptr;

    if (!value.isValid())
        return QStringLiteral("<invalid>");

    const QMetaType metaType = value.metaType();
    if (metaType.flags().testFlag(QMetaType::PointerToQObject)) {
        QObject *object = qvariant_cast<QObject *>(value);
        if (referencedObject)
            *referencedObject = object;
        return describeObject(object);
    }

    if (metaType == QMetaType::fromType<QJSValue>()) {
        const QJSValue jsValue = value.value<QJSValue>();
        if (jsValue.isQObject()) {
            QObject *object = jsValue.toQObject();
            if (referencedObject)
                *referencedObject = object;
            return describeObject(object);
        }
        if (jsValue.isNull())
            return QStringLiteral("<null>");
        if (jsValue.isUndefined())
            return QStringLiteral("<undefined>");
        if (jsValue.isBool())
            return jsValue.toBool() ? QStringLiteral("true") : QStringLiteral("false");
        if (jsValue.isNumber())
            return QString::number(jsValue.toNumber());
        if (jsValue.isString())
            return jsValue.toString();
        return QStringLiteral("<QJSValue>");
    }

    const QString text = value.toString();
    if (!text.isEmpty())
        return text;

    if (metaType.isValid() && metaType.name())
        return QStringLiteral("<%1>").arg(QString::fromLatin1(metaType.name()));

    return QStringLiteral("<unprintable>");
}

void dumpInterestingMembers(QObject *object, QTextStream &out, const QString &indent)
{
    if (!object)
        return;

    const QMetaObject *metaObject = object->metaObject();

    bool wroteHeader = false;
    for (int i = metaObject->methodOffset(); i < metaObject->methodCount(); ++i) {
        const QMetaMethod method = metaObject->method(i);
        const QString signature = QString::fromLatin1(method.methodSignature());
        if (!containsKeyword(signature))
            continue;
        if (!wroteHeader) {
            out << indent << "methods:\n";
            wroteHeader = true;
        }
        out << indent << "  - " << signature << "\n";
    }

    wroteHeader = false;
    for (int i = metaObject->propertyOffset(); i < metaObject->propertyCount(); ++i) {
        const QMetaProperty property = metaObject->property(i);
        const QString name = QString::fromLatin1(property.name());
        if (!containsKeyword(name))
            continue;
        if (!wroteHeader) {
            out << indent << "properties:\n";
            wroteHeader = true;
        }
        out << indent << "  - " << name;
        if (property.isReadable()) {
            QObject *referencedObject = nullptr;
            const QVariant value = property.read(object);
            out << " = " << formatVariantValue(value, &referencedObject);
            out << " [" << QString::fromLatin1(value.metaType().name() ? value.metaType().name() : "unknown") << "]";
            out << "\n";
            if (referencedObject && referencedObject != object) {
                out << indent << "    ref: " << describeObject(referencedObject) << "\n";
                dumpInterestingMembers(referencedObject, out, indent + QStringLiteral("      "));
            }
            continue;
        }
        out << "\n";
    }
}

void dumpObjectTree(QObject *object, QTextStream &out, int depth, int maxDepth)
{
    if (!object || depth > maxDepth)
        return;

    const QString indent(depth * 2, QLatin1Char(' '));
    const QString className = QString::fromLatin1(object->metaObject()->className());
    const QString objectName = object->objectName();
    const bool interesting = containsKeyword(className) || containsKeyword(objectName) || depth <= 2;

    out << indent << describeObject(object) << "\n";
    if (interesting)
        dumpInterestingMembers(object, out, indent + QStringLiteral("  "));

    QSet<QObject *> nextChildren;
    if (auto *quickItem = qobject_cast<QQuickItem *>(object)) {
        const auto childItems = quickItem->childItems();
        for (QQuickItem *childItem : childItems)
            nextChildren.insert(childItem);
    }
    const QObjectList children = object->children();
    for (QObject *child : children)
        nextChildren.insert(child);

    for (QObject *child : nextChildren)
        dumpObjectTree(child, out, depth + 1, maxDepth);
}

QString dumpUiReport(const QString &requestedPath)
{
    const QString path = requestedPath.trimmed().isEmpty()
        ? QString::fromUtf8(kUiDumpPath)
        : requestedPath.trimmed();

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return QStringLiteral("ERROR: could not open %1").arg(path);

    QTextStream out(&file);
    out << "desktop-clipboard native UI dump\n";
    out << "timestamp=" << QDateTime::currentDateTimeUtc().toString(Qt::ISODate) << "\n";
    out << "thread=" << QThread::currentThread() << "\n\n";

    const auto windows = QGuiApplication::allWindows();
    out << "windows=" << windows.size() << "\n\n";
    int windowIndex = 0;
    for (QWindow *window : windows) {
        out << "window[" << windowIndex++ << "] " << describeObject(window) << "\n";
        if (QQmlEngine *engine = qmlEngine(window)) {
            out << "  qmlEngine=" << engine << "\n";
        } else {
            out << "  qmlEngine=<null>\n";
        }

        out << "  windowTree:\n";
        dumpObjectTree(window, out, 2, 6);
        if (auto *quickWindow = qobject_cast<QQuickWindow *>(window)) {
            QQuickItem *contentItem = quickWindow->contentItem();
            out << "  contentItem=" << describeObject(contentItem) << "\n";
            out << "  itemTree:\n";
            dumpObjectTree(contentItem, out, 2, 8);
        }
        out << "\n";
    }

    file.close();
    return QStringLiteral("ok:%1").arg(path);
}

struct StagedPayload {
    QString revision;
    QString text;
    QString error;
};

struct StagedImagePayload {
    QByteArray bytes;
    QImage image;
    QString mimeName;
    QString error;
};

StagedPayload readStagedPayload()
{
    QFile file(QString::fromUtf8(kStagedPath));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return { {}, {}, QStringLiteral("missing staged file") };

    const QString raw = QString::fromUtf8(file.readAll());
    const int splitAt = raw.indexOf(QLatin1Char('\n'));
    if (splitAt < 0)
        return { {}, {}, QStringLiteral("malformed staged file") };

    const QString revision = raw.left(splitAt).trimmed();
    const QString text = raw.mid(splitAt + 1);
    if (text.isEmpty())
        return { revision, text, QStringLiteral("empty staged text") };

    return { revision, text, {} };
}

StagedImagePayload readStagedImagePayload()
{
    QFile file(QString::fromUtf8(kStagedImagePath));
    if (!file.open(QIODevice::ReadOnly))
        return { {}, {}, {}, QStringLiteral("missing staged image file") };

    const QByteArray bytes = file.readAll();
    file.close();
    if (bytes.isEmpty())
        return { {}, {}, {}, QStringLiteral("staged image file is empty") };

    QImage image;
    if (!image.loadFromData(bytes))
        return { {}, {}, {}, QStringLiteral("staged image is not a valid image") };

    QMimeDatabase mimeDb;
    QString mimeName = mimeDb.mimeTypeForData(bytes).name();
    if (mimeName.isEmpty() || mimeName == QStringLiteral("application/octet-stream"))
        mimeName = QStringLiteral("image/png");

    return { bytes, image, mimeName, {} };
}

QString readStagedSummary()
{
    const StagedPayload payload = readStagedPayload();
    if (!payload.error.isEmpty())
        return QStringLiteral("ERROR: %1").arg(payload.error);
    return QStringLiteral("revision=%1 chars=%2").arg(payload.revision).arg(payload.text.size());
}

void collectChildObjects(QObject *object, QSet<QObject *> &nextChildren)
{
    if (!object)
        return;
    if (auto *quickItem = qobject_cast<QQuickItem *>(object)) {
        const auto childItems = quickItem->childItems();
        for (QQuickItem *childItem : childItems)
            nextChildren.insert(childItem);
    }
    const QObjectList children = object->children();
    for (QObject *child : children)
        nextChildren.insert(child);

    const QMetaObject *metaObject = object->metaObject();
    for (int i = metaObject->propertyOffset(); i < metaObject->propertyCount(); ++i) {
        const QMetaProperty property = metaObject->property(i);
        if (!property.isReadable())
            continue;
        if (!property.metaType().flags().testFlag(QMetaType::PointerToQObject))
            continue;
        QObject *referencedObject = qvariant_cast<QObject *>(property.read(object));
        if (referencedObject && referencedObject != object)
            nextChildren.insert(referencedObject);
    }
}

QObject *findFirstObject(QObject *root, const std::function<bool(QObject *)> &predicate, QSet<QObject *> &visited)
{
    if (!root || visited.contains(root))
        return nullptr;
    visited.insert(root);

    if (predicate(root))
        return root;

    QSet<QObject *> nextChildren;
    collectChildObjects(root, nextChildren);
    for (QObject *child : nextChildren) {
        if (QObject *found = findFirstObject(child, predicate, visited))
            return found;
    }
    return nullptr;
}

bool ensureQmlEngine()
{
    if (gQmlEngine)
        return true;

    const auto windows = QGuiApplication::allWindows();
    for (QWindow *window : windows) {
        if (QQmlEngine *engine = qmlEngine(window)) {
            gQmlEngine = engine;
            return true;
        }
    }
    return false;
}

bool ensureRemarkableLibrary()
{
    if (gRemarkableLibrary && gRemarkableLibraryController)
        return true;
    if (!ensureQmlEngine())
        return false;

    static const char *kHelperQml = R"QML(
        import QtQml
        import com.remarkable 1.0 as RM
        QtObject {
            property var lib: RM.Library
            property var ctrl: RM.LibraryController
        }
    )QML";

    QQmlComponent component(gQmlEngine);
    component.setData(kHelperQml, QUrl());
    QObject *holder = component.create(gQmlEngine->rootContext());
    if (!holder)
        return false;

    holder->setParent(gQmlEngine);
    gRemarkableHelper = holder;

    const QVariant libraryValue = holder->property("lib");
    const QVariant controllerValue = holder->property("ctrl");
    gRemarkableLibrary = libraryValue.isValid() ? libraryValue.value<QObject *>() : nullptr;
    gRemarkableLibraryController = controllerValue.isValid() ? controllerValue.value<QObject *>() : nullptr;
    return gRemarkableLibrary && gRemarkableLibraryController;
}

QObject *findActiveDocumentView()
{
    const auto windows = QGuiApplication::allWindows();
    for (QWindow *window : windows) {
        QSet<QObject *> visited;
        auto predicate = [](QObject *object) {
            const QString className = QString::fromLatin1(object->metaObject()->className());
            if (!className.contains(QStringLiteral("DocumentView"), Qt::CaseInsensitive))
                return false;
            const QVariant loaded = object->property("documentLoaded");
            const QVariant notePage = object->property("notePage");
            return loaded.isValid() && loaded.toBool() && notePage.isValid() && notePage.toBool();
        };

        if (auto *quickWindow = qobject_cast<QQuickWindow *>(window)) {
            if (QObject *found = findFirstObject(quickWindow->contentItem(), predicate, visited))
                return found;
        }

        if (QObject *found = findFirstObject(window, predicate, visited))
            return found;
    }
    return nullptr;
}

QObject *findObjectByNeedle(const QString &needle)
{
    const QString normalizedNeedle = needle.trimmed();
    if (normalizedNeedle.isEmpty())
        return nullptr;

    const auto windows = QGuiApplication::allWindows();
    for (QWindow *window : windows) {
        QSet<QObject *> visited;
        auto predicate = [&normalizedNeedle](QObject *object) {
            const QString className = QString::fromLatin1(object->metaObject()->className());
            const QString objectName = object->objectName();
            return className.contains(normalizedNeedle, Qt::CaseInsensitive)
                || objectName.contains(normalizedNeedle, Qt::CaseInsensitive);
        };

        if (auto *quickWindow = qobject_cast<QQuickWindow *>(window)) {
            if (QObject *found = findFirstObject(quickWindow->contentItem(), predicate, visited))
                return found;
        }

        if (QObject *found = findFirstObject(window, predicate, visited))
            return found;
    }
    return nullptr;
}

bool invokeNoArg(QObject *object, const char *method)
{
    return object && QMetaObject::invokeMethod(object, method, Qt::DirectConnection);
}

bool invokeBoolArg(QObject *object, const char *method, bool value)
{
    return object && QMetaObject::invokeMethod(object, method, Qt::DirectConnection, Q_ARG(bool, value));
}

bool invokeStringArg(QObject *object, const char *method, const QString &value)
{
    return object && QMetaObject::invokeMethod(object, method, Qt::DirectConnection, Q_ARG(QString, value));
}

QString invokeQStringResultNoArg(QObject *object, const char *method)
{
    if (!object)
        return QString();
    QString result;
    QMetaObject::invokeMethod(object, method, Qt::DirectConnection, Q_RETURN_ARG(QString, result));
    return result;
}

QMetaProperty findMetaProperty(QObject *object, const char *name)
{
    QMetaProperty property;
    if (!object)
        return property;
    const int index = object->metaObject()->indexOfProperty(name);
    if (index >= 0)
        property = object->metaObject()->property(index);
    return property;
}

QString compactForSummary(QString text)
{
    text.replace(QLatin1Char('\n'), QLatin1Char(' '));
    if (text.size() > 80)
        text = text.left(80) + QStringLiteral("…");
    return text.isEmpty() ? QStringLiteral("<empty>") : text;
}

QString summarizeClipboardState(QObject *clipboardObject)
{
    if (!clipboardObject)
        return QStringLiteral("clipboard=<null>");

    const QVariant sceneHasText = clipboardObject->property("hasText");
    const QVariant sceneHasItems = clipboardObject->property("hasItems");
    const QVariant sceneText = clipboardObject->property("text");
    const QVariant sceneItems = clipboardObject->property("items");
    const QVariant sceneLastCopiedType = clipboardObject->property("lastCopiedType");
    const QMetaProperty textProperty = findMetaProperty(clipboardObject, "text");
    const QMetaProperty itemsProperty = findMetaProperty(clipboardObject, "items");
    const QMetaProperty lastCopiedTypeProperty = findMetaProperty(clipboardObject, "lastCopiedType");
    const QMetaObject *sceneTextMetaObject = sceneText.isValid() ? sceneText.metaType().metaObject() : nullptr;
    const QMetaObject *sceneItemsMetaObject = sceneItems.isValid() ? sceneItems.metaType().metaObject() : nullptr;
    const QString textString = compactForSummary(invokeQStringResultNoArg(clipboardObject, "textString"));

    return QStringLiteral("sceneHasText=%1 sceneHasItems=%2 lastCopiedType=%3 sceneTextType=%4 sceneTextMeta=%5 sceneItemsType=%6 sceneItemsMeta=%7 textWritable=%8 itemsWritable=%9 lastCopiedWritable=%10 textString=%11")
        .arg(sceneHasText.isValid() ? sceneHasText.toBool() : false)
        .arg(sceneHasItems.isValid() ? sceneHasItems.toBool() : false)
        .arg(sceneLastCopiedType.isValid() ? sceneLastCopiedType.toInt() : -1)
        .arg(QString::fromLatin1(sceneText.isValid() && sceneText.metaType().name() ? sceneText.metaType().name() : "unknown"))
        .arg(sceneTextMetaObject ? QString::fromLatin1(sceneTextMetaObject->className()) : QStringLiteral("<none>"))
        .arg(QString::fromLatin1(sceneItems.isValid() && sceneItems.metaType().name() ? sceneItems.metaType().name() : "unknown"))
        .arg(sceneItemsMetaObject ? QString::fromLatin1(sceneItemsMetaObject->className()) : QStringLiteral("<none>"))
        .arg(textProperty.isValid() ? textProperty.isWritable() : false)
        .arg(itemsProperty.isValid() ? itemsProperty.isWritable() : false)
        .arg(lastCopiedTypeProperty.isValid() ? lastCopiedTypeProperty.isWritable() : false)
        .arg(textString);
}

QString applyMimeDataToSystemClipboard(QMimeData *mimeData, QObject *clipboardObject)
{
    QClipboard *clipboard = QGuiApplication::clipboard();
    if (!clipboard) {
        delete mimeData;
        return QStringLiteral("ERROR: no QClipboard");
    }

    clipboard->setMimeData(mimeData);
    const bool syncOk = invokeNoArg(clipboardObject, "systemClipboardDataChanged");
    const QMimeData *current = clipboard->mimeData();
    const bool hasImage = current && current->hasImage();
    const bool hasHtml = current && current->hasHtml();
    const bool hasText = current && current->hasText();
    const bool hasUrls = current && current->hasUrls();

    return QStringLiteral("qtHasImage=%1 qtHasHtml=%2 qtHasText=%3 qtHasUrls=%4 sync=%5 %6")
        .arg(hasImage)
        .arg(hasHtml)
        .arg(hasText)
        .arg(hasUrls)
        .arg(syncOk)
        .arg(summarizeClipboardState(clipboardObject));
}

bool invokeVariantBySignature(QObject *object, const char *signature, const QVariant &value)
{
    if (!object || !value.isValid())
        return false;

    const QMetaObject *metaObject = object->metaObject();
    for (int i = metaObject->methodOffset(); i < metaObject->methodCount(); ++i) {
        const QMetaMethod method = metaObject->method(i);
        if (method.methodSignature() != QByteArray(signature))
            continue;
        const char *typeName = value.metaType().name();
        if (!typeName)
            return false;
        return method.invoke(
            object,
            Qt::DirectConnection,
            QGenericArgument(typeName, value.constData())
        );
    }
    return false;
}

bool invokeQVariantBySignature(QObject *object, const char *signature, const QVariant &value = QVariant())
{
    if (!object)
        return false;

    const QMetaObject *metaObject = object->metaObject();
    for (int i = metaObject->methodOffset(); i < metaObject->methodCount(); ++i) {
        const QMetaMethod method = metaObject->method(i);
        if (method.methodSignature() != QByteArray(signature))
            continue;

        QVariant copy = value;
        return method.invoke(
            object,
            Qt::DirectConnection,
            QGenericArgument("QVariant", &copy)
        );
    }
    return false;
}

QString insertStagedIntoActiveDocument()
{
    const StagedPayload payload = readStagedPayload();
    if (!payload.error.isEmpty())
        return QStringLiteral("ERROR: %1").arg(payload.error);

    QObject *documentView = findActiveDocumentView();
    if (!documentView)
        return QStringLiteral("ERROR: active DocumentView not found");

    QObject *sceneController = nullptr;
    const QVariant sceneControllerValue = documentView->property("sceneController");
    if (sceneControllerValue.isValid() && sceneControllerValue.metaType().flags().testFlag(QMetaType::PointerToQObject))
        sceneController = qvariant_cast<QObject *>(sceneControllerValue);
    if (!sceneController)
        return QStringLiteral("ERROR: active sceneController not found");

    const QString pageId = documentView->property("currentPageId").toString();
    const bool hasRootBefore = sceneController->property("hasRootDocument").toBool();
    const int lengthBefore = sceneController->property("rootDocumentLength").toInt();

    const bool visibleOk = invokeBoolArg(sceneController, "setRootDocumentVisible", true);
    const bool createOk = hasRootBefore ? true : invokeNoArg(sceneController, "createRootDocument");
    const bool focusOk = invokeNoArg(sceneController, "focusRootDocument");
    const bool beginOk = invokeNoArg(sceneController, "beginInputMethodTransaction");
    const bool replaceOk = invokeStringArg(sceneController, "replaceText", payload.text);
    const bool endOk = invokeNoArg(sceneController, "endInputMethodTransaction");

    const bool hasRootAfter = sceneController->property("hasRootDocument").toBool();
    const int lengthAfter = sceneController->property("rootDocumentLength").toInt();
    const int cursorIndex = sceneController->property("textCursorIndex").toInt();
    const bool cursorVisible = sceneController->property("isTextCursorVisible").toBool();

    const bool operationOk = visibleOk && createOk && focusOk && beginOk && replaceOk && endOk;

    return QStringLiteral(
        "%1 page=%2 revision=%3 chars=%4 visible=%5 create=%6 focus=%7 begin=%8 replace=%9 end=%10 rootBefore=%11 rootAfter=%12 lenBefore=%13 lenAfter=%14 cursor=%15 cursorVisible=%16"
    )
        .arg(operationOk ? QStringLiteral("ok") : QStringLiteral("partial"))
        .arg(pageId)
        .arg(payload.revision)
        .arg(payload.text.size())
        .arg(visibleOk)
        .arg(createOk)
        .arg(focusOk)
        .arg(beginOk)
        .arg(replaceOk)
        .arg(endOk)
        .arg(hasRootBefore)
        .arg(hasRootAfter)
        .arg(lengthBefore)
        .arg(lengthAfter)
        .arg(cursorIndex)
        .arg(cursorVisible);
}

QString dumpGlobalObjectsReport(const QString &requestedPath)
{
    const QString path = requestedPath.trimmed().isEmpty()
        ? QString::fromUtf8(kGlobalsDumpPath)
        : requestedPath.trimmed();

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return QStringLiteral("ERROR: could not open %1").arg(path);

    QTextStream out(&file);
    out << "desktop-clipboard native globals dump\n";
    out << "timestamp=" << QDateTime::currentDateTimeUtc().toString(Qt::ISODate) << "\n\n";

    QCoreApplication *app = QCoreApplication::instance();
    if (!app) {
        out << "app=<null>\n";
        file.close();
        return QStringLiteral("ok:%1").arg(path);
    }

    out << "app=" << describeObject(app) << "\n";

    QList<QObject *> queue;
    QSet<QObject *> visited;
    queue.append(app);
    int hits = 0;
    constexpr int kMaxVisited = 2500;
    constexpr int kMaxHits = 40;

    while (!queue.isEmpty() && visited.size() < kMaxVisited && hits < kMaxHits) {
        QObject *object = queue.takeFirst();
        if (!object || visited.contains(object))
            continue;
        visited.insert(object);

        const QString className = QString::fromLatin1(object->metaObject()->className());
        const QString objectName = object->objectName();
        const bool interesting = containsKeyword(className) || containsKeyword(objectName);
        if (interesting) {
            out << "\nobject=" << describeObject(object) << "\n";
            dumpInterestingMembers(object, out, QStringLiteral("  "));
            ++hits;
        }

        const QObjectList children = object->children();
        for (QObject *child : children)
            queue.append(child);
    }

    out << "\nhits=" << hits << " visited=" << visited.size() << " remainingQueue=" << queue.size() << "\n";
    file.close();
    return QStringLiteral("ok:%1").arg(path);
}

QObject *evaluateContextObject(QObject *documentView, const QString &expressionText)
{
    if (!documentView)
        return nullptr;
    QQmlContext *context = qmlContext(documentView);
    if (!context)
        return nullptr;

    QQmlExpression expression(context, documentView, expressionText);
    const QVariant value = expression.evaluate();
    if (expression.hasError())
        return nullptr;

    QObject *referencedObject = nullptr;
    formatVariantValue(value, &referencedObject);
    return referencedObject;
}

QObject *findClipboardObject(QObject *documentView)
{
    return evaluateContextObject(documentView, QStringLiteral("Clipboard"));
}

QObject *findRecognizerObject(QObject *documentView)
{
    return evaluateContextObject(documentView, QStringLiteral("Recognizer"));
}

QString inspectActiveContextGlobals()
{
    QObject *documentView = findActiveDocumentView();
    if (!documentView)
        return QStringLiteral("ERROR: active DocumentView not found");

    QQmlContext *context = qmlContext(documentView);
    if (!context)
        return QStringLiteral("ERROR: no QQmlContext for active DocumentView");

    QStringList lines;
    const QList<QPair<QString, QString>> expressions = {
        { QStringLiteral("typeof Clipboard"), QStringLiteral("typeof Clipboard") },
        { QStringLiteral("Clipboard"), QStringLiteral("Clipboard") },
        { QStringLiteral("Clipboard.text"), QStringLiteral("Clipboard && Clipboard.text") },
        { QStringLiteral("Clipboard.contentHtml"), QStringLiteral("Clipboard && Clipboard.contentHtml") },
        { QStringLiteral("Clipboard.contentText"), QStringLiteral("Clipboard && Clipboard.contentText") },
        { QStringLiteral("Clipboard.lastCopy"), QStringLiteral("Clipboard && Clipboard.lastCopy") },
        { QStringLiteral("typeof Recognizer"), QStringLiteral("typeof Recognizer") },
        { QStringLiteral("Recognizer"), QStringLiteral("Recognizer") },
        { QStringLiteral("Recognizer.recognitionJob"), QStringLiteral("Recognizer && Recognizer.recognitionJob") },
        { QStringLiteral("Recognizer.selectionToolJob"), QStringLiteral("Recognizer && Recognizer.selectionToolJob") },
        { QStringLiteral("Recognizer.language"), QStringLiteral("Recognizer && Recognizer.language") },
        { QStringLiteral("Recognizer.text"), QStringLiteral("Recognizer && Recognizer.text") },
        { QStringLiteral("typeof rm"), QStringLiteral("typeof rm") },
        { QStringLiteral("typeof Values"), QStringLiteral("typeof Values") }
    };

    for (const auto &entry : expressions) {
        QQmlExpression expression(context, documentView, entry.second);
        QObject *referencedObject = nullptr;
        const QVariant value = expression.evaluate();
        if (expression.hasError()) {
            lines << QStringLiteral("%1 ERROR %2").arg(entry.first, expression.error().toString());
            continue;
        }
        lines << QStringLiteral("%1 = %2 [%3]")
            .arg(entry.first)
            .arg(formatVariantValue(value, &referencedObject))
            .arg(QString::fromLatin1(value.metaType().name() ? value.metaType().name() : "unknown"));
        if (referencedObject) {
            QString detail;
            QTextStream detailStream(&detail);
            detailStream << "  ref: " << describeObject(referencedObject) << "\n";
            dumpInterestingMembers(referencedObject, detailStream, QStringLiteral("    "));
            lines << detail.trimmed();
        }
    }

    return lines.join(QLatin1Char('\n'));
}

QString loadStagedImageClipboard()
{
    const StagedImagePayload payload = readStagedImagePayload();
    if (!payload.error.isEmpty())
        return QStringLiteral("ERROR: %1").arg(payload.error);

    auto *mimeData = new QMimeData();
    const QUrl imageUrl = QUrl::fromLocalFile(QString::fromUtf8(kStagedImagePath));
    mimeData->setImageData(payload.image);
    mimeData->setData(payload.mimeName.toUtf8(), payload.bytes);
    mimeData->setUrls({ imageUrl });
    mimeData->setHtml(QStringLiteral("<p><img src=\"%1\" /></p>")
        .arg(imageUrl.toString().toHtmlEscaped()));

    QObject *documentView = findActiveDocumentView();
    QObject *clipboardObject = findClipboardObject(documentView);

    return QStringLiteral("ok mime=%1 bytes=%2 size=%3x%4 %5")
        .arg(payload.mimeName)
        .arg(payload.bytes.size())
        .arg(payload.image.width())
        .arg(payload.image.height())
        .arg(applyMimeDataToSystemClipboard(mimeData, clipboardObject));
}

QString insertStagedImageIntoActiveDocument()
{
    QObject *documentView = findActiveDocumentView();
    if (!documentView)
        return QStringLiteral("ERROR: active DocumentView not found");

    QObject *sceneController = nullptr;
    const QVariant sceneControllerValue = documentView->property("sceneController");
    if (sceneControllerValue.isValid() && sceneControllerValue.metaType().flags().testFlag(QMetaType::PointerToQObject))
        sceneController = qvariant_cast<QObject *>(sceneControllerValue);
    if (!sceneController)
        return QStringLiteral("ERROR: active sceneController not found");

    QObject *clipboardObject = findClipboardObject(documentView);
    if (!clipboardObject)
        return QStringLiteral("ERROR: Clipboard singleton not found");

    const QString clipboardSummary = loadStagedImageClipboard();
    if (clipboardSummary.startsWith(QStringLiteral("ERROR:")))
        return clipboardSummary;

    const QVariant sceneClipboard = clipboardObject->property("text");
    if (!sceneClipboard.isValid())
        return QStringLiteral("ERROR: Clipboard.text is invalid after sync");

    const bool hasRootBefore = sceneController->property("hasRootDocument").toBool();
    const bool createOk = hasRootBefore ? true : invokeNoArg(sceneController, "createRootDocument");
    const bool focusOk = invokeNoArg(sceneController, "focusRootDocument");
    const bool pasteOk = invokeVariantBySignature(sceneController, "pasteText(SceneClipboardText)", sceneClipboard);
    const bool hasRootAfter = sceneController->property("hasRootDocument").toBool();
    const int lengthAfter = sceneController->property("rootDocumentLength").toInt();
    const QString pageId = documentView->property("currentPageId").toString();

    return QStringLiteral("page=%1 create=%2 focus=%3 paste=%4 rootBefore=%5 rootAfter=%6 lenAfter=%7 clipType=%8")
        .arg(pageId)
        .arg(createOk)
        .arg(focusOk)
        .arg(pasteOk)
        .arg(hasRootBefore)
        .arg(hasRootAfter)
        .arg(lengthAfter)
        .arg(QString::fromLatin1(sceneClipboard.metaType().name() ? sceneClipboard.metaType().name() : "unknown"));
}

QString probeRegisteredTypes()
{
    QStringList lines;
    const QStringList names = {
        QStringLiteral("Clipboard"),
        QStringLiteral("Clipboard*"),
        QStringLiteral("Recognizer"),
        QStringLiteral("Recognizer*"),
        QStringLiteral("RecognitionJob"),
        QStringLiteral("RecognitionJob*"),
        QStringLiteral("SceneClipboardText"),
        QStringLiteral("QList<std::shared_ptr<SceneItem>>"),
        QStringLiteral("SceneItem"),
        QStringLiteral("SceneItem*"),
        QStringLiteral("SceneImageItem"),
        QStringLiteral("SceneImageItem*"),
        QStringLiteral("CrdtImageItem"),
        QStringLiteral("CrdtImageItem*"),
        QStringLiteral("SceneImagesTable"),
        QStringLiteral("SceneImagesTable*"),
        QStringLiteral("ImageBlock"),
        QStringLiteral("ImageBlock*"),
        QStringLiteral("DocumentWorker"),
        QStringLiteral("DocumentWorker*"),
        QStringLiteral("SelectionImage"),
        QStringLiteral("SelectionImage*"),
        QStringLiteral("MarkdownToHtml"),
        QStringLiteral("MarkdownToHtml*"),
        QStringLiteral("HtmlExporter"),
        QStringLiteral("HtmlExporter*")
    };

    for (const QString &name : names) {
        const QByteArray utf8 = name.toUtf8();
        const QMetaType metaType = QMetaType::fromName(utf8.constData());
        lines << QStringLiteral("type=%1 valid=%2 id=%3 flags=%4")
            .arg(name)
            .arg(metaType.isValid())
            .arg(metaType.id())
            .arg(int(metaType.flags()));
        if (!metaType.isValid())
            continue;
        const QMetaObject *metaObject = metaType.metaObject();
        lines << QStringLiteral("  metaObject=%1")
            .arg(metaObject ? QString::fromLatin1(metaObject->className()) : QStringLiteral("<none>"));
        if (!metaObject)
            continue;
        QObject *instance = metaObject->newInstance();
        lines << QStringLiteral("  newInstance=%1").arg(instance ? describeObject(instance) : QStringLiteral("<failed>"));
        if (instance)
            delete instance;
        const int propertyCount = metaObject->propertyCount() - metaObject->propertyOffset();
        const int methodCount = metaObject->methodCount() - metaObject->methodOffset();
        lines << QStringLiteral("  properties=%1 methods=%2").arg(propertyCount).arg(methodCount);
        for (int i = metaObject->propertyOffset(); i < metaObject->propertyCount() && i < metaObject->propertyOffset() + 12; ++i) {
            const QMetaProperty property = metaObject->property(i);
            lines << QStringLiteral("    prop %1 : %2")
                .arg(QString::fromLatin1(property.name()))
                .arg(QString::fromLatin1(property.typeName() ? property.typeName() : "unknown"));
        }
        for (int i = metaObject->methodOffset(); i < metaObject->methodCount() && i < metaObject->methodOffset() + 16; ++i) {
            const QMetaMethod method = metaObject->method(i);
            lines << QStringLiteral("    method %1")
                .arg(QString::fromLatin1(method.methodSignature()));
        }
    }

    return lines.join(QLatin1Char('\n'));
}

QString probeClipboardBridge()
{
    QObject *documentView = findActiveDocumentView();
    if (!documentView)
        return QStringLiteral("ERROR: active DocumentView not found");

    QObject *clipboardObject = findClipboardObject(documentView);
    if (!clipboardObject)
        return QStringLiteral("ERROR: Clipboard singleton not found");

    QStringList lines;
    lines << QStringLiteral("initial %1").arg(summarizeClipboardState(clipboardObject));

    const bool clearOk = invokeStringArg(clipboardObject, "setTextFromString", QString());
    lines << QStringLiteral("direct-clear ok=%1 %2").arg(clearOk).arg(summarizeClipboardState(clipboardObject));

    auto *plainMime = new QMimeData();
    plainMime->setText(QStringLiteral("system plain probe alpha 41"));
    lines << QStringLiteral("system-plain %1").arg(applyMimeDataToSystemClipboard(plainMime, clipboardObject));

    auto *htmlMime = new QMimeData();
    htmlMime->setHtml(QStringLiteral("<p><b>system html probe beta 42</b></p>"));
    lines << QStringLiteral("system-html %1").arg(applyMimeDataToSystemClipboard(htmlMime, clipboardObject));

    const StagedImagePayload imagePayload = readStagedImagePayload();
    if (!imagePayload.error.isEmpty()) {
        lines << QStringLiteral("system-image ERROR: %1").arg(imagePayload.error);
    } else {
        const QUrl imageUrl = QUrl::fromLocalFile(QString::fromUtf8(kStagedImagePath));

        auto *urlMime = new QMimeData();
        urlMime->setUrls({ imageUrl });
        lines << QStringLiteral("system-url-only %1").arg(applyMimeDataToSystemClipboard(urlMime, clipboardObject));

        auto *imageOnlyMime = new QMimeData();
        imageOnlyMime->setImageData(imagePayload.image);
        imageOnlyMime->setData(imagePayload.mimeName.toUtf8(), imagePayload.bytes);
        lines << QStringLiteral("system-image-only %1").arg(applyMimeDataToSystemClipboard(imageOnlyMime, clipboardObject));

        auto *imageRichMime = new QMimeData();
        imageRichMime->setImageData(imagePayload.image);
        imageRichMime->setData(imagePayload.mimeName.toUtf8(), imagePayload.bytes);
        imageRichMime->setUrls({ imageUrl });
        imageRichMime->setHtml(QStringLiteral("<p><img src=\"%1\" /></p>").arg(imageUrl.toString().toHtmlEscaped()));
        lines << QStringLiteral("system-image-rich %1").arg(applyMimeDataToSystemClipboard(imageRichMime, clipboardObject));
    }

    const bool directSetOk = invokeStringArg(clipboardObject, "setTextFromString", QStringLiteral("direct text probe gamma 43"));
    lines << QStringLiteral("direct-setTextFromString ok=%1 %2").arg(directSetOk).arg(summarizeClipboardState(clipboardObject));

    return lines.join(QLatin1Char('\n'));
}

QString describeMetaObjectFull(QObject *object, const QString &label)
{
    QStringList lines;
    lines << QStringLiteral("[%1] %2").arg(label, describeObject(object));
    if (!object)
        return lines.join(QLatin1Char('\n'));

    const QMetaObject *metaObject = object->metaObject();
    lines << QStringLiteral("  class=%1").arg(QString::fromLatin1(metaObject->className()));
    lines << QStringLiteral("  properties=%1 methods=%2")
        .arg(metaObject->propertyCount() - metaObject->propertyOffset())
        .arg(metaObject->methodCount() - metaObject->methodOffset());

    for (int i = metaObject->propertyOffset(); i < metaObject->propertyCount(); ++i) {
        const QMetaProperty property = metaObject->property(i);
        QString valueSummary = QStringLiteral("<unreadable>");
        if (property.isReadable()) {
            QObject *referencedObject = nullptr;
            valueSummary = formatVariantValue(property.read(object), &referencedObject);
        }
        lines << QStringLiteral("  prop %1 : %2 readable=%3 writable=%4 value=%5")
            .arg(QString::fromLatin1(property.name()))
            .arg(QString::fromLatin1(property.typeName() ? property.typeName() : "unknown"))
            .arg(property.isReadable())
            .arg(property.isWritable())
            .arg(valueSummary);
    }

    for (int i = metaObject->methodOffset(); i < metaObject->methodCount(); ++i) {
        const QMetaMethod method = metaObject->method(i);
        lines << QStringLiteral("  method %1")
            .arg(QString::fromLatin1(method.methodSignature()));
    }

    return lines.join(QLatin1Char('\n'));
}

void scanForPropertyTypeRefs(QObject *object, QSet<QObject *> &visited, QStringList &lines, int depth = 0)
{
    if (!object || visited.contains(object) || depth > 8)
        return;
    visited.insert(object);

    const QMetaObject *metaObject = object->metaObject();
    for (int i = metaObject->propertyOffset(); i < metaObject->propertyCount(); ++i) {
        const QMetaProperty property = metaObject->property(i);
        const QString typeName = QString::fromLatin1(property.typeName() ? property.typeName() : "unknown");
        const QString propertyName = QString::fromLatin1(property.name());
        const QString haystack = (typeName + QStringLiteral(" ") + propertyName).toLower();
        if (!(haystack.contains(QStringLiteral("worker")) || haystack.contains(QStringLiteral("sceneitem")) || haystack.contains(QStringLiteral("clipboard"))))
            continue;

        QString valueSummary = QStringLiteral("<unreadable>");
        QObject *referencedObject = nullptr;
        if (property.isReadable())
            valueSummary = formatVariantValue(property.read(object), &referencedObject);
        lines << QStringLiteral("scan depth=%1 owner=%2 prop=%3 type=%4 value=%5")
            .arg(depth)
            .arg(describeObject(object))
            .arg(propertyName)
            .arg(typeName)
            .arg(valueSummary);
    }

    QSet<QObject *> nextChildren;
    collectChildObjects(object, nextChildren);
    for (QObject *child : nextChildren)
        scanForPropertyTypeRefs(child, visited, lines, depth + 1);
}

void scanForMethodSignatures(QObject *object, QSet<QObject *> &visited, QStringList &lines, int depth = 0)
{
    if (!object || visited.contains(object) || depth > 8)
        return;
    visited.insert(object);

    const QMetaObject *metaObject = object->metaObject();
    for (int i = metaObject->methodOffset(); i < metaObject->methodCount(); ++i) {
        const QMetaMethod method = metaObject->method(i);
        const QString signature = QString::fromLatin1(method.methodSignature());
        const QString lower = signature.toLower();
        if (!(lower.contains(QStringLiteral("documentworker"))
            || lower.contains(QStringLiteral("qlist<std::shared_ptr<sceneitem>>"))
            || lower.contains(QStringLiteral("cloneaddandselectitems"))
            || lower.contains(QStringLiteral("updateimage"))
            || lower.contains(QStringLiteral("create("))
            || lower.contains(QStringLiteral("image"))
            || lower.contains(QStringLiteral("recognition"))
            || lower.contains(QStringLiteral("recognizer"))
            || lower.contains(QStringLiteral("formattedtext"))
            || lower.contains(QStringLiteral("applyformattedtext")))) {
            continue;
        }
        lines << QStringLiteral("method-scan depth=%1 owner=%2 sig=%3")
            .arg(depth)
            .arg(describeObject(object))
            .arg(signature);
    }

    QSet<QObject *> nextChildren;
    collectChildObjects(object, nextChildren);
    for (QObject *child : nextChildren)
        scanForMethodSignatures(child, visited, lines, depth + 1);
}

QString summarizeRecognitionJob(QObject *job)
{
    if (!job)
        return QStringLiteral("job=<null>");

    const QVariant status = job->property("status");
    const QVariant error = job->property("error");
    const QVariant progress = job->property("progress");
    const QVariant finishedPages = job->property("finishedPages");
    const QVariant newPage = job->property("newPage");
    QString plainText = job->property("plainText").toString();
    plainText.replace(QLatin1Char('\n'), QLatin1Char(' '));
    if (plainText.size() > 80)
        plainText = plainText.left(80) + QStringLiteral("…");

    return QStringLiteral("job=%1 status=%2 error=%3 progress=%4 finishedPages=%5 newPage=%6 plainText=%7")
        .arg(describeObject(job))
        .arg(status.isValid() ? status.toInt() : -1)
        .arg(error.isValid() ? error.toInt() : -1)
        .arg(progress.isValid() ? progress.toDouble() : -1.0)
        .arg(finishedPages.isValid() ? finishedPages.toInt() : -1)
        .arg(newPage.isValid() ? newPage.toInt() : -1)
        .arg(plainText.isEmpty() ? QStringLiteral("<empty>") : plainText);
}

QString probeFactories()
{
    QObject *documentView = findActiveDocumentView();
    if (!documentView)
        return QStringLiteral("ERROR: active DocumentView not found");

    QObject *sceneController = nullptr;
    const QVariant sceneControllerValue = documentView->property("sceneController");
    if (sceneControllerValue.isValid() && sceneControllerValue.metaType().flags().testFlag(QMetaType::PointerToQObject))
        sceneController = qvariant_cast<QObject *>(sceneControllerValue);

    QObject *documentObject = nullptr;
    const QVariant documentValue = documentView->property("document");
    if (documentValue.isValid() && documentValue.metaType().flags().testFlag(QMetaType::PointerToQObject))
        documentObject = qvariant_cast<QObject *>(documentValue);

    QObject *workerObject = nullptr;
    if (sceneController) {
        const QVariant workerValue = sceneController->property("worker");
        if (workerValue.isValid() && workerValue.metaType().flags().testFlag(QMetaType::PointerToQObject))
            workerObject = qvariant_cast<QObject *>(workerValue);
    }

    QStringList lines;
    lines << describeMetaObjectFull(documentView, QStringLiteral("DocumentView"));
    lines << describeMetaObjectFull(sceneController, QStringLiteral("SceneController"));
    lines << describeMetaObjectFull(documentObject, QStringLiteral("Document"));
    lines << describeMetaObjectFull(workerObject, QStringLiteral("Worker"));

    QSet<QObject *> visited;
    scanForPropertyTypeRefs(documentView, visited, lines);

    visited.clear();
    scanForMethodSignatures(documentView, visited, lines);

    return lines.join(QLatin1Char('\n'));
}

QString probeRecognizerJob()
{
    QObject *documentView = findActiveDocumentView();
    if (!documentView)
        return QStringLiteral("ERROR: active DocumentView not found");

    QObject *recognizer = findRecognizerObject(documentView);
    if (!recognizer)
        return QStringLiteral("ERROR: Recognizer singleton not found");

    QObject *beforeJob = nullptr;
    {
        const QVariant jobValue = recognizer->property("recognitionJob");
        if (jobValue.isValid() && jobValue.metaType().flags().testFlag(QMetaType::PointerToQObject))
            beforeJob = qvariant_cast<QObject *>(jobValue);
    }

    const int currentPage = documentView->property("currentPage").toInt();
    QString documentId = documentView->property("_tocLoadingDocId").toString();
    if (documentId.isEmpty())
        documentId = documentView->property("_linkLoadingDocId").toString();
    if (documentId.isEmpty()) {
        const QVariant pageSelectionValue = documentView->property("pageSelection");
        if (pageSelectionValue.isValid() && pageSelectionValue.metaType().flags().testFlag(QMetaType::PointerToQObject)) {
            if (QObject *pageSelection = qvariant_cast<QObject *>(pageSelectionValue))
                documentId = pageSelection->property("documentId").toString();
        }
    }

    QList<int> pages;
    pages << currentPage;
    const bool invokeOk = QMetaObject::invokeMethod(
        recognizer,
        "startRecognitionJob",
        Qt::DirectConnection,
        Q_ARG(QString, documentId),
        Q_ARG(QList<int>, pages),
        Q_ARG(QString, QStringLiteral("en_US"))
    );

    for (int i = 0; i < 20; ++i)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

    QObject *afterJob = nullptr;
    {
        const QVariant jobValue = recognizer->property("recognitionJob");
        if (jobValue.isValid() && jobValue.metaType().flags().testFlag(QMetaType::PointerToQObject))
            afterJob = qvariant_cast<QObject *>(jobValue);
    }

    return QStringLiteral("invokeOk=%1 documentId=%2 currentPage=%3 before=%4 after=%5")
        .arg(invokeOk)
        .arg(documentId.isEmpty() ? QStringLiteral("<empty>") : documentId)
        .arg(currentPage)
        .arg(summarizeRecognitionJob(beforeJob))
        .arg(summarizeRecognitionJob(afterJob));
}

QString inspectObjectByNeedle(const QString &needle)
{
    const QString normalizedNeedle = needle.trimmed();
    if (normalizedNeedle.isEmpty())
        return QStringLiteral("ERROR: empty needle");

    QObject *object = findObjectByNeedle(normalizedNeedle);
    if (!object)
        return QStringLiteral("ERROR: object not found for %1").arg(normalizedNeedle);

    return describeMetaObjectFull(object, QStringLiteral("Match:%1").arg(normalizedNeedle));
}

QString documentIdForView(QObject *documentView)
{
    if (!documentView)
        return QString();

    const QString tocLoadingId = documentView->property("_tocLoadingDocId").toString();
    if (!tocLoadingId.isEmpty())
        return tocLoadingId;

    const QString linkLoadingId = documentView->property("_linkLoadingDocId").toString();
    if (!linkLoadingId.isEmpty())
        return linkLoadingId;

    const QVariant pageSelectionValue = documentView->property("pageSelection");
    if (pageSelectionValue.isValid() && pageSelectionValue.metaType().flags().testFlag(QMetaType::PointerToQObject)) {
        if (QObject *pageSelection = qvariant_cast<QObject *>(pageSelectionValue)) {
            const QString documentId = pageSelection->property("documentId").toString();
            if (!documentId.isEmpty())
                return documentId;
        }
    }

    return QString();
}

QJsonObject readJsonObjectFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return QJsonObject();

    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    return document.isObject() ? document.object() : QJsonObject();
}

QJsonObject readMetadataForDocumentId(const QString &documentId)
{
    if (documentId.trimmed().isEmpty())
        return QJsonObject();
    return readJsonObjectFile(QStringLiteral("%1/%2.metadata").arg(QString::fromUtf8(kXochitlRootPath), documentId));
}

QJsonObject readContentForDocumentId(const QString &documentId)
{
    if (documentId.trimmed().isEmpty())
        return QJsonObject();
    return readJsonObjectFile(QStringLiteral("%1/%2.content").arg(QString::fromUtf8(kXochitlRootPath), documentId));
}

QString readVisibleNameForDocumentId(const QString &documentId)
{
    return readMetadataForDocumentId(documentId).value(QStringLiteral("visibleName")).toString();
}

QString readParentIdForDocumentId(const QString &documentId)
{
    return readMetadataForDocumentId(documentId).value(QStringLiteral("parent")).toString();
}

QString readFirstPageIdForDocumentId(const QString &documentId)
{
    const QJsonObject content = readContentForDocumentId(documentId);
    const QJsonObject cPages = content.value(QStringLiteral("cPages")).toObject();
    const QJsonArray pages = cPages.value(QStringLiteral("pages")).toArray();
    if (pages.isEmpty() || !pages.first().isObject())
        return QString();
    return pages.first().toObject().value(QStringLiteral("id")).toString();
}

QString createLinkedNotebook(const QString &requestedTitle)
{
    QObject *sourceView = findActiveDocumentView();
    if (!sourceView)
        return QStringLiteral("ERROR: active DocumentView not found");

    if (!ensureRemarkableLibrary())
        return QStringLiteral("ERROR: remarkable library controller not available");

    const QString sourceDocId = documentIdForView(sourceView);
    if (sourceDocId.isEmpty())
        return QStringLiteral("ERROR: source document id not found");

    const int sourcePage = sourceView->property("currentPage").toInt();
    const QString sourcePageId = sourceView->property("currentPageId").toString();
    const QString targetDocName = requestedTitle.trimmed().isEmpty()
        ? QStringLiteral("Linked note")
        : requestedTitle.trimmed();
    const QString parentId = readParentIdForDocumentId(sourceDocId);

    QString targetDocId;
    const bool createOk = QMetaObject::invokeMethod(
        gRemarkableLibraryController,
        "createDocument",
        Qt::DirectConnection,
        Q_RETURN_ARG(QString, targetDocId),
        Q_ARG(QString, parentId),
        Q_ARG(QString, targetDocName)
    );

    if (!createOk || targetDocId.trimmed().isEmpty())
        return QStringLiteral("ERROR: createDocument() failed");

    QString targetPageId;
    for (int i = 0; i < 160; ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        targetPageId = readFirstPageIdForDocumentId(targetDocId);
        if (!targetPageId.isEmpty())
            break;
        QThread::msleep(25);
    }

    if (targetPageId.isEmpty())
        return QStringLiteral("ERROR: created document has no page id yet");

    QJsonObject result {
        { QStringLiteral("ok"), true },
        { QStringLiteral("createMode"), QStringLiteral("library-controller") },
        { QStringLiteral("sourceDocId"), sourceDocId },
        { QStringLiteral("sourcePage"), sourcePage },
        { QStringLiteral("sourcePageId"), sourcePageId },
        { QStringLiteral("targetDocId"), targetDocId },
        { QStringLiteral("targetPageId"), targetPageId },
        { QStringLiteral("targetPageKey"), targetPageId },
        { QStringLiteral("targetDocName"), readVisibleNameForDocumentId(targetDocId).isEmpty() ? targetDocName : readVisibleNameForDocumentId(targetDocId) },
        { QStringLiteral("reopenOk"), true },
        { QStringLiteral("currentDocId"), sourceDocId }
    };

    return QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact));
}

QString invokeOnGuiThread(const std::function<QString()> &callback)
{
    QCoreApplication *app = QCoreApplication::instance();
    if (!app)
        return QStringLiteral("ERROR: no QCoreApplication");

    if (QThread::currentThread() == app->thread())
        return callback();

    QString result = QStringLiteral("ERROR: invoke failed");
    const bool invoked = QMetaObject::invokeMethod(
        app,
        [&result, &callback]() {
            result = callback();
        },
        Qt::BlockingQueuedConnection
    );

    if (!invoked)
        return QStringLiteral("ERROR: invokeMethod failed");

    return result;
}

char *dupResult(const QString &value)
{
    const QByteArray bytes = value.toUtf8();
    return ::strdup(bytes.constData());
}

} // namespace

extern "C" char *desktopClipboardPing(const char *value)
{
    const QString message = QString::fromUtf8(value ? value : "");
    return dupResult(QStringLiteral("pong:%1").arg(message));
}

extern "C" char *desktopClipboardReadStaged(const char *)
{
    return dupResult(readStagedSummary());
}

extern "C" char *desktopClipboardDumpUi(const char *value)
{
    const QString requestedPath = QString::fromUtf8(value ? value : "");
    return dupResult(invokeOnGuiThread([&requestedPath]() {
        return dumpUiReport(requestedPath);
    }));
}

extern "C" char *desktopClipboardInsertStaged(const char *)
{
    return dupResult(invokeOnGuiThread([]() {
        return insertStagedIntoActiveDocument();
    }));
}

extern "C" char *desktopClipboardDumpGlobals(const char *value)
{
    const QString requestedPath = QString::fromUtf8(value ? value : "");
    return dupResult(invokeOnGuiThread([&requestedPath]() {
        return dumpGlobalObjectsReport(requestedPath);
    }));
}

extern "C" char *desktopClipboardInspectContext(const char *)
{
    return dupResult(invokeOnGuiThread([]() {
        return inspectActiveContextGlobals();
    }));
}

extern "C" char *desktopClipboardLoadStagedImageClipboard(const char *)
{
    return dupResult(invokeOnGuiThread([]() {
        return loadStagedImageClipboard();
    }));
}

extern "C" char *desktopClipboardInsertStagedImage(const char *)
{
    return dupResult(invokeOnGuiThread([]() {
        return insertStagedImageIntoActiveDocument();
    }));
}

extern "C" char *desktopClipboardProbeTypes(const char *)
{
    return dupResult(invokeOnGuiThread([]() {
        return probeRegisteredTypes();
    }));
}

extern "C" char *desktopClipboardProbeClipboardBridge(const char *)
{
    return dupResult(invokeOnGuiThread([]() {
        return probeClipboardBridge();
    }));
}

extern "C" char *desktopClipboardProbeFactories(const char *)
{
    return dupResult(invokeOnGuiThread([]() {
        return probeFactories();
    }));
}

extern "C" char *desktopClipboardProbeRecognizerJob(const char *)
{
    return dupResult(invokeOnGuiThread([]() {
        return probeRecognizerJob();
    }));
}

extern "C" char *desktopClipboardInspectObject(const char *value)
{
    const QString needle = QString::fromUtf8(value ? value : "");
    return dupResult(invokeOnGuiThread([&needle]() {
        return inspectObjectByNeedle(needle);
    }));
}

extern "C" char *desktopClipboardCreateLinkedNotebook(const char *value)
{
    const QString requestedTitle = QString::fromUtf8(value ? value : "");
    return dupResult(invokeOnGuiThread([&requestedTitle]() {
        return createLinkedNotebook(requestedTitle);
    }));
}
