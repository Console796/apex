#pragma once

#include <QJsonObject>

class QWidget;
class QMouseEvent;
class QObject;

namespace apex::ui {

// Human-interface JSONL tracing. The environment is read once; all calls are
// cheap branches when APEX_INPUT_LOG is unset. Call after QApplication exists.
bool inputLogEnabled();
void inputTraceInitialize();
void inputTraceEvent(const char* type, QJsonObject fields = {});
void inputTraceSetState(QJsonObject state);
int inputTraceMarker();
void inputTraceWindowStart(const QWidget& window);
void inputTraceMouse(QObject* receiver, const QMouseEvent& event);

}  // namespace apex::ui
