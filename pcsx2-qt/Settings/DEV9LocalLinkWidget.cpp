// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "DEV9LocalLinkWidget.h"
#include "SettingsWindow.h"

#include "common/MemorySettingsInterface.h"

#include <QtCore/QSignalBlocker>

#ifndef _WIN32
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#endif

namespace LocalLink = Catalog::LocalLink;

namespace
{
	QString problemText(LocalLink::Problem problem)
	{
		// Same wording as Big Picture, which reads the same rules.
		switch (problem)
		{
			case LocalLink::Problem::AddressTooLong:
				return QCoreApplication::translate("DEV9LocalLinkWidget", "That address is too long.");
			case LocalLink::Problem::PortNotANumber:
				return QCoreApplication::translate("DEV9LocalLinkWidget", "The port must be a number.");
			case LocalLink::Problem::PortOutOfRange:
				return QCoreApplication::translate("DEV9LocalLinkWidget", "The port must be between 1024 and 65535.");
			case LocalLink::Problem::PeerIdNotANumber:
				return QCoreApplication::translate("DEV9LocalLinkWidget", "The peer ID must be a number.");
			case LocalLink::Problem::PeerIdOutOfRange:
				return QCoreApplication::translate("DEV9LocalLinkWidget", "The peer ID must be between 2 and 65533. 1 is the host.");
			case LocalLink::Problem::RoomCodeLength:
				return QCoreApplication::translate("DEV9LocalLinkWidget", "The room code must be 4 to 12 letters or digits.");
			case LocalLink::Problem::None:
				break;
		}
		return QString();
	}

	QString localIPv4Addresses()
	{
		QStringList addresses;
#ifndef _WIN32
		ifaddrs* list = nullptr;
		if (getifaddrs(&list) == 0)
		{
			for (const ifaddrs* it = list; it; it = it->ifa_next)
			{
				if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET)
					continue;
				if (!(it->ifa_flags & IFF_UP) || (it->ifa_flags & IFF_LOOPBACK))
					continue;

				char buffer[INET_ADDRSTRLEN];
				const auto* address = reinterpret_cast<const sockaddr_in*>(it->ifa_addr);
				if (inet_ntop(AF_INET, &address->sin_addr, buffer, sizeof(buffer)))
					addresses.push_back(QString::fromLatin1(buffer));
			}
			freeifaddrs(list);
		}
#endif
		return addresses.join(QStringLiteral(", "));
	}
} // namespace

DEV9LocalLinkWidget::DEV9LocalLinkWidget(SettingsWindow* dialog, QWidget* parent)
	: QWidget(parent)
	, m_dialog(dialog)
{
	m_ui.setupUi(this);

	m_ui.localLinkPort->setRange(static_cast<int>(LocalLink::PORT_MIN), static_cast<int>(LocalLink::PORT_MAX));
	m_ui.localLinkLocalAddress->setPlaceholderText(tr("Not connected"));

	connect(m_ui.localLinkHost, &QCheckBox::toggled, this, &DEV9LocalLinkWidget::onHostToggled);
	connect(m_ui.localLinkAddress, &QLineEdit::editingFinished, this, &DEV9LocalLinkWidget::onAddressEdited);
	connect(m_ui.localLinkPort, &QSpinBox::valueChanged, this, &DEV9LocalLinkWidget::onPortEdited);
	connect(m_ui.localLinkRoomCode, &QLineEdit::editingFinished, this, &DEV9LocalLinkWidget::onRoomCodeEdited);
	connect(m_ui.localLinkGenerate, &QPushButton::clicked, this, &DEV9LocalLinkWidget::onGenerateClicked);
	connect(m_ui.localLinkPeerId, &QSpinBox::valueChanged, this, &DEV9LocalLinkWidget::onPeerIdEdited);

	m_dialog->registerWidgetHelp(m_ui.localLinkHost, tr("Host"), tr("Unchecked"),
		tr("Check this on the device that hosts the session and starts the game. Leave it unchecked to join a host."));
	m_dialog->registerWidgetHelp(m_ui.localLinkLocalAddress, tr("This Device's Address"), tr("N/A"),
		tr("Give one of these to the other players to enter as the host address on their device."));
	m_dialog->registerWidgetHelp(m_ui.localLinkAddress, tr("Host Address"), tr("N/A"),
		tr("The address shown on the host device, for example 192.168.1.42, or a VPN address such as 100.101.102.103. A host name also works."));
	m_dialog->registerWidgetHelp(m_ui.localLinkPort, tr("Port"), tr("19072"),
		tr("Use the same port on every device (1024-65535). There is no automatic negotiation."));
	m_dialog->registerWidgetHelp(m_ui.localLinkRoomCode, tr("Room Code"), tr("Generated"),
		tr("Use the same 4-12 letter or digit code on every device. It keeps two nearby sessions apart; it is not security, so use it on a network you trust. "
		   "Lower case is changed to upper case."));
	m_dialog->registerWidgetHelp(m_ui.localLinkGenerate, tr("Generate Room Code"), tr("N/A"),
		tr("Replaces the room code with a new random one. Give it to the other players."));
	m_dialog->registerWidgetHelp(m_ui.localLinkPeerId, tr("Peer ID"), tr("Automatic"),
		tr("Different on every device, because it decides the console's network address (2-65533). Set automatically; change it only if two devices share one. The host is always 1."));

	reload();
}

DEV9LocalLinkWidget::~DEV9LocalLinkWidget() = default;

template <typename Rule>
LocalLink::Problem DEV9LocalLinkWidget::change(Rule&& rule)
{
	const Values before = readValues();

	MemorySettingsInterface scratch;
	writeScratch(scratch, before);
	const LocalLink::Problem problem = rule(scratch);
	if (problem == LocalLink::Problem::None)
		store(before, readScratch(scratch));

	// Show what is stored now: a refused entry goes back to the stored text.
	showValues(readValues());
	showProblem(problem);
	return problem;
}

void DEV9LocalLinkWidget::onSelected()
{
	change([](SettingsInterface& si) {
		LocalLink::SeedDefaults(si);
		return LocalLink::Problem::None;
	});
}

void DEV9LocalLinkWidget::reload()
{
	showValues(readValues());
	showProblem(LocalLink::Problem::None);
}

void DEV9LocalLinkWidget::showEvent(QShowEvent* event)
{
	QWidget::showEvent(event);
	reload();
}

DEV9LocalLinkWidget::Values DEV9LocalLinkWidget::readValues() const
{
	Values values;
	values.host = m_dialog->getEffectiveBoolValue(LocalLink::SECTION, LocalLink::KEY_HOST, false);
	values.address = m_dialog->getEffectiveStringValue(LocalLink::SECTION, LocalLink::KEY_ADDRESS, "");
	values.port = static_cast<u32>(m_dialog->getEffectiveIntValue(LocalLink::SECTION, LocalLink::KEY_PORT, static_cast<int>(LocalLink::DEFAULT_PORT)));
	values.peer_id = static_cast<u32>(m_dialog->getEffectiveIntValue(LocalLink::SECTION, LocalLink::KEY_PEER_ID, static_cast<int>(LocalLink::HOST_PEER_ID)));
	values.room_code = m_dialog->getEffectiveStringValue(LocalLink::SECTION, LocalLink::KEY_ROOM_CODE, "");
	return values;
}

void DEV9LocalLinkWidget::writeScratch(MemorySettingsInterface& scratch, const Values& values)
{
	scratch.SetBoolValue(LocalLink::SECTION, LocalLink::KEY_HOST, values.host);
	scratch.SetStringValue(LocalLink::SECTION, LocalLink::KEY_ADDRESS, values.address.c_str());
	scratch.SetUIntValue(LocalLink::SECTION, LocalLink::KEY_PORT, values.port);
	scratch.SetUIntValue(LocalLink::SECTION, LocalLink::KEY_PEER_ID, values.peer_id);
	scratch.SetStringValue(LocalLink::SECTION, LocalLink::KEY_ROOM_CODE, values.room_code.c_str());
}

DEV9LocalLinkWidget::Values DEV9LocalLinkWidget::readScratch(const MemorySettingsInterface& scratch)
{
	Values values;
	values.host = scratch.GetBoolValue(LocalLink::SECTION, LocalLink::KEY_HOST, false);
	values.address = scratch.GetStringValue(LocalLink::SECTION, LocalLink::KEY_ADDRESS, "");
	values.port = scratch.GetUIntValue(LocalLink::SECTION, LocalLink::KEY_PORT, LocalLink::DEFAULT_PORT);
	values.peer_id = scratch.GetUIntValue(LocalLink::SECTION, LocalLink::KEY_PEER_ID, LocalLink::HOST_PEER_ID);
	values.room_code = scratch.GetStringValue(LocalLink::SECTION, LocalLink::KEY_ROOM_CODE, "");
	return values;
}

void DEV9LocalLinkWidget::store(const Values& before, const Values& after)
{
	if (after.host != before.host)
		m_dialog->setBoolSettingValue(LocalLink::SECTION, LocalLink::KEY_HOST, after.host);
	if (after.address != before.address)
		m_dialog->setStringSettingValue(LocalLink::SECTION, LocalLink::KEY_ADDRESS, after.address.c_str());
	if (after.port != before.port)
		m_dialog->setIntSettingValue(LocalLink::SECTION, LocalLink::KEY_PORT, static_cast<int>(after.port));
	if (after.peer_id != before.peer_id)
		m_dialog->setIntSettingValue(LocalLink::SECTION, LocalLink::KEY_PEER_ID, static_cast<int>(after.peer_id));
	if (after.room_code != before.room_code)
		m_dialog->setStringSettingValue(LocalLink::SECTION, LocalLink::KEY_ROOM_CODE, after.room_code.c_str());
}

void DEV9LocalLinkWidget::showValues(const Values& values)
{
	const QSignalBlocker block_host(m_ui.localLinkHost);
	const QSignalBlocker block_address(m_ui.localLinkAddress);
	const QSignalBlocker block_port(m_ui.localLinkPort);
	const QSignalBlocker block_room_code(m_ui.localLinkRoomCode);
	const QSignalBlocker block_peer_id(m_ui.localLinkPeerId);

	m_ui.localLinkHost->setChecked(values.host);
	m_ui.localLinkAddress->setText(QString::fromStdString(values.address));
	m_ui.localLinkPort->setValue(static_cast<int>(values.port));
	m_ui.localLinkRoomCode->setText(QString::fromStdString(values.room_code));

	if (values.host)
	{
		m_ui.localLinkPeerId->setRange(static_cast<int>(LocalLink::HOST_PEER_ID), static_cast<int>(LocalLink::HOST_PEER_ID));
		m_ui.localLinkPeerId->setValue(static_cast<int>(LocalLink::HOST_PEER_ID));
	}
	else
	{
		m_ui.localLinkPeerId->setRange(static_cast<int>(LocalLink::GUEST_PEER_ID_MIN), static_cast<int>(LocalLink::GUEST_PEER_ID_MAX));
		m_ui.localLinkPeerId->setValue(static_cast<int>(values.peer_id));
	}
	m_ui.localLinkPeerId->setEnabled(!values.host);

	if (values.host)
		m_ui.localLinkLocalAddress->setText(localIPv4Addresses());
	m_ui.localLinkForm->setRowVisible(m_ui.localLinkLocalAddress, values.host);
	m_ui.localLinkForm->setRowVisible(m_ui.localLinkAddress, !values.host);
}

void DEV9LocalLinkWidget::showProblem(LocalLink::Problem problem)
{
	const QString text = problemText(problem);
	m_ui.localLinkProblem->setVisible(!text.isEmpty());
	m_ui.localLinkProblem->setText(text.isEmpty() ? text : QStringLiteral("<b style=\"color:#d9534f\">%1</b>").arg(text.toHtmlEscaped()));
}

void DEV9LocalLinkWidget::onHostToggled(bool host)
{
	change([host](SettingsInterface& si) {
		si.SetBoolValue(LocalLink::SECTION, LocalLink::KEY_HOST, host);
		// A guest needs a peer ID in 2..65533 and the default is the host's 1.
		LocalLink::SeedDefaults(si);
		return LocalLink::Problem::None;
	});
}

void DEV9LocalLinkWidget::onAddressEdited()
{
	const std::string text = m_ui.localLinkAddress->text().toStdString();
	change([&text](SettingsInterface& si) { return LocalLink::Apply(si, LocalLink::Field::Address, text); });
}

void DEV9LocalLinkWidget::onPortEdited()
{
	const std::string text = std::to_string(m_ui.localLinkPort->value());
	change([&text](SettingsInterface& si) { return LocalLink::Apply(si, LocalLink::Field::Port, text); });
}

void DEV9LocalLinkWidget::onRoomCodeEdited()
{
	const std::string text = m_ui.localLinkRoomCode->text().toStdString();
	change([&text](SettingsInterface& si) { return LocalLink::Apply(si, LocalLink::Field::RoomCode, text); });
}

void DEV9LocalLinkWidget::onGenerateClicked()
{
	const std::string code = LocalLink::GenerateRoomCode();
	change([&code](SettingsInterface& si) { return LocalLink::Apply(si, LocalLink::Field::RoomCode, code); });
}

void DEV9LocalLinkWidget::onPeerIdEdited()
{
	const std::string text = std::to_string(m_ui.localLinkPeerId->value());
	change([&text](SettingsInterface& si) { return LocalLink::Apply(si, LocalLink::Field::PeerId, text); });
}

#include "moc_DEV9LocalLinkWidget.cpp"
