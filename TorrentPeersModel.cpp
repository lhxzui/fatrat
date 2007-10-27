#include "TorrentDownload.h"
#include "TorrentPeersModel.h"
#include "fatrat.h"
#include <QtDebug>

TorrentPeersModel::TorrentPeersModel(QObject* parent, TorrentDownload* d)
	: QAbstractListModel(parent), m_download(d), m_nLastRowCount(0)
{
	m_columns << tr("IP address") << tr("Country") << tr("Client");
	m_columns << tr("Seed?") << tr("Download") << tr("Upload");
	m_columns << tr("Downloaded") << tr("Uploaded") << tr("State");
}

QModelIndex TorrentPeersModel::index(int row, int column, const QModelIndex &parent) const
{
	if(!parent.isValid())
		return createIndex(row,column,(void*)this);
	else
		return QModelIndex();
}

QModelIndex TorrentPeersModel::parent(const QModelIndex &index) const
{
	return QModelIndex();
}

int TorrentPeersModel::rowCount(const QModelIndex &parent) const
{
	return m_nLastRowCount;
}

QVariant TorrentPeersModel::headerData(int section, Qt::Orientation orientation, int role) const
{
	if (orientation == Qt::Horizontal && role == Qt::DisplayRole)
		return m_columns[section];
	return QVariant();
}

QVariant TorrentPeersModel::data(const QModelIndex &index, int role) const
{
	if(index.row() >= m_peers.size())
		return QVariant();
	const libtorrent::peer_info& info = m_peers[index.row()];
		
	if(role == Qt::DisplayRole)
	{
		switch(index.column())
		{
			case 0:
			{
				std::string ip = info.ip.address().to_string();
				return QString(ip.c_str());
			}
			case 1:
				return QString(QByteArray(info.country, 2));
			case 2:
				return QString::fromUtf8(info.client.c_str());
			case 3:
				return (info.seed) ? QString("*") : QString();
			case 4:
				return formatSize(info.down_speed, true);
			case 5:
				return formatSize(info.up_speed, true);
			case 6:
				return formatSize(info.total_download);
			case 7:
				return formatSize(info.total_upload);
			case 8:
				if(info.flags & libtorrent::peer_info::connecting)
					return tr("Connecting");
				else if(info.flags & libtorrent::peer_info::handshake)
					return tr("Handshaking");
				else if(info.flags & libtorrent::peer_info::queued)
					return tr("Queued");
				else
				{
					QString text;
					if(info.flags & libtorrent::peer_info::interesting)
						text += "Interesting ";
					if(info.flags & libtorrent::peer_info::choked)
						text += "Chocked ";
					if(info.flags & libtorrent::peer_info::remote_interested)
						text += "Remote_interested ";
					if(info.flags & libtorrent::peer_info::remote_choked)
						text += "Remote_chocked ";
					if(info.connection_type == libtorrent::peer_info::web_seed)
						text += "WEB_SEED";
					return text;
				}
		}
	}
	else if(role == Qt::DecorationRole)
	{
		if(index.column() == 1)
		{
			if(isalpha(info.country[0]))
			{
				QString flag = QString(":/flags/%1%2.gif").arg((char) tolower(info.country[0])).arg((char) tolower(info.country[1]));
				
				return QIcon(flag);
			}
		}
	}
	
	return QVariant();
}

bool TorrentPeersModel::hasChildren(const QModelIndex & parent) const
{
	return !parent.isValid();
}

void TorrentPeersModel::refresh()
{
	int count = 0;
	
	if(m_download->m_handle.is_valid())
	{
		m_download->m_handle.get_peer_info(m_peers);
		count = m_peers.size();
	}
	
	if(count > m_nLastRowCount)
	{
		beginInsertRows(QModelIndex(), m_nLastRowCount, count);
		endInsertRows();
	}
	else if(count < m_nLastRowCount)
	{
		beginRemoveRows(QModelIndex(), count, m_nLastRowCount);
		endRemoveRows();
	}
	m_nLastRowCount = count;
	
	dataChanged(createIndex(0,0), createIndex(count, m_columns.size())); // refresh the view
}