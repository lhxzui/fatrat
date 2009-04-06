/*
FatRat download manager
http://fatrat.dolezel.info

Copyright (C) 2006-2008 Lubos Dolezel <lubos a dolezel.info>

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
version 2 as published by the Free Software Foundation
with the OpenSSL special exemption.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#ifndef _FRQUEUE_H
#define _FRQUEUE_H
#include <QDomNode>
#include <QReadWriteLock>
#include <QList>
#include <QUuid>
#include "Transfer.h"

class Queue : public QObject
{
Q_OBJECT
public:
	Queue();
	~Queue();
	
	static void loadQueues();
	static void saveQueues();
	static void unloadQueues();
	
	Q_INVOKABLE void setSpeedLimits(int down,int up) { m_nDownLimit=down; m_nUpLimit=up; }
	void speedLimits(int& down, int& up) const { down=m_nDownLimit; up=m_nUpLimit; }
	
	Q_INVOKABLE void setTransferLimits(int down = -1,int up = -1) { m_nDownTransferLimit=down; m_nUpTransferLimit=up; }
	void transferLimits(int& down,int& up) const { down=m_nDownTransferLimit; up=m_nUpTransferLimit; }
	
	Q_INVOKABLE void setName(QString name);
	Q_INVOKABLE QString name() const;
	Q_PROPERTY(QString name READ name WRITE setName)
	
	Q_INVOKABLE void setDefaultDirectory(QString path);
	Q_INVOKABLE QString defaultDirectory() const;
	Q_PROPERTY(QString defaultDirectory READ defaultDirectory WRITE setDefaultDirectory)
	
	Q_INVOKABLE void setMoveDirectory(QString path);
	Q_INVOKABLE QString moveDirectory() const;
	Q_PROPERTY(QString moveDirectory READ moveDirectory WRITE setMoveDirectory)
	
	Q_INVOKABLE QString uuid() const { return m_uuid.toString(); }
	Q_PROPERTY(QString uuid READ uuid)
	
	Q_INVOKABLE bool upAsDown() const { return m_bUpAsDown; }
	Q_INVOKABLE void setUpAsDown(bool v) { m_bUpAsDown=v; }
	Q_PROPERTY(bool upAsDown READ upAsDown WRITE setUpAsDown)
	
	Q_INVOKABLE int size();
	Q_PROPERTY(int size READ size)
	
	void lock() { m_lock.lockForRead(); }
	void lockW() { m_lock.lockForWrite(); }
	void unlock() { m_lock.unlock(); }
	
	Q_INVOKABLE Transfer* at(int r);
	
	Q_INVOKABLE void add(Transfer* d);
	void add(QList<Transfer*> d);
	
	Q_INVOKABLE int moveDown(int n);
	Q_INVOKABLE int moveUp(int n);
	Q_INVOKABLE void moveToTop(int n);
	Q_INVOKABLE void moveToBottom(int n);
	Q_INVOKABLE void moveToPos(int from, int to);
	
	Q_INVOKABLE void remove(int n, bool nolock = false);
	Q_INVOKABLE void removeWithData(int n, bool nolock = false);
	Transfer* take(int n, bool nolock = false);
	
	void autoLimits(int& down, int& up) const { down=m_nDownAuto; up=m_nUpAuto; }
	void setAutoLimits(int down, int up);
	
	bool contains(Transfer* t) const;
private:
	void loadQueue(const QDomNode& node);
	void saveQueue(QDomNode& node,QDomDocument& doc);
	
	QString m_strName, m_strDefaultDirectory, m_strMoveDirectory;
	int m_nDownLimit,m_nUpLimit,m_nDownTransferLimit,m_nUpTransferLimit;
	int m_nDownAuto, m_nUpAuto;
	bool m_bUpAsDown;
	QUuid m_uuid;
	mutable QReadWriteLock m_lock;
public:
	// statistics
	struct Stats
	{
		int active_d, waiting_d, active_u, waiting_u;
		int down, up;
	} m_stats;
protected:
	QList<Transfer*> m_transfers;
	
	friend class QueueMgr;
};

#endif
