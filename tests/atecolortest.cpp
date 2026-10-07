#include "../src/ui/ate_link.h"
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextStream>
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
  out<<"SUMMARY failures="<<failures<<Qt::endl;return failures;
}
