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

#include "SettingsJabberForm.h"
#include "JabberService.h"
#include "Settings.h"
#include <QSettings>

extern QSettings* g_settings;

SettingsJabberForm::SettingsJabberForm(QWidget* w, QObject* parent)
	: QObject(parent)
{
	setupUi(w);
}

void SettingsJabberForm::load()
{
	checkEnable->setChecked(getSettingsValue("jabber/enabled").toBool());
	lineJID->setText(getSettingsValue("jabber/jid").toString());
	linePassword->setText(getSettingsValue("jabber/password").toString());
	lineResource->setText(getSettingsValue("jabber/resource").toString());
	checkRestrictSelf->setChecked(getSettingsValue("jabber/restrict_self").toBool());
	groupRestrictPassword->setChecked(getSettingsValue("jabber/restrict_password_bool").toBool());
	lineRestrictPassword->setText(getSettingsValue("jabber/restrict_password").toString());
	checkAutoAuth->setChecked(getSettingsValue("jabber/grant_auth").toBool());
	spinPriority->setValue(getSettingsValue("jabber/priority").toInt());
	
	QUuid uuid = getSettingsValue("jabber/proxy").toString();
	m_listProxy = Proxy::loadProxys();
	
	comboProxy->addItem(tr("None", "No proxy"));
	for(int i=0;i<m_listProxy.size();i++)
	{
		comboProxy->addItem(m_listProxy[i].toString());
		if(uuid == m_listProxy[i].uuid)
			comboProxy->setCurrentIndex(i+1);
	}
}

void SettingsJabberForm::accepted()
{
	g_settings->setValue("jabber/enabled", checkEnable->isChecked());
	g_settings->setValue("jabber/jid", lineJID->text());
	g_settings->setValue("jabber/password", linePassword->text());
	g_settings->setValue("jabber/restrict_self", checkRestrictSelf->isChecked());
	g_settings->setValue("jabber/restrict_password_bool", groupRestrictPassword->isChecked());
	g_settings->setValue("jabber/restrict_password", lineRestrictPassword->text());
	g_settings->setValue("jabber/priority", spinPriority->value());
	g_settings->setValue("jabber/resource", lineResource->text());
	g_settings->setValue("jabber/grant_auth", checkAutoAuth->isChecked());
	
	int index = comboProxy->currentIndex() - 1;
	g_settings->setValue("jabber/proxy", (index >= 0) ? m_listProxy[index].uuid.toString() : "");
	
	JabberService::instance()->applySettings();
}

