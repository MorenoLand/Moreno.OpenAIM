#include "../src/ui/ate_link.h"
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextStream>
#include "../src/ui/native_dialog.h"
#include "../src/ui/buddy_info_window.h"
#include <QThread>
#include <QSettings>
#include "../src/ui/native_preferences.h"
int main(int argc,char **argv) {
  QGuiApplication::setOrganizationName(QStringLiteral("MorenoLand"));QGuiApplication::setApplicationName(QStringLiteral("OpenAIM-colorcheck"));QGuiApplication app(argc,argv);QTextStream out(stdout);int failures=0;
  auto check=[&](const QString &name,bool ok){out<<(ok?"PASS ":"FAIL ")<<name<<Qt::endl;if(!ok)++failures;};
  QTextDocument document;document.setTextWidth(320);QTextCursor cursor(&document);
  ate::insertMessageHtml(cursor,QStringLiteral("<HTML><BODY BGCOLOR='#000000'><P><FONT COLOR='#ffff00'>yellow</FONT></P><P>default</P></BODY></HTML>"),Qt::white);
  bool backgrounds=true,yellow=false,readable=false;for(QTextBlock block=document.begin();block.isValid();block=block.next()){if(block.text().isEmpty())continue;backgrounds=backgrounds&&block.blockFormat().background().color()==Qt::black;for(auto it=block.begin();!it.atEnd();++it){const QTextFragment fragment=it.fragment();if(fragment.text()==QStringLiteral("yellow"))yellow=fragment.charFormat().foreground().color()==Qt::yellow;if(fragment.text()==QStringLiteral("default"))readable=fragment.charFormat().foreground().color()==Qt::white;}}
  check(QStringLiteral("BODY background covers multiline message"),backgrounds);check(QStringLiteral("explicit yellow foreground preserved"),yellow);check(QStringLiteral("unstyled dark-background text readable"),readable);
  QImage image(340,180,QImage::Format_ARGB32);image.fill(Qt::white);{QPainter painter(&image);document.drawContents(&painter);}int black=0,yellowPixels=0;for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x){const QColor pixel=image.pixelColor(x,y);if(pixel==Qt::black)++black;if(pixel.red()>150&&pixel.green()>150&&pixel.blue()<80)++yellowPixels;}
  check(QStringLiteral("actual QTextDocument rendering shows black and yellow"),black>1000&&yellowPixels>0);
  cursor.movePosition(QTextCursor::End);cursor.insertBlock();ate::insertMessageHtml(cursor,QStringLiteral("white"),Qt::white);check(QStringLiteral("next message does not inherit black background"),cursor.blockFormat().background().color()==Qt::white);check(QStringLiteral("next message default text is black"),cursor.charFormat().foreground().color()==Qt::black);
  QTextDocument explicitDocument;QTextCursor explicitCursor(&explicitDocument);ate::insertMessageHtml(explicitCursor,QStringLiteral("<BODY BGCOLOR=black><P style='background-color:#ff0000'><FONT COLOR='#0000ff'>explicit</FONT></P></BODY>"),Qt::white);bool explicitBackground=false,explicitForeground=false;for(QTextBlock block=explicitDocument.begin();block.isValid();block=block.next())if(block.text().contains(QStringLiteral("explicit"))){explicitBackground=block.blockFormat().background().color()==Qt::red;for(auto it=block.begin();!it.atEnd();++it)if(it.fragment().text().contains(QStringLiteral("explicit")))explicitForeground=it.fragment().charFormat().foreground().color()==Qt::blue;}
  check(QStringLiteral("explicit paragraph background preserved"),explicitBackground);check(QStringLiteral("explicit blue foreground preserved"),explicitForeground);
  QTextDocument blackDocument;QTextCursor blackCursor(&blackDocument);QTextCharFormat blackFormat;blackFormat.setForeground(Qt::black);blackCursor.insertText(QStringLiteral("black"),blackFormat);check(QStringLiteral("explicit black foreground preserved in outgoing HTML"),ate::html(blackDocument,Qt::white).contains(QStringLiteral("COLOR=\"#000000\"")));
  QTextDocument yellowDocument;QTextCursor yellowCursor(&yellowDocument);ate::insertMessageHtml(yellowCursor,QStringLiteral("<BODY BGCOLOR=yellow>default</BODY>"),Qt::white);check(QStringLiteral("bright yellow background uses readable default text"),yellowCursor.charFormat().foreground().color()==Qt::black);
#ifdef Q_OS_WIN
  bool completed=false;HWND dialog=createOriginalDialog(nullptr,230,{},[&,marker=QString(512,QLatin1Char('x'))](HWND window,int id,int){if(id!=IDCANCEL)return false;DestroyWindow(window);completed=marker.size()==512;return true;});if(dialog)SendMessageW(dialog,WM_COMMAND,MAKEWPARAM(IDCANCEL,BN_CLICKED),0);check(QStringLiteral("modeless callback survives destroying its dialog"),dialog&&completed&&!IsWindow(dialog));
  OscarClient client;BuddyInfoWindow::open(&client,QString(),{});BuddyInfoWindow *info=nullptr;HWND ok=nullptr,close=nullptr;for(int i=0;i<30&&!ok;++i){QCoreApplication::processEvents();for(QWindow *window:QGuiApplication::topLevelWindows())if(auto *candidate=dynamic_cast<BuddyInfoWindow*>(window)){info=candidate;HWND owner=reinterpret_cast<HWND>(info->winId());ok=GetDlgItem(owner,0x11);close=GetDlgItem(owner,2);break;}if(!ok)QThread::msleep(10);}check(QStringLiteral("Buddy Info buttons have no redundant window border"),ok&&close&&!(GetWindowLongPtrW(ok,GWL_STYLE)&WS_BORDER)&&!(GetWindowLongPtrW(close,GWL_STYLE)&WS_BORDER));if(info)info->close();QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
  QSettings settings;settings.setValue(QStringLiteral("IM/IMDirectNoStartDlg"),true);settings.setValue(QStringLiteral("IM/IMDirectNoStatusDlg"),true);settings.sync();NativePreferences preferences(nullptr);preferences.activatePage(276);QCoreApplication::processEvents();HWND host=GetActiveWindow();HWND checks[2]{};EnumChildWindows(host,[](HWND window,LPARAM data)->BOOL{auto *controls=reinterpret_cast<HWND*>(data);int id=GetDlgCtrlID(window);if(id==825)controls[0]=window;if(id==826)controls[1]=window;return TRUE;},reinterpret_cast<LPARAM>(checks));check(QStringLiteral("IM Image Display boxes load inverted suppression settings"),checks[0]&&checks[1]&&SendMessageW(checks[0],BM_GETCHECK,0,0)==BST_UNCHECKED&&SendMessageW(checks[1],BM_GETCHECK,0,0)==BST_UNCHECKED);if(checks[0]&&checks[1]){SendMessageW(checks[0],BM_SETCHECK,BST_CHECKED,0);SendMessageW(checks[1],BM_SETCHECK,BST_CHECKED,0);SendMessageW(host,WM_COMMAND,MAKEWPARAM(264,BN_CLICKED),0);settings.sync();check(QStringLiteral("IM Image Apply re-enables start and status dialogs"),!settings.value(QStringLiteral("IM/IMDirectNoStartDlg")).toBool()&&!settings.value(QStringLiteral("IM/IMDirectNoStatusDlg")).toBool());}preferences.close();
#endif
  out<<"SUMMARY failures="<<failures<<Qt::endl;return failures;
}
