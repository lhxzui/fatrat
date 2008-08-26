/*
FatRat download manager
http://fatrat.dolezel.info

Copyright (C) 2006-2008 Lubos Dolezel <lubos a dolezel.info>

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
version 2 as published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include "config.h"

#include "HttpService.h"
#include "Settings.h"
#include "RuntimeException.h"
#include "SettingsWebForm.h"
#include "SpeedGraph.h"
#include "Transfer.h"
#include "Queue.h"
#include "fatrat.h"
#include "Logger.h"
#include "dbus/DbusImpl.h"

#include <QSettings>
#include <QtDebug>
#include <QStringList>
#include <QFile>
#include <QUrl>
#include <QDir>
#include <QTcpSocket>
#include <QBuffer>
#include <QImage>
#include <QPainter>
#include <QFileInfo>
#include <QScriptEngine>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <cstdlib>
#include <locale.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

extern QList<Queue*> g_queues;
extern QReadWriteLock g_queuesLock;
extern QSettings* g_settings;
extern QVector<EngineEntry> g_enginesDownload;

static const int SOCKET_TIMEOUT = 10;

HttpService* HttpService::m_instance = 0;

#define HTTP_HEADERS "Server: FatRat/" VERSION "\r\n"

HttpService::HttpService()
{
	m_instance = this;
	initScriptEngine();
	applySettings();
	
	SettingsItem si;
	si.lpfnCreate = SettingsWebForm::create;
	si.icon = QIcon(":/fatrat/webinterface.png");
	
	addSettingsPage(si);
}

HttpService::~HttpService()
{
	m_instance = 0;
	if(isRunning())
	{
		m_bAbort = true;
		wait();
		close(m_server);
	}
}

void HttpService::applySettings()
{
	bool enable = getSettingsValue("remote/enable").toBool();
	if(enable && !isRunning())
	{
		try
		{
			m_bAbort = false;
			setup();
			start();
		}
		catch(const RuntimeException& e)
		{
			qDebug() << e.what();
			Logger::global()->enterLogMessage("HttpService", e.what());
		}
	}
	else if(!enable && isRunning())
	{
		m_bAbort = true;
		wait();
		close(m_server);
		m_server = 0;
	}
}

void HttpService::setup()
{
	quint16 port;
	bool bIPv6 = true;
	
	port = getSettingsValue("remote/port").toUInt();
	
	if((m_server = socket(PF_INET6, SOCK_STREAM, 0)) < 0)
	{
		if(errno == EAFNOSUPPORT)
		{
			bIPv6 = false;
			
			if((m_server = socket(PF_INET, SOCK_STREAM, 0)) < 0)
				throwErrno();
		}
		else
			throwErrno();
	}
	
	int reuse = 1;
	setsockopt(m_server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
	
	if(bIPv6)
	{
		sockaddr_in6 addr6;
		memset(&addr6, 0, sizeof addr6);
		addr6.sin6_family = AF_INET6;
		addr6.sin6_port = htons(port);
		addr6.sin6_addr = in6addr_any;
		
		if(bind(m_server, (sockaddr*) &addr6, sizeof addr6) < 0)
			throwErrno();
	}
	else
	{
		sockaddr_in addr4;
		addr4.sin_family = AF_INET;
		addr4.sin_port = htons(port);
		addr4.sin_addr.s_addr = INADDR_ANY;
		
		if(bind(m_server, (sockaddr*) &addr4, sizeof addr4) < 0)
			throwErrno();
	}
	
	if(listen(m_server, 10) < 0)
		throwErrno();
	
	Logger::global()->enterLogMessage("HttpService", tr("Listening on port %1").arg(port));
}

void HttpService::throwErrno()
{
	QString err = QString::fromUtf8(strerror(errno));
	throw RuntimeException(err);
}

void HttpService::run()
{
	int ep;
	epoll_event ev, events[10];
	
	ep = epoll_create(30);
	
	ev.events = EPOLLIN | EPOLLPRI | EPOLLHUP | EPOLLERR;
	ev.data.fd = m_server;
	
	epoll_ctl(ep, EPOLL_CTL_ADD, m_server, &ev);
	
	while(!m_bAbort)
	{
		int nfds = epoll_wait(ep, events, 10, 500);
		
		for(int i=0;i<nfds;i++)
		{
			int fd = events[i].data.fd;
			
			if(fd == m_server)
			{
				sockaddr addr;
				socklen_t len = sizeof addr;
				int client = accept(m_server, &addr, &len);
				
				if(client < 0)
					continue;
				
				int arg = fcntl(client, F_GETFL);
				fcntl(client, F_SETFL, arg | O_NONBLOCK);
				
				ev.events = EPOLLIN | EPOLLHUP | EPOLLERR | EPOLLOUT;
				ev.data.fd = client;
				
				epoll_ctl(ep, EPOLL_CTL_ADD, client, &ev);
			}
			else
			{
				bool bTerminate = false;
				if((events[i].events & EPOLLERR || events[i].events & EPOLLHUP))
					bTerminate = true;
				else if(events[i].events & EPOLLIN && !processClientRead(fd))
					bTerminate = true;
				else if(events[i].events & EPOLLOUT && !processClientWrite(fd))
					bTerminate = true;
				
				if(bTerminate)
				{
					freeClient(fd, ep);
					m_clients.remove(fd);
				}
			}
		}
		
		for(QMap<int, ClientData>::iterator it = m_clients.begin(); it != m_clients.end();)
		{
			if(time(0) - it.value().lastData > SOCKET_TIMEOUT)
			{
				freeClient(it.key(), ep);
				it = m_clients.erase(it);
			}
			else
				it++;
		}
	}
	
	for(QMap<int, ClientData>::iterator it = m_clients.begin(); it != m_clients.end();)
	{
		freeClient(it.key(), ep);
		it = m_clients.erase(it);
	}
	
	close(ep);
}

void HttpService::freeClient(int fd, int ep)
{
	ClientData& data = m_clients[fd];
	delete data.file;
	delete data.buffer;
	epoll_ctl(ep, EPOLL_CTL_DEL, fd, 0);
	close(fd);
}

long HttpService::contentLength(const QList<QByteArray>& data)
{
	for(int i=0;i<data.size();i++)
	{
		if(!strncasecmp(data[i].constData(), "Content-Length: ", 16))
			return atol(data[i].constData()+16);
	}
	return 0;
}

bool HttpService::processClientRead(int fd)
{
	char buffer[4096];
	ClientData& data = m_clients[fd];
	
	while(true)
	{
		ssize_t bytes = read(fd, buffer, sizeof buffer);
		
		if(bytes < 0)
		{
			if(errno == EAGAIN)
				break;
			else
				return false;
		}
		else if(!bytes)
			return false;
		
		char* start = buffer;
		time(&data.lastData);
		
		// parse lines
		while(bytes)
		{
			// processing POST data
			if(!data.requests.isEmpty() && data.requests.back().contentLength)
			{
				HttpRequest& rq = data.requests.back();
				long cp = qMin<long>(rq.contentLength, bytes);
				
				rq.postData += QByteArray(start, cp);
				rq.contentLength -= cp;
				
				bytes -= cp;
				start += cp;
			}
			else // processing HTTP headers
			{
				if(data.incoming.isEmpty())
					data.incoming << QByteArray();
				
				char* p = (char*) memchr(start, '\n', bytes);
				
				if(!p)
				{
					data.incoming.back() += QByteArray(start, bytes);
					bytes = 0;
				}
				else
				{
					ssize_t now = p-start+1;
					
					data.incoming.back() += QByteArray(start, p-start+1);
					bytes -= now;
					start = p + 1;
					
					if(data.incoming.back() == "\r\n") // end of the request
					{
						HttpRequest rq;
						
						rq.lines = data.incoming;
						rq.contentLength = contentLength(rq.lines);
						
						data.incoming.clear();
						data.requests << rq;
					}
					else
						data.incoming << QByteArray();
				}
			}
		}
	}
	
	for(int i=0;i<data.requests.size();)
	{
		if(!data.requests[i].contentLength)
		{
			serveClient(fd);
			data.requests.removeAt(i);
		}
		else
			break;
	}
	
	return true;
}

bool HttpService::processClientWrite(int fd)
{
	ClientData& data = m_clients[fd];
	time(&data.lastData);
	
	if(data.file)
	{
		if(!data.file->atEnd() && data.file->isOpen())
		{
			QByteArray buf = data.file->read(2*1024);
			
			return write(fd, buf.constData(), buf.size()) == buf.size();
		}
	}
	else if(data.buffer && !data.buffer->isEmpty())
	{
		QByteArray ar;
		unsigned long bytes = 8*1024;
		
		ar.resize(bytes);
		data.buffer->getData(ar.data(), &bytes);
		
		int written = write(fd, ar.constData(), bytes);
		if(written <= 0)
			return false;
		else if(written < int(bytes))
			data.buffer->putBack(ar.constData()+written, bytes-written);
	}
	
	return true;
}

bool HttpService::authenitcate(const QList<QByteArray>& data)
{
	for(int i=0;i<data.size();i++)
	{
		if(!strncasecmp(data[i].constData(), "Authorization: Basic ", 21))
		{
			QList<QByteArray> auth = QByteArray::fromBase64(data[i].mid(21)).split(':');
			if(auth.size() != 2)
				return false;
			
			return auth[1] == getSettingsValue("remote/password").toString().toUtf8();
		}
	}
	return false;
}

QByteArray HttpService::progressBar(QByteArray queryString)
{
	const int WIDTH = 150, HEIGHT = 20;
	QBuffer buf;
	QImage image(QSize(WIDTH, HEIGHT), QImage::Format_RGB32);
	QImage pg(":/other/progressbar.png");
	QPainter painter;
	float pct = 0;
	
	if(queryString != "?")
		pct = atof(queryString.constData()) / 100.f;
	
	painter.begin(&image);
	painter.fillRect(0, 0, WIDTH, HEIGHT, QBrush(Qt::white));
	int pts = WIDTH*pct;
	
	for(int done=0;done<pts;)
	{
		int w = qMin<int>(pts-done, pg.width());
		painter.drawImage(QRect(done, 0, w, HEIGHT), pg, QRect(0, 0, w, HEIGHT));
		done += w;
	}
	
	painter.setPen(Qt::gray);
	painter.drawRect(0, 0, WIDTH-1, HEIGHT-1);
	
	QFont font = painter.font();
	font.setBold(true);
	
	painter.setClipRect(pts, 0, WIDTH, HEIGHT);
	painter.setFont(font);
	painter.setPen(Qt::black);
	painter.drawText(QRect(0, 0, WIDTH, HEIGHT), Qt::AlignCenter, queryString);
	
	painter.setClipRect(0, 0, pts, HEIGHT);
	painter.setPen(Qt::white);
	painter.drawText(QRect(0, 0, WIDTH, HEIGHT), Qt::AlignCenter, queryString);
	
	painter.end();
	
	image.save(&buf, "PNG");
	return buf.data();
}

QByteArray HttpService::graph(QString queryString)
{
	QBuffer buf;
	QImage image(QSize(640, 480), QImage::Format_RGB32);
	
	QReadLocker locker(&g_queuesLock);
	Queue* q;
	Transfer* t;
	
	findTransfer(queryString, &q, &t);
	if(!q || !t)
		return QByteArray();
	
	SpeedGraph::draw(t, QSize(640, 480), &image);
	
	q->unlock();
	g_queuesLock.unlock();
	
	image.save(&buf, "PNG");
	return buf.data();
}

void HttpService::serveClient(int fd)
{
	char buffer[4096];
	bool bHead, bGet, bPost, bAuthFail = false; // send 401
	qint64 fileSize = -1;
	QDateTime modTime;
	QByteArray fileName, queryString, postData, disposition;
	ClientData& data = m_clients[fd];
	HttpRequest& rq = data.requests[0];
	QList<QByteArray> firstLine = rq.lines[0].split(' ');
	
	bHead = firstLine[0] == "HEAD";
	bGet = firstLine[0] == "GET";
	bPost = firstLine[0] == "POST";
	
	qDebug() << postData;
	if(bGet || bHead || bPost)
	{
		if(firstLine.size() >= 2)
			fileName = firstLine[1];
		if(bPost)
			postData = rq.postData;
	}
	
	int q = fileName.indexOf('?');
	if(q != -1)
	{
		queryString = fileName.mid(q+1);
		fileName.resize(q);
	}
	
	try
	{
		if(fileName.isEmpty() || fileName.indexOf("/..") != -1 || fileName.indexOf("../") != -1)
		{
			const char* msg = "HTTP/1.0 400 Bad Request\r\n" HTTP_HEADERS "\r\n";
			write(fd, msg, strlen(msg));
			return;
		}
		
		if(fileName == "/")
			fileName = "/index.qsp";
		
		if(fileName.endsWith(".qsp"))
		{
			fileName.prepend(DATA_LOCATION "/data/remote");
			
			if(authenitcate(rq.lines))
			{
				QFile file(fileName);
				if(file.open(QIODevice::ReadOnly))
				{
					data.buffer = new OutputBuffer;
					
					qDebug() << "Executing" << fileName;
					interpretScript(&file, data.buffer, queryString, postData);
					fileSize = data.buffer->size();
				}
			}
			else if(!bHead)
				bAuthFail = true;
		}
		else if(fileName.startsWith("/generate/"))
		{
			if(!bHead)
			{
				QByteArray png;
				QString q = urlDecode(queryString);
				data.buffer = new OutputBuffer;
				
				if(fileName.endsWith("/progress.png"))
					png = progressBar(q.toLatin1());
				else if(fileName.endsWith("/graph.png"))
					png = graph(q);
				else
				{
					delete data.buffer;
					data.buffer = 0;
				}
				
				if(data.buffer)
				{
					qDebug() << "Storing" << png.size() << "bytes";
					data.buffer->putData(png.constData(), png.size());
					fileSize = data.buffer->size();
				}
			}
		}
		else if(fileName == "/download")
		{
			QMap<QString,QString> gets = processQueryString(queryString);
			QString t;
			QString path;
			Queue* qo = 0;
			Transfer* to = 0;
			
			try
			{
				if(!authenitcate(rq.lines))
				{
					bAuthFail = true;
					throw 0;
				}
				
				t = gets["transfer"];
				path = gets["path"];
				
				if(path.indexOf("/..") != -1 || path.indexOf("../") != -1)
					throw 0;
				
				findTransfer(t, &qo, &to);
				if(!qo || !to)
					throw 0;
				
				path.prepend(to->dataPath(true));
				
				QFileInfo info(path);
				if(!info.exists())
					throw 0;
				
				modTime = info.lastModified();
				fileSize = info.size();
			}
			catch(...)
			{
				path.clear();
			}
			
			if(qo && to)
			{
				qo->unlock();
				g_queuesLock.unlock();
			}
			
			if(!path.isEmpty() && !bHead)
			{
				int last = path.lastIndexOf('/');
				if(last != -1)
				{
					disposition = path.mid(last+1).toUtf8();
					if(disposition.size() > 100)
						disposition.resize(100);
				}
				
				data.file = new QFile(path);
				data.file->open(QIODevice::ReadOnly);
			}
		}
		else
		{
			fileName.prepend(DATA_LOCATION "/data/remote");
			qDebug() << "Opening" << fileName;
			
			QFileInfo info(fileName);
			if(info.exists())
			{
				modTime = info.lastModified();
				fileSize = info.size();
				
				if(!bHead)
				{
					data.file = new QFile(fileName);
					data.file->open(QIODevice::ReadOnly);
				}
			}
			else
				throw "404 Not Found";
		}
		
		if(bAuthFail)
		{
			strcpy(buffer, "HTTP/1.0 401 Unauthorized\r\nWWW-Authenticate: Basic realm=\"FatRat web interface\""
					"\r\nContent-Length: 16\r\n" HTTP_HEADERS "\r\n401 Unauthorized");
		}
		else if(bHead && fileSize < 0)
		{
			strcpy(buffer, "HTTP/1.0 200 OK\r\n" HTTP_HEADERS
					"Cache-Control: no-cache\r\nPragma: no-cache\r\n\r\n");
		}
		else if(fileSize != -1)
		{
			sprintf(buffer, "HTTP/1.0 200 OK\r\n" HTTP_HEADERS "Content-Length: %lld\r\n", fileSize);
			
			if(!modTime.isNull())
			{
				char time[100];
				time_t t = modTime.toTime_t();
				struct tm tt;
				char locale[20];
				
				gmtime_r(&t, &tt);
				strcpy(locale, setlocale(LC_TIME, 0));
				setlocale(LC_TIME, "C");
				
				strftime(time, sizeof(time), "Last-Modified: %a, %d %b %Y %T %Z\r\n", &tt);
				
				setlocale(LC_TIME, locale);
				strcat(buffer, time);
			}
			else
			{
				strcat(buffer, "Cache-Control: no-cache\r\nPragma: no-cache\r\n");
			}
			
			if(!disposition.isEmpty())
			{
				char disp[200];
				sprintf(disp, "Content-Disposition: attachment; filename=\"%s\"\r\n", disposition.constData());
				strcat(buffer, disp);
			}
			
			strcat(buffer, "\r\n");
		}
	}
	catch(const char* errorMsg)
	{
		sprintf(buffer, "HTTP/1.0 %s\r\nContent-Length: %ld\r\n%s\r\n%s", errorMsg, strlen(errorMsg), HTTP_HEADERS, errorMsg);
	}
	
	write(fd, buffer, strlen(buffer));
	
	processClientWrite(fd);
}

QMap<QString,QString> HttpService::processQueryString(QByteArray queryString)
{
	QMap<QString,QString> result;
	QStringList s = urlDecode(queryString).split('&');
	foreach(QString e, s)
	{
		int p = e.indexOf('=');
		if(p < 0)
			continue;
		result[e.left(p)] = e.mid(p+1);
	}
	return result;
}

QScriptValue HttpService::convertQueryString(const QMap<QString,QString>& queryString)
{
	QScriptValue retval = m_engine->newObject();
	
	for(QMap<QString,QString>::const_iterator it = queryString.constBegin();
		   it != queryString.constEnd();it++)
	{
		QScriptValue value = m_engine->toScriptValue(it.value());
		QRegExp reArray("(\\w+)\\[(\\d+)\\]");
		//QRegExp reObject("(\\w+)\\.(\\w+)");
		
		QString name = it.key();
		name.replace('.', '_');
		
		if(reArray.exactMatch(name))
		{
			QScriptValue v = retval.property(reArray.cap(1));
			if(v.isUndefined() || !v.isArray())
			{
				v = m_engine->newArray();
				v.setProperty(reArray.cap(2).toInt(), value);
				retval.setProperty(reArray.cap(1), v);
			}
			else
				v.setProperty(reArray.cap(2).toInt(), value);
		}
		/*else if(reObject.exactMatch(name))
		{
			QScriptValue v = retval.property(reArray.cap(1));
			if(v.isUndefined() || !v.isArray())
			{
				v = m_engine->newObject();
				v.setProperty(reArray.cap(2), value);
				retval.setProperty(reArray.cap(1), v);
			}
			else
				v.setProperty(reArray.cap(2), value);
		}*/
		else
			retval.setProperty(name, value);
	}
	return retval;
}

QString HttpService::urlDecode(QByteArray arr)
{
	arr.replace('+', ' ');
	return QUrl::fromPercentEncoding(arr);
}

QScriptValue pagePrintFunction(QScriptContext* context, QScriptEngine* engine)
{
	QString result;
	for(int i = 0; i < context->argumentCount(); i++)
	{
		if(i > 0)
			result.append(' ');
		result.append(context->argument(i).toString());
	}

	QScriptValue calleeData = context->callee().data();
	OutputBuffer* buffer = qobject_cast<OutputBuffer*>(calleeData.toQObject());
	QByteArray b = result.toUtf8();
	
	buffer->putData(b.constData(), b.size());

	return engine->undefinedValue();
}

QScriptValue formatSizeFunction(QScriptContext* context, QScriptEngine* engine)
{
	qulonglong size;
	bool persec;
	
	if(context->argumentCount() != 2)
	{
		context->throwError("formatSize(): wrong argument count");
		return engine->undefinedValue();
	}
	
	size = context->argument(0).toNumber();
	persec = context->argument(1).toBoolean();
	
	return engine->toScriptValue(formatSize(size, persec));
}

QScriptValue formatTimeFunction(QScriptContext* context, QScriptEngine* engine)
{
	qulonglong secs;
	
	if(context->argumentCount() != 1)
	{
		context->throwError("formatTime(): wrong argument count");
		return engine->undefinedValue();
	}
	
	secs = context->argument(0).toNumber();
	
	return engine->toScriptValue(formatTime(secs));
}

QScriptValue fileInfoFunction(QScriptContext* context, QScriptEngine* engine)
{
	QString file;
	
	if(context->argumentCount() != 1)
	{
		context->throwError("fileInfo(): wrong argument count");
		return engine->undefinedValue();
	}
	
	file = context->argument(0).toString();
	if(file.indexOf("/..") != -1 || file.indexOf("../") != -1)
	{
		context->throwError("fileInfo(): security alert");
		return engine->undefinedValue();
	}
	
	QFileInfo info(file);
	if(!info.exists())
		return QScriptValue(engine, QScriptValue::NullValue);
	
	QScriptValue retval = engine->newObject();
	retval.setProperty("directory", engine->toScriptValue(info.isDir()));
	retval.setProperty("size", engine->toScriptValue(info.size()));
	
	return retval;
}

QScriptValue listDirectoryFunction(QScriptContext* context, QScriptEngine* engine)
{
	QString dir;
	
	if(context->argumentCount() != 1)
	{
		context->throwError("listDirectory(): wrong argument count");
		return engine->undefinedValue();
	}
	
	dir = context->argument(0).toString();
	if(dir.indexOf("/..") != -1 || dir.indexOf("../") != -1)
	{
		context->throwError("listDirectory(): security alert");
		return engine->undefinedValue();
	}
	
	QDir ddir(dir);
	if(!ddir.exists())
		return QScriptValue(engine, QScriptValue::NullValue);
	
	QStringList ct = ddir.entryList(QDir::NoDotAndDotDot|QDir::Files|QDir::Dirs);
	return engine->toScriptValue(ct);
}

QScriptValue transferSpeedFunction(QScriptContext* context, QScriptEngine* engine)
{
	if(context->argumentCount() != 0)
	{
		context->throwError("Transfer.speed(): wrong argument count");
		return engine->undefinedValue();
	}
	Transfer* t = (Transfer*) context->thisObject().toQObject();
	int down, up;
	t->speeds(down, up);
	
	QScriptValue v = engine->newObject();
	v.setProperty("down", engine->toScriptValue(down));
	v.setProperty("up", engine->toScriptValue(up));
	
	return v;
}

QScriptValue transferSpeedLimitsFunction(QScriptContext* context, QScriptEngine* engine)
{
	if(context->argumentCount() != 0)
	{
		context->throwError("Transfer.speedLimits(): wrong argument count");
		return engine->undefinedValue();
	}
	Transfer* t = (Transfer*) context->thisObject().toQObject();
	int down, up;
	t->userSpeedLimits(down, up);
	
	QScriptValue v = engine->newObject();
	v.setProperty("down", engine->toScriptValue(down));
	v.setProperty("up", engine->toScriptValue(up));
	
	return v;
}

QScriptValue queueSpeedLimitsFunction(QScriptContext* context, QScriptEngine* engine)
{
	if(context->argumentCount() != 0)
	{
		context->throwError("Queue.speedLimits(): wrong argument count");
		return engine->undefinedValue();
	}
	Queue* t = (Queue*) context->thisObject().toQObject();
	int down, up;
	t->speedLimits(down, up);
	
	QScriptValue v = engine->newObject();
	v.setProperty("down", engine->toScriptValue(down));
	v.setProperty("up", engine->toScriptValue(up));
	
	return v;
}

QScriptValue queueTransferLimitsFunction(QScriptContext* context, QScriptEngine* engine)
{
	if(context->argumentCount() != 0)
	{
		context->throwError("Queue.transferLimits(): wrong argument count");
		return engine->undefinedValue();
	}
	Queue* t = (Queue*) context->thisObject().toQObject();
	int down, up;
	t->transferLimits(down, up);
	
	QScriptValue v = engine->newObject();
	v.setProperty("down", engine->toScriptValue(down));
	v.setProperty("up", engine->toScriptValue(up));
	
	return v;
}

QScriptValue transferTimeLeftFunction(QScriptContext* context, QScriptEngine* engine)
{
	if(context->argumentCount() != 0)
	{
		context->throwError("Transfer.speed(): wrong argument count");
		return engine->undefinedValue();
	}
	
	Transfer* t = (Transfer*) context->thisObject().toQObject();
	Transfer::Mode mode = t->primaryMode();
	QString time;
	int down,up;
	qint64 total, done;
	
	t->speeds(down,up);
	total = t->total();
	done = t->done();
	
	if(total)
	{
		if(mode == Transfer::Download && down)
			time = formatTime((total-done)/down);
		else if(mode == Transfer::Upload && up)
			time = formatTime((total-done)/up);
	}
	
	return engine->toScriptValue(time);
}

QScriptValue getSettingsValueFunction(QScriptContext* context, QScriptEngine* engine)
{
	if(context->argumentCount() != 1)
	{
		context->throwError("getSettingsValue(): wrong argument count");
		return engine->undefinedValue();
	}
	
	QVariant r = getSettingsValue(context->argument(0).toString());
	return engine->toScriptValue(r);
}

QScriptValue addQueueFunction(QScriptContext* context, QScriptEngine* engine)
{
	if(context->argumentCount() != 0)
	{
		context->throwError("addQueue(): wrong argument count");
		return engine->undefinedValue();
	}
	
	Queue* queue = new Queue;
	QScriptValue retval = engine->newQObject(queue);
	QScriptValue fun, fun2;
	
	fun = engine->newFunction(queueSpeedLimitsFunction);
	fun2 = engine->newFunction(queueTransferLimitsFunction);
	
	retval.setProperty("speedLimits", fun);
	retval.setProperty("transferLimits", fun2);
	
	g_queues << queue;
	
	return retval;
}

QScriptValue addTransfersFunction(QScriptContext* context, QScriptEngine* engine)
{
	if(context->argumentCount() != 4)
	{
		context->throwError("addTransfers(): wrong argument count");
		return engine->undefinedValue();
	}
	
	QString uris = context->argument(0).toString();
	QString target = context->argument(1).toString();
	int classIndex = context->argument(2).toInt32();
	QString queueUUID = context->argument(3).toString();
	
	QList<Transfer*> listTransfers;
	try
	{
		QStringList listUris;
		int queueIndex = -1;
		
		if(uris.isEmpty())
			throw RuntimeException("No URIs were passed");
		
		if(!getSettingsValue("link_separator").toInt())
			listUris = uris.split('\n', QString::SkipEmptyParts);
		else
			listUris = uris.split(QRegExp("\\s+"), QString::SkipEmptyParts);
		
		for(int i=0;i<g_queues.size();i++)
		{
			if(g_queues[i]->uuid() == queueUUID)
			{
				queueIndex = i;
				break;
			}
		}
		if(queueIndex < 0)
			throw RuntimeException("queueUUID is invalid");
		
		foreach(QString uri, listUris)
		{
			const EngineEntry* _class = 0;
			if(classIndex < 0)
			{
				Transfer::BestEngine eng;
				
				eng = Transfer::bestEngine(uri, Transfer::Download);
				
				if(eng.nClass < 0)
					throw RuntimeException("The URI wasn't accepted by any class: "+uri);
				else
					_class = eng.engine;
			}
			else
				_class = &g_enginesDownload[classIndex];
		
			Transfer* t = _class->lpfnCreate();
			
			if(!t)
				throw RuntimeException("Failed to create an instance of the chosen class");
			
			listTransfers << t;
			t->init(uri, target);
		}
		
		g_queues[queueIndex]->add(listTransfers);
	}
	catch(const RuntimeException& e)
	{
		qDeleteAll(listTransfers);
		return engine->toScriptValue(e.what());
	}
	
	QScriptValue transfers = engine->newArray(listTransfers.size());
	for(int i=0;i<listTransfers.size();i++)
		transfers.setProperty(i, engine->newQObject(listTransfers[i]));
	return transfers;
}

Q_DECLARE_METATYPE(Transfer::Mode)
QScriptValue tmodeToScriptValue(QScriptEngine *engine, Transfer::Mode const &in)
{
	return engine->toScriptValue(int(in));
}
void tmodeFromScriptValue(const QScriptValue &object, Transfer::Mode &out)
{
	out = Transfer::Mode(object.toInt32());
}

Q_DECLARE_METATYPE(Transfer*)
QScriptValue transferToScriptValue(QScriptEngine *engine, Transfer* const &in)
{
	QScriptValue retval = engine->newQObject(in);
	retval.setProperty("speed", engine->newFunction(transferSpeedFunction));
	retval.setProperty("speedLimits", engine->newFunction(transferSpeedLimitsFunction));
	retval.setProperty("timeLeft", engine->newFunction(transferTimeLeftFunction));
	
	return retval;
}

void transferFromScriptValue(const QScriptValue &object, Transfer* &out)
{
	out = qobject_cast<Transfer*>(object.toQObject());
}

void HttpService::findTransfer(QString transferUUID, Queue** q, Transfer** t)
{
	*q = 0;
	*t = 0;
	
	g_queuesLock.lockForRead();
	for(int i=0;i<g_queues.size();i++)
	{
		Queue* c = g_queues[i];
		c->lock();
		
		for(int j=0;j<c->size();j++)
		{
			if(c->at(j)->uuid() == transferUUID)
			{
				*q = c;
				*t = c->at(j);
				return;
			}
		}
		
		c->unlock();
	}
	
	g_queuesLock.unlock();
}

void HttpService::initScriptEngine()
{
	QScriptValue fun;
	
	m_engine = new QScriptEngine(this);
	
	fun = m_engine->newFunction(formatSizeFunction);
	m_engine->globalObject().setProperty("formatSize", fun);
	
	fun = m_engine->newFunction(formatTimeFunction);
	m_engine->globalObject().setProperty("formatTime", fun);
	
	fun = m_engine->newFunction(listDirectoryFunction);
	m_engine->globalObject().setProperty("listDirectory", fun);
	
	fun = m_engine->newFunction(fileInfoFunction);
	m_engine->globalObject().setProperty("fileInfo", fun);
	
	fun = m_engine->newFunction(getSettingsValueFunction);
	m_engine->globalObject().setProperty("getSettingsValue", fun);
	
	fun = m_engine->newFunction(addTransfersFunction);
	m_engine->globalObject().setProperty("addTransfers", fun);
	
	fun = m_engine->newFunction(addQueueFunction);
	m_engine->globalObject().setProperty("addQueue", fun);
	
	QScriptValue engines = m_engine->newArray(g_enginesDownload.size());
	for(int i=0;i<g_enginesDownload.size();i++)
		engines.setProperty(i, m_engine->toScriptValue(QString(g_enginesDownload[i].longName)));
	m_engine->globalObject().setProperty("ENGINES", engines);
	
	//qScriptRegisterMetaType(m_engine, queueToScriptValue, queueFromScriptValue);
	qScriptRegisterMetaType(m_engine, transferToScriptValue, transferFromScriptValue);
	qScriptRegisterMetaType(m_engine, tmodeToScriptValue, tmodeFromScriptValue);
}

void HttpService::interpretScript(QFile* input, OutputBuffer* output, QByteArray queryString, QByteArray postData)
{
	QByteArray in;
	QScriptValue fun, fun2;
	
	g_queuesLock.lockForWrite();
	in = input->readAll();
	
	m_engine->pushContext();
	
	m_engine->globalObject().setProperty("GET", convertQueryString(processQueryString(queryString)));
	m_engine->globalObject().setProperty("POST", convertQueryString(processQueryString(postData)));
	m_engine->globalObject().setProperty("QUERY_STRING", m_engine->toScriptValue(QString(queryString)));
	
	fun = m_engine->newFunction(pagePrintFunction);
	fun.setData(m_engine->newQObject(output));
	m_engine->globalObject().setProperty("print", fun);
	
	QScriptValue queues = m_engine->newArray(g_queues.size());
	fun = m_engine->newFunction(queueSpeedLimitsFunction);
	fun2 = m_engine->newFunction(queueTransferLimitsFunction);
	
	for(int i=0;i<g_queues.size();i++)
	{
		QScriptValue val = m_engine->newQObject(g_queues[i]);
		val.setProperty("speedLimits", fun);
		val.setProperty("transferLimits", fun2);
		queues.setProperty(i, val);
	}
	m_engine->globalObject().setProperty("QUEUES", queues);
	
	int p = 0;
	while(true)
	{
		int e,next = in.indexOf("<?", p);
		int line;
		
		output->putData(in.constData()+p, (next>=0) ? next-p : in.size()-p);
		if(next < 0)
			break;
		
		line = countLines(in, next) + 1;
		next += 2;
		
		e = in.indexOf("?>", next);
		if(e < 0)
			break;
		p = e+2;
		
		m_engine->evaluate(in.mid(next, e-next), input->fileName(), line);
		if(m_engine->hasUncaughtException())
		{
			QByteArray errmsg = handleException();
			output->putData(errmsg.constData(), errmsg.size());
		}
	}
	
	queues = m_engine->globalObject().property("QUEUES");
	for(int i=g_queues.size();i>=0;i--)
	{
		QScriptValue v = queues.property(i);
		if(v.isNull() || v.isUndefined())
		{
			// destroy the queue
			delete g_queues.takeAt(i);
		}
	}
	g_queuesLock.unlock();
	
	m_engine->popContext();
}

QByteArray HttpService::handleException()
{
	QByteArray errmsg = "<div style=\"color: red; border: 1px solid red\"><p><b>QtScript runtime exception:</b> <code>";
	errmsg += m_engine->uncaughtException().toString().toUtf8();
	errmsg += "</code>";
	
	int line = m_engine->uncaughtExceptionLineNumber();
	if(line != -1)
	{
		errmsg += " on line ";
		errmsg += QByteArray::number(line);
	}
	
	errmsg += "</p><p>Backtrace:</p><pre>";
	errmsg += m_engine->uncaughtExceptionBacktrace().join("\n");
	errmsg += "</pre></div>";
	
	return errmsg;
}

int HttpService::countLines(const QByteArray& ar, int left)
{
	int lines = 0, p = 0;
	while(true)
	{
		p = ar.indexOf('\n', p+1);
		if(p > left || p < 0)
			break;
		lines++;
	}
	return lines;
}
